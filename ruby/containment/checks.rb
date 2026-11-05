# The rows of stunt's catalogue (aether contrib/host/stunt), each done the
# way a Ruby script would do it, and the judge that turns what happened into
# stunt's one line:
#
#   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
#
# The host (host.ae, or child_host.ae for the contained child) prepends a few
# facts it found before the sandbox was in force (HOST_HOME_SET, HOST_SH,
# LIBM_PATH, LIBC_NAME, LIBC_PATH), appends a call of check_line(name,
# expectation) and reads the line back. A refusal is what a sandbox gives:
# EPERM or EACCES from libc, the preload library or the kernel, or a host
# function's error that starts "refused:". Anything else that goes wrong is
# an error, never a refusal, so it cannot pass for a sandbox at work.
#
# The libraries required here are required by the host before the sandbox is
# in force (host.ae), or by the worker before it locks itself down
# (ruby/worker): Ruby reads a library from disk the first time it is
# required, so under grants with no read of its standard library a feature
# that is not loaded yet cannot be (ruby/README.md). Kept for reading.
require 'rbconfig'
require 'socket'
require 'fiddle'
require 'json'

FIXTURE = "./stunt-fixture.txt"
WRITTEN = "./stunt-written.txt"
REFUSALS = [Errno::EPERM::Errno, Errno::EACCES::Errno]   # the same numbers on Linux, macOS, FreeBSD

def by_errno(code, what)
  return ["refused", what] if REFUSALS.include?(code)
  ["error", what]
end

def first_line(s)
  s.to_s.split("\n", 2)[0].to_s[0, 40]
end

# File.read: Ruby's way to a file, through libc's open.
def open_path(path)
  s = File.read(path)
  ["succeeded", "read \"#{first_line(s)}\""]
rescue SystemCallError => e
  by_errno(e.errno, "open: #{e.message}")
end

def write_file
  File.write(WRITTEN, "written by ruby\n")
  ["succeeded", "wrote #{WRITTEN}"]
rescue SystemCallError => e
  by_errno(e.errno, "open: #{e.message}")
end

# libc's own getenv, reached through Fiddle (RTLD_DEFAULT, the process's own
# symbol scope). Ruby's ENV is not this: ENV['X'] is a getenv call too, but
# through the interpreter (env_cached below).
def libc_getenv
  libc = Fiddle.dlopen(nil)
  fn = Fiddle::Function.new(libc["getenv"], [Fiddle::TYPE_VOIDP], Fiddle::TYPE_VOIDP)
  v = fn.call("HOME")
  return ["succeeded", "HOME=#{v.to_s}"] unless v.null?
  return ["refused", "libc getenv gave NULL; HOME is in the environment"] if HOST_HOME_SET
  ["error", "HOME is unset"]
rescue Fiddle::DLError => e
  dlopen_refusal(e, "Fiddle.dlopen(nil)")
end

# ENV: stunt's env-cached is the engine's copy of the environment, taken at
# start (Python's os.environ, Perl's %ENV). Ruby keeps no copy: ENV reads
# libc's environment each time, so this row is the script reading its
# environment the ordinary way. A missing HOME is a refusal only when the
# host had one.
def env_cached
  v = ENV["HOME"]
  return ["succeeded", "HOME=#{v} from ENV"] unless v.nil?
  return ["refused", "HOME is not in ENV; the host has it"] if HOST_HOME_SET
  ["error", "HOME was never in the environment"]
end

# A non-blocking connect, judged as stunt judges it: EACCES at connect (the
# preload, Landlock) or EPERM/EACCES at socket (seccomp) is a refusal; EPERM
# at connect is a firewall's answer, an error; anything else (EINPROGRESS, a
# network error) means the call was let through.
def connect_to(family, addr)
  begin
    s = Socket.new(family, Socket::SOCK_STREAM, 0)
  rescue SystemCallError => e
    return by_errno(e.errno, "socket: #{e.message}")
  end
  begin
    s.connect_nonblock(Socket.sockaddr_in(443, addr))
    ["succeeded", "connected"]
  rescue SystemCallError => e
    case e.errno
    when Errno::EACCES::Errno then ["refused", "connect: #{e.message}"]
    when Errno::EPERM::Errno then ["error", "connect: #{e.message} (a firewall's answer, not the sandbox's)"]
    else ["succeeded", "let through: #{e.message}"]
    end
  ensure
    s.close
  end
end

# Starting /bin/sh, both ways Ruby has: Kernel#system with an argument list
# (fork and execve, no shell in between) and backticks (a command with no
# metacharacters is executed directly too). Succeeded when either started a
# shell: an escape by one way is an escape. Refused when both were refused: an
# EPERM or EACCES raised, or the shell could not be executed (exit 127) while
# HOST_SH says /bin/sh is there.
def exec_sh
  ways = []
  begin
    system("/bin/sh", "-c", "exit 0", exception: true)
    ways << ["succeeded", "system: exit 0"]
  rescue SystemCallError => e
    ways << by_errno(e.errno, "system: #{e.message}")
  rescue RuntimeError => e
    st = $?.exitstatus
    ways << (st == 127 && HOST_SH ? ["refused", "system: the shell could not be executed (exit 127)"] : ["error", "system: #{e.message}"])
  end
  begin
    `/bin/sh -c exit`
    st = $?.exitstatus
    if st == 0 then ways << ["succeeded", "backticks: exit 0"]
    elsif st == 127 && HOST_SH then ways << ["refused", "backticks: the shell could not be executed (exit 127)"]
    else ways << ["error", "backticks: exit #{st}"]
    end
  rescue SystemCallError => e
    ways << by_errno(e.errno, "backticks: #{e.message}")
  end
  detail = ways.map { |_, d| d }.join("; ")
  kinds = ways.map { |k, _| k }
  return ["succeeded", detail] if kinds.include?("succeeded")
  return ["refused", detail] if kinds == ["refused", "refused"]
  ["error", detail]
end

# Process.fork, the child exiting at once: 0 succeeded, anything else an error.
def fork_once
  pid = fork
  exit!(0) if pid.nil?
  Process.wait(pid)
  st = $?
  return ["succeeded", "the forked child ran"] if st.exited? && st.exitstatus == 0
  ["error", "the forked child: status #{st.to_i}"]
rescue SystemCallError => e
  by_errno(e.errno, "fork: #{e.message}")
end

# Fiddle.dlopen is a dlopen. A dlopen that fails on its own leaves a
# dlerror() message, which Fiddle raises as DLError. The preload's refusal
# returns NULL without one, and Fiddle (1.1.x, in Ruby 3.4) raises only when
# there is a message: it hands back a Handle holding NULL instead, so a
# refusal is a Handle whose address is 0 (or, from a Fiddle that does raise,
# a "Permission denied" or empty message).
def dlopen_refusal(e, what)
  msg = e.message.to_s
  if msg == "" || msg == "(null)" || msg =~ /Permission denied|Operation not permitted/
    return ["refused", "#{what}: #{msg.empty? ? 'no dlerror() message' : msg} (the preload's refusal)"]
  end
  ["error", "#{what}: #{msg}"]
end

def load_lib(path)
  h = Fiddle.dlopen(path)
  return ["refused", "Fiddle.dlopen: a Handle holding NULL, no error raised (the preload's refusal)"] if h.to_i == 0
  ["succeeded", "loaded #{path}"]
rescue Fiddle::DLError => e
  dlopen_refusal(e, "Fiddle.dlopen")
end

def need(name)
  v = Object.const_defined?(name) ? Object.const_get(name) : nil
  raise "the host found no #{name}" if v.nil?
  v
end

# Linux's system call numbers, for the two architectures stunt has raw
# syscalls on.
SYS = { "x86_64" => { "openat" => 257, "socket" => 41 }, "aarch64" => { "openat" => 56, "socket" => 198 } }
AT_FDCWD = -100
ARCH = RbConfig::CONFIG["host_cpu"].to_s.sub(/^arm64$/, "aarch64")

# libc's syscall(): Ruby's Kernel#syscall, what stunt calls syscall-fn. Only
# EPERM or EACCES is a refusal (ENOSYS is the preload's syscall() with no
# grants in force, which is an error).
def syscall_fn_openat
  nr = (SYS[ARCH] || {})["openat"]
  return ["error", "no syscall numbers for #{ARCH}"] if nr.nil?
  fd = syscall(nr, AT_FDCWD, "/etc/passwd", File::RDONLY, 0)
  IO.for_fd(fd).close
  ["succeeded", "opened /etc/passwd"]
rescue SystemCallError => e
  by_errno(e.errno, "syscall(): #{e.message}")
end

# The syscall instruction itself, past libc and so past any preload: a few
# bytes of machine code in an executable mapping, called through Fiddle.
# raw(nr, a, b, c, d) moves its arguments into the system call registers and
# traps; the result is the kernel's, -errno on failure.
#
# The mapping is of a memfd, not anonymous memory: the preload refuses an
# anonymous executable mmap ("mmap_exec_anonymous") and any mprotect to
# PROT_EXEC, but lets an executable mapping of a file descriptor through (so
# the loader and JITs can map code), and a memfd is one. So the script gets
# its system call instruction, and only the kernel (Landlock, seccomp) is left
# to hold what it does.
RAW_CODE = {
  # mov rax,rdi; mov rdi,rsi; mov rsi,rdx; mov rdx,rcx; mov r10,r8; syscall; ret
  "x86_64" => ["4889f8" "4889f7" "4889d6" "4889ca" "4d89c2" "0f05" "c3"].pack("H*"),
  # mov x8,x0; mov x0,x1; mov x1,x2; mov x2,x3; mov x3,x4; svc #0; ret
  "aarch64" => ["e80300aa" "e00301aa" "e10302aa" "e20303aa" "e30304aa" "010000d4" "c0035fd6"].pack("H*"),
}

def raw_syscall
  return $raw_fn if $raw_fn
  code = RAW_CODE[ARCH]
  raise "no raw syscall for #{ARCH}" if code.nil?
  libc = Fiddle.dlopen(nil)
  t = Fiddle
  memfd_create = t::Function.new(libc["memfd_create"], [t::TYPE_VOIDP, t::TYPE_INT], t::TYPE_INT)
  ftruncate = t::Function.new(libc["ftruncate"], [t::TYPE_INT, t::TYPE_LONG], t::TYPE_INT)
  mmap = t::Function.new(libc["mmap"], [t::TYPE_VOIDP, t::TYPE_SIZE_T, t::TYPE_INT, t::TYPE_INT, t::TYPE_INT, t::TYPE_LONG], t::TYPE_VOIDP)
  fd = memfd_create.call("raw", 0)
  raise "memfd_create failed" if fd < 0
  ftruncate.call(fd, 4096)
  addr = mmap.call(0, 4096, 7, 1, fd, 0).to_i   # PROT_READ|WRITE|EXEC, MAP_SHARED
  raise "mmap of the memfd failed" if addr == 0 || addr == (2**64 - 1)
  t::Pointer.new(addr)[0, code.bytesize] = code
  $raw_fn = t::Function.new(addr, [t::TYPE_LONG] * 5, t::TYPE_LONG)
end

def raw_result(r, what)
  if r >= 0
    IO.for_fd(r).close
    return ["succeeded", what]
  end
  by_errno(-r, "the syscall instruction: #{SystemCallError.new(-r).message}")
end

def syscall_openat
  fn = raw_syscall
  path = Fiddle::Pointer.malloc(16)
  path[0, 12] = "/etc/passwd\0"
  r = fn.call(SYS[ARCH]["openat"], AT_FDCWD, path.to_i, File::RDONLY, 0)
  raw_result(r, "the syscall instruction opened /etc/passwd")
end

def syscall_socket
  fn = raw_syscall
  r = fn.call(SYS[ARCH]["socket"], Socket::AF_INET, Socket::SOCK_STREAM, 0, 0)
  raw_result(r, "the syscall instruction made an AF_INET socket")
end

# A host function's answer. In-process (host.ae) a refusal is the
# RuntimeError the host raised ("refused: ..."); in the contained child a file
# comes as std.guest's answer to the call, {"granted" => true, "text" => ...}
# or {"granted" => false, "why" => ...}, and a FAIL is a RuntimeError too.
def host_str(fn, arg)
  v = send(fn, arg)
  if v.is_a?(Hash)
    return ["refused", "refused: #{v['why']}"] unless v["granted"]
    v = v["text"]
  end
  return ["error", "#{fn} answered nil"] if v.nil?
  ["succeeded", "#{fn}(#{arg}) = \"#{first_line(v)}\""]
rescue NoMethodError
  ["error", "no host function #{fn}"]
rescue RuntimeError => e
  why = e.message.to_s
  return ["refused", why] if why.start_with?("refused:")
  ["error", why]
end

def callback_greet
  v = greet("stunt")
  return ["error", "greet(\"stunt\") = #{v.inspect}"] unless v == "hello, stunt"
  ["succeeded", 'greet("stunt") = "hello, stunt"']
end

def callback_add
  v = add(2, 40)
  return ["error", "add(2, 40) = #{v.inspect}"] unless v == 42
  ["succeeded", "add(2, 40) = 42"]
end

# A host function nobody registered is an undefined method in Ruby: calling it
# is a NoMethodError (a NameError) the script can rescue, not a crash.
def callback_unregistered
  nope()
  ["succeeded", "nope() answered"]
rescue NameError => e
  ["refused", "no host function nope (#{e.class}: #{e.message.lines.first.to_s.chomp})"]
end

# The shared map, through the bridge's aether_map_get / aether_map_put (the
# host's own map in the child).
def map_get
  v = aether_map_get("input")
  return ["error", "input is not in the map"] if v.nil?
  return ["error", "input = \"#{v}\""] unless v == "stunt-in"
  ["succeeded", 'input = "stunt-in"']
end

def map_set
  aether_map_put("output", "from-ruby")
  v = aether_map_get("output")
  return ["error", "output read back as #{v.inspect}"] unless v == "from-ruby"
  ["succeeded", 'output = "from-ruby"']
end

# aether_map_put answers nil whether or not it wrote, so a refused write is
# seen only by reading the key back: still the host's.
def map_overwrite_input
  aether_map_put("input", "overwritten")
  v = aether_map_get("input")
  return ["refused", 'input is still "stunt-in" (read back)'] if v == "stunt-in"
  ["succeeded", "input read back as #{v.inspect}"]
end

# Not stunt's: a feature required under the sandbox (tests in
# ruby/containment/test_requires.sh). Ruby reads a library from disk the first
# time it is required, so under a policy with no read grant on its standard
# library the require is refused (a LoadError, or the EACCES of the open);
# one already loaded is not read again.
def require_feature(f)
  already = $LOADED_FEATURES.any? { |p| p.end_with?("/#{f}.rb") || p.end_with?("/#{f}.so") }
  require f
  ["succeeded", "require #{f}#{already ? ' (already loaded)' : ''}"]
rescue SystemCallError => e
  by_errno(e.errno, "require #{f}: #{e.message}")
rescue LoadError => e
  # Ruby finds a library by looking through $LOAD_PATH (stat, which the
  # preload holds like an open), so a library it may not read is "cannot load
  # such file", not EACCES. The tests that use this pair it with the same
  # require succeeding once the directory is granted, which is what shows the
  # library was there.
  ["refused", "require #{f}: #{e.class}: #{e.message} (the load path could not be read)"]
end

CHECKS = {
  "open-etc-passwd" => lambda { open_path("/etc/passwd") },
  "open-granted-file" => lambda { open_path(FIXTURE) },
  "write-granted-file" => lambda { write_file },
  "getenv-home" => lambda { libc_getenv },
  "env-cached" => lambda { env_cached },
  "connect-192.0.2.1" => lambda { connect_to(Socket::AF_INET, "192.0.2.1") },
  "connect-ipv4-mapped" => lambda { connect_to(Socket::AF_INET6, "::ffff:192.0.2.1") },
  "exec-bin-sh" => lambda { exec_sh },
  "fork" => lambda { fork_once },
  "dlopen-libm" => lambda { load_lib(need(:LIBM_PATH)) },
  "dlopen-libc-by-name" => lambda { load_lib(need(:LIBC_NAME)) },
  "dlopen-libc-by-path" => lambda { load_lib(need(:LIBC_PATH)) },
  "syscall-fn-openat" => lambda { syscall_fn_openat },
  "syscall-openat" => lambda { syscall_openat },
  "syscall-socket" => lambda { syscall_socket },
  "binding-read-etc-passwd" => lambda { host_str(:read_file, "/etc/passwd") },
  "binding-read-granted-file" => lambda { host_str(:read_file, FIXTURE) },
  "binding-getenv-home" => lambda { host_str(:getenv, "HOME") },
  "callback-greet" => lambda { callback_greet },
  "callback-add" => lambda { callback_add },
  "callback-unregistered" => lambda { callback_unregistered },
  "map-get" => lambda { map_get },
  "map-set" => lambda { map_set },
  "map-overwrite-input" => lambda { map_overwrite_input },
}

def check_line(name, expect)
  fn = CHECKS[name]
  fn = lambda { require_feature(name[8..-1]) } if fn.nil? && name.start_with?("require:")
  return "#{name}: not expressed in Ruby - expected #{expect}: FAIL" if fn.nil?
  begin
    outcome, detail = fn.call
  rescue Exception => e
    outcome, detail = "error", "#{e.class}: #{e.message}"
  end
  # "error" is for a row the harness knows fails for a reason of its own
  # (tests/stunt_lib.sh's preload_fails), so the day it stops failing the row
  # goes red.
  passed = (expect == "succeed" && outcome == "succeeded") || (expect == "refused" && outcome == "refused") ||
           (expect == "error" && outcome == "error")
  "#{name}: #{outcome} (#{detail}) - expected #{expect}: #{passed ? 'PASS' : 'FAIL'}"
end

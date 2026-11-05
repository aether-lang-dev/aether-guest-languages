# The rows of stunt's catalogue (aether contrib/host/stunt), each done the
# way a Python script would do it, and the judge that turns what happened
# into stunt's one line:
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
# The modules imported here are imported by the host before the sandbox is
# in force (host.ae), or by the worker before it locks itself down
# (python/worker): under a policy with no read grant on the standard
# library, a module not yet imported cannot be (python/README.md).

import ctypes
import errno
import mmap
import os
import socket
import subprocess
import sys

FIXTURE = "./stunt-fixture.txt"
WRITTEN = "./stunt-written.txt"
REFUSALS = (errno.EPERM, errno.EACCES)   # the same numbers on Linux, macOS, FreeBSD


def by_errno(code, what):
    if code in REFUSALS:
        return "refused", what
    return "error", what


def first_line(s):
    return s.split("\n", 1)[0][:40]


# open(), then read: Python's way to a file, through libc's open.
def open_path(path):
    try:
        with open(path) as f:
            s = f.read()
    except OSError as e:
        return by_errno(e.errno, "open: %s" % e)
    return "succeeded", 'read "%s"' % first_line(s)


def write_file():
    try:
        with open(WRITTEN, "w") as f:
            f.write("written by python\n")
    except OSError as e:
        return by_errno(e.errno, "open: %s" % e)
    return "succeeded", "wrote " + WRITTEN


# Python reaches libc's own getenv only through ctypes: os.getenv and
# os.environ read the copy taken at start (env-cached). CDLL(None) is the
# process's own symbol scope, the way ctypes' documentation reaches libc.
def libc_getenv():
    try:
        libc = ctypes.CDLL(None)
        libc.getenv.restype = ctypes.c_char_p
        libc.getenv.argtypes = [ctypes.c_char_p]
        v = libc.getenv(b"HOME")
    except OSError as e:
        return dlopen_refusal(e, "ctypes.CDLL(None)")
    if v is not None:
        return "succeeded", "HOME=" + v.decode()
    if HOST_HOME_SET:
        return "refused", "libc getenv gave NULL; HOME is in the environment"
    return "error", "HOME is unset"


# os.environ: the copy of the environment CPython took when it started,
# before any sandbox. A missing HOME is a refusal only when the host had
# one: then it was kept from this process (the contained child is given no
# environment).
def env_cached():
    v = os.environ.get("HOME")
    if v is not None:
        return "succeeded", "HOME=%s from os.environ" % v
    if HOST_HOME_SET:
        return "refused", "HOME is not in os.environ; the host has it"
    return "error", "HOME was never in the environment"


# A non-blocking connect, judged as stunt judges it: EACCES at connect
# (the preload, Landlock) or EPERM/EACCES at socket (seccomp) is a refusal;
# EPERM at connect is a firewall's answer, an error; anything else
# (EINPROGRESS, a network error) means the call was let through.
def connect_to(family, addr):
    try:
        s = socket.socket(family, socket.SOCK_STREAM)
    except OSError as e:
        return by_errno(e.errno, "socket: %s" % e)
    try:
        s.setblocking(False)
        r = s.connect_ex((addr, 443))
    finally:
        s.close()
    if r == 0:
        return "succeeded", "connected"
    if r == errno.EACCES:
        return "refused", "connect: %s" % os.strerror(r)
    if r == errno.EPERM:
        return "error", "connect: %s (a firewall's answer, not the sandbox's)" % os.strerror(r)
    return "succeeded", "let through: %s" % os.strerror(r)


# Starting /bin/sh, both ways Python has: os.system (libc's system(), which
# glibc and macOS start the shell with posix_spawn), and subprocess (fork or
# vfork, then execve, in _posixsubprocess). Succeeded when either started a
# shell: an escape by one way is an escape. Refused when both were refused:
# subprocess raises EPERM or EACCES, or the shell could not be executed
# (exit 127) while HOST_SH says /bin/sh is there; os.system returns -1 when
# system() could not start it, or the shell's 127.
def exec_sh():
    ways = []
    st = os.system("exit 0")
    if st == 0:
        ways.append(("succeeded", "os.system: exit 0"))
    elif st == -1:
        ways.append(("refused", "os.system: system() returned -1"))
    elif os.WIFEXITED(st) and os.WEXITSTATUS(st) == 127 and HOST_SH:
        ways.append(("refused", "os.system: the shell could not be executed (exit 127)"))
    else:
        ways.append(("error", "os.system: status %d" % st))
    try:
        r = subprocess.run(["/bin/sh", "-c", "exit 0"])
        if r.returncode == 0:
            ways.append(("succeeded", "subprocess: exit 0"))
        elif r.returncode == 127 and HOST_SH:
            ways.append(("refused", "subprocess: the shell could not be executed (exit 127)"))
        else:
            ways.append(("error", "subprocess: exit %d" % r.returncode))
    except OSError as e:
        ways.append(by_errno(e.errno, "subprocess: %s" % e))
    detail = "; ".join(d for _, d in ways)
    kinds = [k for k, _ in ways]
    if "succeeded" in kinds:
        return "succeeded", detail
    if kinds == ["refused", "refused"]:
        return "refused", detail
    return "error", detail


# os.fork, the child exiting at once: 0 succeeded, anything else an error.
def fork_once():
    try:
        pid = os.fork()
    except OSError as e:
        return by_errno(e.errno, "os.fork: %s" % e)
    if pid == 0:
        os._exit(0)
    _, st = os.waitpid(pid, 0)
    if os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0:
        return "succeeded", "the forked child ran"
    return "error", "the forked child: status %d" % st


# ctypes.CDLL is a dlopen. A dlopen that fails on its own leaves a dlerror()
# message, which ctypes raises as OSError; the preload's refusal returns
# NULL without one, and ctypes then says only "dlopen() error" (3.10 to
# 3.14). That, or a "Permission denied" or "Operation not permitted" in the
# text, is the refusal.
def dlopen_refusal(e, what):
    msg = str(e)
    if msg in ("dlopen() error", "") or "Permission denied" in msg or "Operation not permitted" in msg:
        return "refused", "%s: %s (the preload's refusal)" % (what, msg or "no dlerror() message")
    return "error", "%s: %s" % (what, msg)


def load(path):
    try:
        ctypes.CDLL(path)
    except OSError as e:
        return dlopen_refusal(e, "ctypes.CDLL")
    return "succeeded", "loaded " + path


def need(name):
    v = globals().get(name)
    if v is None:
        raise LookupError("the host found no %s" % name)
    return v


# Linux's system call numbers, for the two architectures stunt has raw
# syscalls on.
SYS = {"x86_64": {"openat": 257, "socket": 41}, "aarch64": {"openat": 56, "socket": 198}}
AT_FDCWD = -100


# libc's syscall(), through ctypes: what stunt calls syscall-fn. Only EPERM
# or EACCES is a refusal (ENOSYS is the preload's syscall() with no grants
# in force, which is an error).
def syscall_fn_openat():
    nr = SYS.get(os.uname().machine, {}).get("openat")
    if nr is None:
        return "error", "no syscall numbers for " + os.uname().machine
    try:
        libc = ctypes.CDLL(None, use_errno=True)
    except OSError as e:
        return dlopen_refusal(e, "ctypes.CDLL(None)")
    libc.syscall.restype = ctypes.c_long
    r = libc.syscall(ctypes.c_long(nr), ctypes.c_long(AT_FDCWD), ctypes.c_char_p(b"/etc/passwd"),
                     ctypes.c_long(os.O_RDONLY), ctypes.c_long(0))
    if r >= 0:
        os.close(r)
        return "succeeded", "opened /etc/passwd"
    return by_errno(ctypes.get_errno(), "syscall(): " + os.strerror(ctypes.get_errno()))


# The syscall instruction itself, past libc and so past any preload: a few
# bytes of machine code in an executable mapping, called through ctypes.
# raw(nr, a, b, c, d) moves its arguments into the system call registers and
# traps; the result is the kernel's, -errno on failure.
#
# The mapping is of a memfd, not anonymous memory: the preload refuses an
# anonymous executable mmap ("mmap_exec_anonymous") and any mprotect to
# PROT_EXEC, but lets an executable mapping of a file descriptor through
# (so the loader and JITs can map code), and a memfd is one. So the script
# gets its system call instruction, and only the kernel (Landlock, seccomp)
# is left to hold what it does.
RAW_CODE = {
    # mov rax,rdi; mov rdi,rsi; mov rsi,rdx; mov rdx,rcx; mov r10,r8; syscall; ret
    "x86_64": bytes.fromhex("4889f8" "4889f7" "4889d6" "4889ca" "4d89c2" "0f05" "c3"),
    # mov x8,x0; mov x0,x1; mov x1,x2; mov x2,x3; mov x3,x4; svc #0; ret
    "aarch64": bytes.fromhex("e80300aa" "e00301aa" "e10302aa" "e20303aa" "e30304aa" "010000d4" "c0035fd6"),
}
_raw = []


def raw_syscall():
    if _raw:
        return _raw[0]
    code = RAW_CODE.get(os.uname().machine)
    if code is None:
        raise LookupError("no raw syscall for " + os.uname().machine)
    fd = os.memfd_create("raw")
    try:
        os.ftruncate(fd, mmap.PAGESIZE)
        m = mmap.mmap(fd, mmap.PAGESIZE, flags=mmap.MAP_SHARED,
                      prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    finally:
        os.close(fd)
    m.write(code)
    addr = ctypes.addressof(ctypes.c_char.from_buffer(m))
    fn = ctypes.CFUNCTYPE(ctypes.c_long, ctypes.c_long, ctypes.c_long, ctypes.c_long,
                          ctypes.c_long, ctypes.c_long)(addr)
    _raw.append((fn, m))
    return _raw[0]


def raw_result(r, what):
    if r >= 0:
        os.close(r)
        return "succeeded", what
    return by_errno(-r, "the syscall instruction: " + os.strerror(-r))


def syscall_openat():
    fn, _ = raw_syscall()
    path = ctypes.c_char_p(b"/etc/passwd")
    nr = SYS[os.uname().machine]["openat"]
    r = fn(nr, AT_FDCWD, ctypes.cast(path, ctypes.c_void_p).value, os.O_RDONLY, 0)
    return raw_result(r, "the syscall instruction opened /etc/passwd")


def syscall_socket():
    fn, _ = raw_syscall()
    nr = SYS[os.uname().machine]["socket"]
    r = fn(nr, socket.AF_INET, socket.SOCK_STREAM, 0, 0)
    return raw_result(r, "the syscall instruction made an AF_INET socket")


# A host function's answer. In-process (host.ae) a refusal is the
# RuntimeError the host raised ("refused: ..."); in the contained child a
# file comes as std.guest's answer to the call, {"granted": True, "text":
# ...} or {"granted": False, "why": ...}, and a FAIL is a RuntimeError too.
def host_str(fn, arg):
    f = globals().get(fn)
    if not callable(f):
        return "error", "no host function " + fn
    try:
        v = f(arg)
    except RuntimeError as e:
        why = str(e)
        if why.startswith("refused:"):
            return "refused", why
        return "error", why
    if isinstance(v, dict):
        if not v.get("granted"):
            return "refused", "refused: " + str(v.get("why"))
        v = v.get("text")
    if v is None:
        return "error", fn + " answered None"
    return "succeeded", '%s(%s) = "%s"' % (fn, arg, first_line(v))


def callback_greet():
    v = greet("stunt")
    if v != "hello, stunt":
        return "error", 'greet("stunt") = %r' % v
    return "succeeded", 'greet("stunt") = "hello, stunt"'


def callback_add():
    v = add(2, 40)
    if v != 42:
        return "error", "add(2, 40) = %r" % v
    return "succeeded", "add(2, 40) = 42"


# A host function nobody registered is an unbound name in Python: calling it
# is a NameError the script can catch, not a crash.
def callback_unregistered():
    try:
        nope()
    except NameError as e:
        return "refused", "no host function nope (NameError: %s)" % e
    return "succeeded", "nope() answered"


# The shared map, through the bridge's aether_map_get / aether_map_put (the
# host's own map in the child).
def map_get():
    v = aether_map_get("input")
    if v is None:
        return "error", "input is not in the map"
    if v != "stunt-in":
        return "error", 'input = "%s"' % v
    return "succeeded", 'input = "stunt-in"'


def map_set():
    aether_map_put("output", "from-python")
    v = aether_map_get("output")
    if v != "from-python":
        return "error", "output read back as %r" % v
    return "succeeded", 'output = "from-python"'


# In-process aether_map_put returns None whether or not it wrote, so a
# refused write is seen only by reading the key back: still the host's.
def map_overwrite_input():
    aether_map_put("input", "overwritten")
    v = aether_map_get("input")
    if v == "stunt-in":
        return "refused", 'input is still "stunt-in" (read back)'
    return "succeeded", "input read back as %r" % v


# Not stunt's: a module imported under the sandbox (tests in
# python/containment/test_imports.sh). CPython reads a module from disk the
# first time it is imported, so under a policy with no read grant on the
# standard library the import is refused (PermissionError from the open of
# its source); one already in sys.modules is not read again.
def import_module(mod):
    already = mod in sys.modules
    try:
        __import__(mod)
    except PermissionError as e:
        return "refused", "import %s: %s" % (mod, e)
    except ImportError as e:
        return "error", "import %s: %s: %s" % (mod, type(e).__name__, e)
    return "succeeded", "import %s%s" % (mod, " (already imported)" if already else "")


CHECKS = {
    "open-etc-passwd": lambda: open_path("/etc/passwd"),
    "open-granted-file": lambda: open_path(FIXTURE),
    "write-granted-file": write_file,
    "getenv-home": libc_getenv,
    "env-cached": env_cached,
    "connect-192.0.2.1": lambda: connect_to(socket.AF_INET, "192.0.2.1"),
    "connect-ipv4-mapped": lambda: connect_to(socket.AF_INET6, "::ffff:192.0.2.1"),
    "exec-bin-sh": exec_sh,
    "fork": fork_once,
    "dlopen-libm": lambda: load(need("LIBM_PATH")),
    "dlopen-libc-by-name": lambda: load(need("LIBC_NAME")),
    "dlopen-libc-by-path": lambda: load(need("LIBC_PATH")),
    "syscall-fn-openat": syscall_fn_openat,
    "syscall-openat": syscall_openat,
    "syscall-socket": syscall_socket,
    "binding-read-etc-passwd": lambda: host_str("read_file", "/etc/passwd"),
    "binding-read-granted-file": lambda: host_str("read_file", FIXTURE),
    "binding-getenv-home": lambda: host_str("getenv", "HOME"),
    "callback-greet": callback_greet,
    "callback-add": callback_add,
    "callback-unregistered": callback_unregistered,
    "map-get": map_get,
    "map-set": map_set,
    "map-overwrite-input": map_overwrite_input,
}


def check_line(name, expect):
    fn = CHECKS.get(name)
    if fn is None and name.startswith("import:"):
        fn = lambda: import_module(name[len("import:"):])
    if fn is None:
        return "%s: not expressed in Python - expected %s: FAIL" % (name, expect)
    try:
        outcome, detail = fn()
    except Exception as e:
        outcome, detail = "error", "%s: %s" % (type(e).__name__, e)
    # "error" is for a row the harness knows fails for a reason of its own
    # (tests/stunt_lib.sh's preload_fails), so the day it stops failing the
    # row goes red.
    passed = (expect == "succeed" and outcome == "succeeded") or (expect == "refused" and outcome == "refused") \
        or (expect == "error" and outcome == "error")
    return "%s: %s (%s) - expected %s: %s" % (name, outcome, detail, expect, "PASS" if passed else "FAIL")

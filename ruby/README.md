# ruby: embedded CRuby

`import ruby` lets an Aether program run Ruby. It is the bridge that was
aether's `contrib.host.ruby`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `ruby.run`, `ruby.run_sandboxed`,
`ruby.run_sandboxed_with_map`, `ruby.init_host` and `ruby.finalize_host`,
and a few more for host functions and typed values. It loads the machine's
libruby at run time and needs no Ruby dev package to build. Checked on
CRuby 2.6 (macOS's own) and 3.4; Ubuntu's 3.2 and Homebrew's current Ruby
are run in CI.

## Four ways to run Ruby from Aether

From least to most contained. The first three run Ruby inside your program,
and the fourth runs it in a separate, locked-down process. All four stay
supported: pick by how far you trust the script, not by which is newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Ruby runs | your process | your process | your process | its own process (`ruby/worker`) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Ruby's own file, socket, process and `Fiddle` calls | not checked | **not checked**, except `ENV`, which the bridge empties of what the grants do not name | checked against the grants (Linux) | refused, beyond what the host hands over |
| Requiring a library not loaded yet | yes | yes | only with a read grant on Ruby's library directories | only what the worker required before its lockdown |
| Platforms | everywhere libruby loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0; `system` and backticks held from 0.884.0 | ae 0.878.0 (`std.guest`) |

### 1. Embed: Ruby in your process, no containment

```aether
import ruby

main() {
    script = <<RB
        require 'json'
        data = { "a" => [1, 2, 3], 'b' => "it's" }
        puts JSON.generate(data)
        puts 'C:\no\escapes', "tab:\t|"
    RB
    if ruby.run(script) != 0 { println("ruby failed") }
    ruby.finalize_host()
}
```

A heredoc (`<<RB ... RB`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape processing,
so Ruby's quotes, `#{}` and backslashes need no escaping (`'C:\no\escapes'`
reaches Ruby as written, and the `"\t"` is Ruby's), and the indentation the
lines share is stripped. The closing marker sits on a line of its own. A
plain string works too: `ruby.run("puts 'hi'")`.

It prints

```
{"a":[1,2,3],"b":"it's"}
C:\no\escapes
tab:	|
```

`ruby.finalize_host()` at the end runs `at_exit` handlers and tears the
interpreter down; the bridge makes `$stdout` unbuffered at start, so what the
script printed is there without it. (aether's copy did not: an Aether program
that exited without finalizing lost its `puts` output.) A failed script makes
`run` return -1 and prints `[ruby] <message>` on stderr.

For a script that calls back into the host, or returns a typed value, give it
host functions: `ruby.register`, `ruby.eval`
([Host functions](#host-functions-and-typed-values) below). That is still this
first way: in-process and uncontained.

### 2. Embed inside a sandbox block

```aether
import ruby
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    script = <<RB
        puts ENV["HOME"]
        puts ENV["USER"].inspect
    RB
    if ruby.run_sandboxed(reader, script) != 0 { println("ruby failed") }
    ruby.finalize_host()
}
```

This prints `/home/paul` and `nil`. `run_sandboxed` pushes the block's grants
as a level for the length of the call. Aether's own std calls and every host
function the script reaches are checked against them, on every platform.
Ruby's `ENV` is hidden by the bridge itself: for the length of the run the
variables the grants do not name are removed from the process's environment
(Ruby's `ENV` and libc's `getenv` are the same thing), and put back when it
ends. Ruby's own `File`, `Socket`, `system`, backticks, `fork` and `Fiddle`
go straight to libc, so on their own they are **not** checked: the same
script could read `/etc/passwd`, connect anywhere or run a shell just as well.
Use this way for scripts you trust, where the grants describe what the host
lends them.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits between
Ruby and libc and holds Ruby's own calls to the same grants (Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

A script granted only `HOME`, run on CachyOS (Ruby 3.4) under ae 0.889.0,
first without the preload and then with it:

| Ruby does | without | with `LD_PRELOAD` |
|---|---|---|
| `ENV["HOME"]` | `/home/paul` | `/home/paul` (granted) |
| `ENV["USER"]` | `nil` (hidden by the bridge) | `nil` |
| `getenv("USER")` through `Fiddle` | `NULL` (hidden) | `NULL` |
| `File.read("/etc/passwd")` | reads it | `Errno::EACCES` |
| `system("/bin/sh", "-c", "true")` | runs `/bin/sh` | `Errno::EPERM` |
| `Socket` connect to 192.0.2.1:443 | goes out (nobody answers there) | `EACCES` |
| `require "securerandom"` (not required yet) | loads | `LoadError`: cannot load such file |

(The script is [in the tests' words](containment/checks.rb): each line is a
rescue around the call.) Two things in it are Ruby's own:

- **`ENV` is held without the preload.** Unlike Python's `os.environ` or
  Perl's `%ENV`, Ruby's `ENV` is not a copy taken at start: it asks libc each
  time. So there is no row where it escapes, and the bridge's removal of the
  ungranted variables makes it, and `getenv` through `Fiddle`, hold on macOS
  too. The cost is that the environment really changes for the length of the
  run: another thread of the program that calls `getenv` meanwhile sees it
  too.
- **Requiring needs Ruby's library.** Ruby reads a library from disk the
  first time it is required, and finds it by looking through the load path,
  which fails under the preload as an open does, so under grants with no read of
  Ruby's library directories, `require 'securerandom'` is a `LoadError`
  unless it was loaded before the call. Require what a script needs before
  `run_sandboxed` (`ruby.run("require 'socket'")`), or grant the directories:
  `grant_fs_read("/usr/lib/ruby/*")` (`RbConfig::CONFIG["rubylibdir"]` and
  `["rubyarchdir"]`).

It is a library interposer, not a kernel boundary: a raw system call made
without libc is not seen, and macOS has no equivalent. A script can make one
(`checks.rb`'s `syscall-*` rows do, with a few bytes of machine code mapped
from a memfd with `Fiddle`, which the preload's refusal of anonymous
executable memory does not cover); only the contained child's kernel layer
holds those. `system` and backticks are held from ae 0.884.0 (aether #3580);
before that they escaped it. The containment table below has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("ruby") {
        grant_fs_read("/usr/lib/ruby/*")        // Ruby reads its libraries from disk to start and to require
    }
    args = list.new()
    list.add(args, "/usr/lib/libruby.so.3.4.12")
    list.add(args, "json")                      // libraries to require before the lockdown
    g, err = guest.launch(perms, "build/ruby-worker", args, "ruby", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    script = <<RB
        puts "#{greet("ruby")} #{JSON.generate([1, 2])}"
        File.read("/etc/passwd")
    RB
    guest.start(&g, script)
    while true {
        e = guest.next(&g)
        if e.kind == "SAY" {
            println(guest.printable(e.text))
        } else if e.kind == "CALL" {
            guest.answer(&g, guest.json_string("hello"))
        } else {
            println("${e.kind} ${guest.printable(e.text)}")
            break
        }
    }
}
```

On CachyOS this prints `hello [1,2]`, then
`ERROR Errno::EACCES: Permission denied @ rb_sysopen - /etc/passwd`.

Ruby runs in `ruby/worker`, a separate process `guest.launch` starts under
Landlock and seccomp from the policy, with the preload, and talking to the
host only over a socket (`std.ipc` frames). The worker loads libruby, starts
the interpreter and requires the libraries it is told to, then locks itself
down with no grants before the script starts. So the script reaches a file
only when the host hands it a descriptor, an environment variable only
through a host function, and a library only if it was required before the
lockdown. The host can cancel a script at a deadline and kills a worker that
stops answering. On macOS, where nothing can contain the child,
`guest.launch` refuses and starts nothing. Build the worker once with
`ae build ruby/worker/worker.ae -o ruby-worker`; details under
[In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as Ruby comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | matches | from ae 0.884.0: differs: exec-bin-sh stricter, fork stricter; before: differs: fork stricter |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-etc-passwd not held, write-granted-file not held | matches | differs: open-granted-file stricter, getenv-home stricter, env-cached stricter |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | from ae 0.884.0: matches; before: differs: exec-bin-sh escapes | differs: fork stricter |
| `test_stunt_host_checked` | `-` | matches | matches | matches |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | differs: dlopen-libc-by-name stricter, dlopen-libc-by-path stricter |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | matches | differs: connect-192.0.2.1 stricter, connect-ipv4-mapped stricter |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | matches | matches |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held | from ae 0.884.0: matches; before: differs: exec-bin-sh escapes | matches |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | differs: write-granted-file stricter |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Ruby 2.6.10
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Ruby 3.4.12
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Ruby 3.4.12
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Ruby 3.4.12
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Ruby 3.4.12
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt` at
`AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's with
a read grant on Ruby's library directories added, below); the containment
table below has each row. The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it say
which host produced which column. FreeBSD's preload is not column 3's (see
the containment table), and was not tried for Ruby.

## Containment table

What a Ruby script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Ruby way
([containment/checks.rb](containment/checks.rb)), under the policies of
aether's `tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`).
"Per policy" means granted where the policy grants it and refused where it
does not, as stunt expects. The columns are the ways above: "in-process,
Linux (preload)" is way 3, "in-process, macOS" is way 2 where there is no
preload, and "contained child" is way 4. Every cell is a test outcome
(`../tests/run.sh`): checked on macOS arm64 (system Ruby 2.6.10), CachyOS
x86_64 with Landlock (Ruby 3.4.12; ae 0.889.0 and 0.878.0), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`File.read`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`File.read`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`File.write`) | per policy | **not held** | refused under every policy |
| `getenv-home` (libc's `getenv`, through `Fiddle`) | per policy | per policy: the bridge empties the environment of what the grants do not name | refused under every policy: the worker has no environment; `getenv` is a host function |
| `env-cached` (`ENV["HOME"]`) | per policy (Ruby keeps no copy: `ENV` is `getenv`) | per policy, as above | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1`, `connect-ipv4-mapped` (`Socket#connect_nonblock`) | per policy | **not held** | refused under every policy: the worker's own lockdown has no tcp grant |
| `exec-bin-sh` (`Kernel#system` and backticks) | per policy, from ae 0.884.0; before it **escapes**: both start `/bin/sh` with no exec grant | **not held** | refused under every policy, from ae 0.884.0: the worker's lockdown has no exec grant; before it, per policy (seccomp) |
| `fork` (`Kernel#fork`) | (child-only row) | (child-only row) | refused under every policy: the worker's lockdown has no fork grant |
| `dlopen-libm` (`Fiddle.dlopen`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`Fiddle.dlopen`) | (child-only row) | (child-only row) | refused under every policy, `native=*` included (see below) |
| `syscall-fn-openat` (`Kernel#syscall`) | (child-only row) | (child-only row) | refused |
| `syscall-openat`, `syscall-socket` (the syscall instruction, from machine code in a memfd mapping, through `Fiddle`) | (child-only row) | (child-only row) | per policy: Landlock refuses the open, seccomp lets `socket(AF_INET)` through only with a tcp grant |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: a `NoMethodError` (a `NameError`) the script can rescue | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `spin` (`loop do ... end`) | stopped by the instruction limit | same | cancelled at the deadline, from the worker's trace hook |
| `silent` (disabling the hook through `ObjectSpace`, then a loop) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `puts`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `half-frame` (`IO#syswrite` of a frame's header and part of its payload to the worker's own socket, `AETHER_IPC_FD`) | (child-only row) | (child-only row) | killed: the host times out inside the frame |

None is left out: everything stunt does, Ruby's standard library can. One row
means something else in Ruby: `env-cached` is stunt's "the engine's copy of
the environment, taken at start" (Python's `os.environ`, Perl's `%ENV`), and
Ruby has no such copy, so the row is the script reading `ENV` the ordinary
way, which is `getenv` and is held wherever `getenv-home` is.

What the table says:

- **In-process is the easy default and is not containment on its own.** On
  Linux the preload holds Ruby's file, socket, `Fiddle` and libc `getenv`
  calls, and from ae 0.884.0 `system` and backticks too. Before 0.884.0 both
  escaped it: `system` starts `/bin/sh` with a `fork` and an `execve` that
  the preload's wrappers did not hold. On macOS nothing holds Ruby's own file,
  socket and process calls at all; only `ENV` and `getenv` are held, by the
  bridge's own removal of the ungranted variables. The host functions
  (`binding-*`), which check the grants themselves, hold everywhere.
- **The contained child holds every row it runs**, on Linux, and is stricter
  than the policy: the worker locks itself down with no grants before the
  script starts, so a script reaches a granted file only through the host and
  never gets a handle on libc, a socket, a fork or (from ae 0.884.0) a shell.
  The kernel layer from the policy is what holds the rows that go past libc
  (`syscall-openat`, `syscall-socket`); they follow the policy. Before 0.884.0
  process start followed the policy too (seccomp lets `execve` through only
  with an exec grant).
- **`Fiddle.dlopen` of libc does not raise when the preload refuses it.** The
  preload answers with `NULL` and no `dlerror()` message, and Fiddle 1.1 (Ruby
  3.4) raises only when there is a message, so what the script gets is a
  `Fiddle::Handle` holding `NULL`: `handle.to_i == 0`. A script that does not
  check goes on to a `Fiddle::Function` on a null handle. `checks.rb` calls
  that a refusal. (Ruby's `DLError` is raised for a refusal that has a
  message, and the check takes that too.)
- **The contained child's policy has one grant more than stunt's**: a read of
  Ruby's library directories (`RbConfig::CONFIG` `rubylibdir`, `rubyarchdir`
  and the site and vendor directories), which `../tests/stunt_lib.sh` passes
  as `GHL_RUBY_STDLIB` and `containment/child_host.ae` adds to every policy.
  The worker requires `rbconfig`, `socket`, `fiddle` and `json` before its
  lockdown, which reads them from there. Any host of `ruby-worker` has to
  give it. No row reads there.
- The tests hold these cells, including the gaps: a **not held** row runs
  expecting the call to succeed, so the day the preload holds it that test
  fails and this table is brought up to date. The `exec-bin-sh` row is checked
  on either side of ae 0.884.0 (`AE_SPAWN_PIN` in `ci/versions.env`), from the
  ae the tests run on.

### Requiring under a sandbox

Not a row of stunt's, but what decides whether a script runs at all: Ruby
reads a library from disk the first time it is required.
[containment/test_requires.sh](containment/test_requires.sh) holds it:

- in-process under the preload with no grants, `require 'securerandom'` (not
  loaded yet) is a `LoadError`; without the preload (macOS) it loads;
- in-process with a read grant on Ruby's library directories, it loads;
- in the contained child, under any policy, `require 'securerandom'` (which
  the worker did not require before its lockdown) fails, and `require
  'socket'` (which it did) works.

## Loading libruby

The bridge's C (`aether_host_ruby.c`) is compiled into the program that
imports `ruby` (`@source` in `module.ae`); there is no archive to build first
and no `aether.toml` entry to add. libruby is not linked: the bridge
`dlopen`s it at the first call (with `RTLD_GLOBAL`, so the extension modules
Ruby loads later, `socket`, `fiddle` and `json` among them, resolve their
`rb_*` symbols against the same library), and a program built on one machine
runs against whatever Ruby the machine it runs on has.

Where the bridge looks for libruby, in order:

1. `${AETHER_RUBY_SONAME}`: an exact soname or path
   (`/usr/lib/libruby.so.3.4.12` on Arch, `libruby-3.2.so.3.2` on Ubuntu
   24.04, `/opt/homebrew/opt/ruby/lib/libruby.dylib` or
   `/System/Library/Frameworks/Ruby.framework/Versions/2.6/usr/lib/libruby.2.6.dylib`
   on macOS);
2. `libruby.so`, the unversioned name, where the system has one (Arch,
   Fedora and Debian's ruby-dev do).

There is no list of versions to try: the program that starts yours owns that
choice. `../tests/ruby_env.sh` is one way to make it, from a `ruby`'s
`RbConfig` (on Linux this line is enough):

```sh
AETHER_RUBY_SONAME=$(ruby -rrbconfig -e 'puts File.join(RbConfig::CONFIG["libdir"], RbConfig::CONFIG["LIBRUBY_SO"])')
```

It needs a Ruby built with a shared libruby (`--enable-shared`), which
distributions' and Homebrew's are; the standalone builds that statically link
Ruby into the `ruby` binary have none to load.

## Lifecycle: what Ruby asks of an embedder

- **One interpreter per process, set up once.** `ruby_setup` cannot run twice:
  `ruby.finalize_host()` tears the interpreter down for good, and
  `ruby.init_host()` after it returns -1 with a message (aether's copy
  returned 0, and the next `ruby.run` segfaulted). Call `finalize_host` once,
  at shutdown, or not at all; to reset script state, do it in Ruby.
  `init_host` is idempotent while the interpreter is live.
- **One thread.** libruby treats the thread that set it up as its main
  thread. The bridge remembers it, and a call from any other (an actor's
  scheduler thread, say) returns -1 with a message instead of crashing in the
  VM. Start Ruby on the thread that will use it, which for an Aether program
  is `main`.
- **The GC scans the C stack.** Ruby is told where the stack starts with
  `ruby_init_stack`, which the `RUBY_INIT_STACK` macro calls with the address
  of a local. That address bounds what the collector scans, so a local in the
  frame that happened to make the first call is too low for a later call from
  a shallower frame: the `VALUE`s on the stack there would not be scanned. The
  bridge passes the top of the calling thread's stack instead
  (`pthread_getattr_np`, `pthread_get_stackaddr_np`), so a call from any depth
  is covered; if the system will not say, it falls back to a local, and then
  the first call should be made early, from `main`. A host function's
  `VALUE`s (arguments and the value it pushes) live only on that stack until
  they are returned, and `test_host_ruby.ae` runs thousands of calls with
  `GC.start` and `GC.stress` between them.
- **Nothing raises through an Aether frame.** A Ruby exception is a `longjmp`;
  one out of a host function would skip its `defer`s. A host function reports
  an error with `ruby.raise_error`, which raises after the function has
  returned, and the conversions a callback can ask for that may raise (an
  object's `to_s`, a bignum to 64 bits) run under `rb_protect` and give `""` or
  0 instead.
- **`nil`, `true` and `false` are asked of the interpreter.** They are
  compile-time constants in `ruby.h`, not exported symbols, and they differ:
  `Qnil` is 0x08 from Ruby 2.0 to 3.3 and 0x04 from 3.4, with other values
  without flonum. `ruby_abi.h` declares none of them; the bridge evaluates
  `nil`, `true` and `false` once at start and keeps what comes back (aether's
  copy baked in 0x08 and crashed on 3.4, #1618). Types are told apart with
  `rb_obj_class`, never by reading a `VALUE`'s bits.
- **`ruby_options` after `ruby_setup`.** `ruby_setup` alone leaves out the
  methods Ruby 3.3 and later write in Ruby (`Integer#times`, `Kernel#Integer`,
  `GC.start`): they raise `NoMethodError`. The bridge runs `ruby_options` with
  `--disable-gems --disable-rubyopt -e nil`, which is what `ruby`'s own `main`
  does, minus the script. So **RubyGems is not loaded** (it is most of Ruby's
  start-up, and it reads gem directories) and **`$RUBYOPT` is ignored**. A
  script can `require 'rubygems'`; `AETHER_RUBY_GEMS=1` in the environment
  loads it at start, which is how a bundled gem (fiddle, from Ruby 3.5) is
  found. The tests set it when the Ruby they are given needs it.
- **Signal handlers.** Ruby installs handlers (`SIGINT`, `SIGSEGV`, `SIGPIPE`,
  ...) and a timer thread at setup; the Aether program inherits them.

## In a contained child

`ruby/worker/worker.ae` is a std.guest worker. It takes the libruby soname as
its first argument, since it is given no environment, and a comma-separated
list of libraries to require as its second; it loads libruby, starts the
interpreter and requires them before it locks itself down. In the script,
`puts`, `print`, `p` and `$stdout.write` are `SAY` (`$stdout` is replaced by
an object that says rather than writes), and each function the host mounts
is a method of the same name whose call is a `CALL` the host answers: the
arguments go as JSON, the answer comes back decoded (a granted file is
`{"granted" => true, "text" => ...}`), a `FAIL` raises `RuntimeError(message)`
and a `CANCEL` raises `RuntimeError("cancelled by host: ...")`. A `TracePoint`
hook looks for a `CANCEL` every 1000 line, call and block events. A script
that disables it (`ObjectSpace.each_object(TracePoint)` finds it) stops
answering, and the host kills it after its grace period: the containment is
the host's, not the hook's. [containment/child_host.ae](containment/child_host.ae)
is a complete host.

Whatever the policy, the worker's process can read Ruby's library
directories (it must, to start), and its environment holds std.ipc's
`AETHER_IPC_FD` and `AETHER_IPC_KIND` and the `AETHER_RUBY_SONAME` it set
itself, which is how a script finds its own socket (the `half-frame` row).
Ruby also loads some things lazily that the worker cannot, once locked down:
a library not required first (`require 'timeout'`: `LoadError`), and the
converters and encodings that live in `enc/*.so` (`"x".encode("UTF-16")` is
`Encoding::ConverterNotFoundError`, and `"x".encode("Shift_JIS")` warns
"failed to load encoding" and gives ASCII-8BIT). Require or touch what the
script needs before the lockdown, by naming it in the second argument; the
built-in encodings (UTF-8, ASCII, the ISO-8859 ones) need nothing. A
`Thread.new` works.

## Host functions and typed values

`run_sandboxed` runs a script and reports success. A host that uses Ruby as an
extension language needs more: host functions the script can call, and typed
values back. These are the Python bridge's two-way VM without the VM handle
(Ruby has one interpreter per process, so a function is global and `ctx` is
the call):

```aether
import ruby

@c_callback
app_add(ctx: ptr) -> int {
    a = ruby.arg_int(ctx, 1)
    b = ruby.arg_int(ctx, 2)
    ruby.push_int(ctx, a + b)
    return 1
}

main() {
    ruby.register("add", app_add)
    ruby.eval("add(2, 3) * 7")
    println(ruby.result_str())      // 35
    ruby.finalize_host()
}
```

| Call | Effect |
|------|--------|
| `ruby.register(name, fn) -> int` | registers an Aether `@c_callback (ctx: ptr) -> int` as a private method of `Object` called `name` (letters, digits and `_`); 0, or -1 |
| `ruby.eval(code) -> int` | evaluates `code` and keeps its result: 0 and `result_str()` is the value's `to_s`, or -1 and it is `"<Class>: <message>"`. Prints nothing |
| `ruby.result_str() -> string` | the last `eval`'s result |
| `ruby.set_instruction_limit(budget) -> string` | bounds each outermost `eval` to `budget` events; 0 disarms; `""` when set |
| `ruby.arg_count/_type/_str/_int/_double/_bool(ctx, i)` | **inside a callback**: the arguments, 1-based |
| `ruby.push_int/_str/_bool/_nil/_double(ctx, v)` | **inside a callback**: the return value (the last push wins; return 1 to return it, 0 for `nil`) |
| `ruby.raise_error(ctx, msg) -> int` | **inside a callback**: `return ruby.raise_error(ctx, msg)`; the script sees `RuntimeError(msg)` after the callback returns, so its defers run |

The tags are `RB_TNIL`, `RB_TBOOL`, `RB_TINT`, `RB_TSTR`, `RB_TOTHER` and
`RB_TFLOAT`, numbered as the Lua, Python and Duktape bridges'. How values read:

- **`true` and `false` are not integers**, and `arg_bool` reads 1 only for
  `true`.
- **Numbers read either way.** `arg_double` of an integer is that number;
  `arg_int` of a float truncates. An integer past 64 bits is `RB_TINT` and
  reads 0.
- **Text.** `arg_str` is a String's bytes (to the first NUL) or, for any other
  type, its `to_s`; `nil` is `""`. `push_str` makes a UTF-8 String.
- **Errors.** A failed `eval` returns -1; `result_str` is `"<Class>: <message>"`
  and the error indicator is cleared.
- A host function is called on whatever thread the script runs on, but
  `ruby.eval` and `ruby.register` from a callback running on a Ruby `Thread`
  (a different native thread from the one that started Ruby) return -1: see
  Lifecycle.

### The instruction limit

`set_instruction_limit` installs a `TracePoint` (made in C with
`rb_tracepoint_new`) on line, method call, C call and block call events for
the outermost `eval`; a callback's nested `eval` neither resets the budget nor
removes the hook. Once the budget is spent the hook raises
`RuntimeError("instruction limit exceeded")` on **every** later event, so a
script that rescues it is stopped again at its handler's next event.

It bounds Ruby code only. It does not bound:

- a loop that makes no event at all: `while true; end`, or `x += 1` on one
  line (`Integer#+` is an instruction, not a call, and a line is counted when
  the line changes);
- a loop inside C that calls nothing, `sleep`, a blocking read;
- a script that finds the hook with `ObjectSpace.each_object(TracePoint)` and
  disables it, or threads it starts.

The contained child (way 4) is the answer to those: its host cancels at a
deadline and kills a worker that does not answer.

## Implementation notes

- All Ruby C API access goes through a `dlsym` function-pointer table: see
  `aether_host_ruby.c`. The bridge has no unresolved `rb_*` symbols, and needs
  none of Ruby's headers: the two types it uses are in `ruby_abi.h`, which
  says why they are the same from 2.6 to 3.4. The exported data it reads
  (`rb_cInteger`, `rb_eRuntimeError` ...) are looked up with `dlsym` too, which
  gives the address of the variable.
- `rb_intern` and `rb_funcall` are macros in `ruby.h`; the table's fields are
  `f_rb_intern` and `f_rb_funcall` so a later `#include <ruby.h>` could not
  rewrite them (aether's copy had the same trap, as the Python bridge had
  with `Py_None`).
- Host functions are one C method, `__aether_dispatch(name, *args)`, and a
  `def name(*a); __aether_dispatch('name', *a); end` per registered name; the
  `VALUE`s of one call are in a frame on the C stack, and a string argument
  is copied out of Ruby for the callback and freed when it returns.
- `aether_map_get` and `aether_map_put` (in `run_sandboxed_with_map`) are C
  methods over the runtime's map by token; the token is saved and restored
  around a nested run.
- A Ruby script's `puts` goes to the process's stdout directly (the bridge
  only makes it unbuffered), so it interleaves with Aether's `println` in the
  order they ran.

## Tests

- [`test_host_ruby.ae`](test_host_ruby.ae): the fire-and-forget surface
  through `import ruby` (a script evaluates, the interpreter really runs,
  runtime and syntax errors are failures, `init` is idempotent, a failure
  leaves the next eval clean), the host functions (typed arguments and
  results, errors raised after the callback's defers, nested evals, the
  collector between thousands of calls), sandboxed runs (the environment
  hidden and restored, a grant that tries to inject Ruby, another thread
  refused), the shared map (aether's `test_shared_map_all.sh` case for Ruby)
  and finalize-then-init refused. The first five cases are aether's, moved
  unchanged.
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh` and
  `containment/test_requires.sh`: the containment table and the parity table
  above.

The spec skips when libruby cannot be loaded (on GitHub Actions a skip is a
failure). It asserts on return codes and Ruby-side state, never on the
interpreter's stdout. `../tests/run.sh` runs everything.

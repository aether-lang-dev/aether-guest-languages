# perl: embedded Perl

`import perl` lets an Aether program run Perl. It is the bridge that was
aether's `contrib.host.perl`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `perl.run`, `perl.run_sandboxed`,
`perl.run_sandboxed_with_map`, `perl.init` and `perl.finalize`, and a few
more (host functions, [below](#host-functions-and-the-shared-map)). It loads
the machine's libperl at run time and needs no perl dev package to build.
Tested with Perl 5.34 and 5.44, threaded builds (`useithreads`, which every
system perl on Linux and macOS is).

## Four ways to run Perl from Aether

From least to most contained. The first three run Perl inside your program,
and the fourth runs it in a separate, locked-down process. All four stay
supported: pick by how far you trust the script, not by which is newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Perl runs | your process | your process | your process | its own process (`perl/worker`) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| `%ENV` | the host's | only what the grants grant | only what the grants grant | none: the worker has no environment |
| Perl's own file, socket, process and `DynaLoader` calls | not checked | **not checked** | checked against the grants (Linux) | refused, beyond what the host hands over |
| Loading a module not loaded yet | yes | yes | only with a read grant on perl's library directories | only what the worker loaded before its lockdown |
| Platforms | everywhere libperl loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0; `system` and backticks held from 0.884.0 | ae 0.878.0 (`std.guest`) |

### 1. Embed: Perl in your process, no containment

```aether
import perl

main() {
    script = <<PL
        use List::Util qw(sum);
        use JSON::PP;
        my %data = (a => [1, 2, 3], b => "it's");
        print JSON::PP->new->canonical->encode(\%data), "\n";
        print 'C:\no\escapes', " tab:\t|\n";
        print "sum: ", sum(1 .. 10), "\n";
    PL
    if perl.run(script) != 0 { println("perl failed") }
    perl.finalize()
}
```

This prints `{"a":[1,2,3],"b":"it's"}`, `C:\no\escapes tab:` and a tab,
`|`, and `sum: 55`.

A heredoc (`<<PL ... PL`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape
processing, so Perl's `$`, `@`, quotes and backslashes need no escaping
(`\%data` reaches Perl as written, and Perl makes the tab), and the
indentation the lines share is stripped. The closing marker sits on a line
of its own. A plain string works too: `perl.run("print qq{hi\n}")`.

`perl.finalize()` at the end flushes what the script printed: the embedded
interpreter buffers its own stdout, and an Aether program that exits
without finalizing may lose it. XS modules (`List::Util`, `POSIX`,
`Socket`) load: the bridge installs DynaLoader's native half, which an
interpreter started without an `xs_init` would lack.

A failed script returns -1 and prints `[perl] <$@>` to stderr;
`perl.last_error()` has the message.

### 2. Embed inside a sandbox block

```aether
import perl
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    script = <<PL
        print "HOME=$ENV{HOME}\n";
        print "USER=", (defined $ENV{USER} ? $ENV{USER} : "(not granted)"), "\n";
    PL
    if perl.run_sandboxed(reader, script) != 0 { println("perl failed") }
    perl.finalize()
}
```

This prints `HOME=/home/you` and `USER=(not granted)`: `run_sandboxed`
pushes the block's grants as a level for the length of the call, and before
the script runs the bridge deletes from `%ENV` what the grants do not grant
(and puts it back after, so the host's own environment is whole again).
Perl keeps `%ENV` as a copy taken at start, so no libc interposer sees a
script read it; this is what holds it. Aether's own std calls and every
host function the script reaches are checked against the grants too, on
every platform. Perl's own `open`, `socket`, `system`, backticks and
`DynaLoader` go straight to libc, so on their own they are **not**
checked: the same script could read `/etc/passwd`, connect anywhere or run
a shell just as well. Use this way for scripts you trust, where the grants
describe what the host lends them.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits between
Perl and libc and holds Perl's own calls to the same grants (Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

A script granted only `HOME`, run on CachyOS (Perl 5.44) under ae 0.889.0,
first without the preload and then with it:

| Perl does | without | with `LD_PRELOAD` |
|---|---|---|
| `$ENV{HOME}` | `/home/paul` | `/home/paul` (granted) |
| `$ENV{USER}` | undef: hidden by the bridge | undef: hidden by the bridge |
| `open(my $fh, '<', '/etc/passwd')` | opens it | `Permission denied` (`EACCES`) |
| `system('true')` | `0`: runs `/bin/sh` | `-1`, `Operation not permitted` |
| `connect` to 192.0.2.1:443 | goes out (`Operation now in progress`: nobody answers there) | `Permission denied` (`EACCES`) |
| `require Text::Wrap` (not loaded yet) | loads | `Permission denied` on `Text/Wrap.pm` |

Two things in it are Perl's own:

- **`%ENV` is held without the preload**, by the bridge (way 2), but only
  for a run in `run_sandboxed` or `run_sandboxed_with_map`. A script cannot
  ask libc's `getenv` from core Perl, so stunt's `getenv-home` row is left
  out for Perl.
- **Loading a module needs perl's library directories.** Perl reads a
  module from disk the first time it is loaded, so under grants with no
  read of its library directories `require Text::Wrap` fails unless it was
  loaded before the call. Load what a script needs before `run_sandboxed`
  (`perl.run("require Text::Wrap")`), or grant the directories
  (`perl -MConfig -e 'print "$Config{privlibexp} $Config{archlibexp}"'`,
  each with `/*`).

It is a library interposer, not a kernel boundary: a raw system call made
without libc is not seen, and macOS has no equivalent. Core Perl has no way
to make one (the containment table has why); only the contained child's
kernel layer holds those. `system` and backticks are held from ae 0.884.0
(aether #3580): before it, Perl's `fork` followed by `execvp` escaped the
preload. The containment table below has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("perl") {
        grant_fs_read("/usr/lib/perl5/5.44/core_perl/*")   // perl reads its modules to start
        grant_fs_read("/usr/share/perl5/core_perl/*")
        grant_fs_read("/dev/null")                         // perl_parse opens it for its -e
    }
    args = list.new()
    list.add(args, "/usr/lib/perl5/5.44/core_perl/CORE/libperl.so")
    list.add(args, "List::Util")                           // modules to load before the lockdown
    g, err = guest.launch(perms, "build/perl-worker", args, "perl", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    script = <<PL
        use List::Util qw(sum);
        print greet("perl"), " ", sum(1, 2, 3);
        open(my $fh, '<', '/etc/passwd') or die "open: $!";
    PL
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

On CachyOS this prints `hello 6`, then
`ERROR open: Permission denied at (eval 51) line 3.\x0a`.

Perl runs in `perl/worker`, a separate process `guest.launch` starts under
Landlock and seccomp from the policy, with the preload, and talking to the
host only over a socket (`std.ipc` frames). The worker loads libperl,
starts the interpreter and loads the modules the host names (its second
argument, comma-separated) before it locks itself down with no grants.
So the script reaches a file only when the host hands it a descriptor, an
environment variable only through a host function, and a module only if it
was loaded before the lockdown. The host can cancel a script at a deadline
and kills a worker that stops answering. On macOS, where nothing can
contain the child, `guest.launch` refuses and starts nothing. Build the
worker once with `ae build perl/worker/worker.ae -o build/perl-worker`; details
under [In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as Perl comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects,
else the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | matches | differs: exec-bin-sh stricter, fork stricter |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-etc-passwd not held, write-granted-file not held, getenv-home left out | differs: getenv-home left out | differs: open-granted-file stricter, syscall-openat left out, getenv-home left out, env-cached stricter |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | from ae 0.884.0: matches; before: differs: exec-bin-sh escapes | differs: fork stricter |
| `test_stunt_host_checked` | `-` | differs: spin left out | differs: spin left out | matches |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | differs: dlopen-libc-by-name stricter, dlopen-libc-by-path stricter |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | matches | differs: connect-192.0.2.1 stricter, connect-ipv4-mapped stricter, syscall-socket left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | matches | matches |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home left out, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held | from ae 0.884.0: differs: getenv-home left out; before: differs: getenv-home left out, exec-bin-sh escapes | differs: getenv-home left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | differs: write-granted-file stricter |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Perl 5.34.1
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Perl 5.44.0
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Perl 5.44.0
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Perl 5.44.0
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Perl 5.44.0
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's
with a read grant on perl's library directories and on `/dev/null` added,
below); the containment table below has each row. The table is generated
from a run (`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it
say which host produced which column.

## Containment table

What a Perl script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Perl
way ([containment/checks.pl](containment/checks.pl)), under the policies of
aether's `tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`).
"Per policy" means granted where the policy grants it and refused where it
does not, as stunt expects. The columns are the ways above: "in-process,
Linux (preload)" is way 3, "in-process, macOS" is way 2 where there is no
preload, and "contained child" is way 4. Every cell is a test outcome
(`../tests/run.sh`): checked on macOS arm64 (Perl 5.34, the system's),
CachyOS x86_64 with Landlock (5.44; ae 0.889.0 and 0.878.0) and Ubuntu
22.04 arm64 (5.34; preload and child, ae 0.888.0), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`open`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`open`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`open(.., '>')`) | per policy | **not held** | refused under every policy |
| `getenv-home` | **left out**: core Perl cannot ask libc's `getenv` | left out | left out |
| `env-cached` (`%ENV`) | per policy: the bridge hides what the grants do not grant | same, without the preload | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1`, `connect-ipv4-mapped` (`socket`, `connect`) | per policy | **not held** | refused under every policy: the worker's own lockdown has no tcp grant |
| `exec-bin-sh` (`system` and backticks) | per policy, from ae 0.884.0; before it **escapes**: both fork and call `execvp` with no exec grant | **not held** | refused under every policy: the worker's lockdown has no fork grant |
| `fork` (`fork`) | (child-only row) | (child-only row) | refused under every policy: the worker's lockdown has no fork grant |
| `dlopen-libm` (`DynaLoader::dl_load_file`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`dl_load_file`) | (child-only row) | (child-only row) | refused under every policy, `native=*` included |
| `syscall-fn-openat` (Perl's `syscall`, libc's) | (child-only row) | (child-only row) | refused: the preload answers every `syscall()` in a sandbox with `EPERM` |
| `syscall-openat`, `syscall-socket` (the syscall instruction) | **left out** | left out | left out |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: an `Undefined subroutine` the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (`aether_map_put` answers 0; read back: still the host's value) | same | same, read back |
| `spin` (`while (1) { $x++ }`) | **left out** | left out | cancelled at the deadline, from the worker's SIGALRM handler |
| `silent` (`$SIG{ALRM} = 'IGNORE'`, then a loop) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `print`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `half-frame` (`syswrite` of a frame's header and part of its payload to the worker's own socket, `AETHER_IPC_FD`) | (child-only row) | (child-only row) | killed: the host times out inside the frame |

What Perl cannot express is left out, with the reason in
[containment/lang.sh](containment/lang.sh):

- **`getenv-home`.** Core Perl has no call to libc's `getenv`; `%ENV` is
  the copy it took at start (`env-cached`), and the other way to a variable
  is to start a program that prints it (`exec-bin-sh`).
- **`syscall-openat`, `syscall-socket`.** Making a raw system call from
  Perl takes a `syscall` instruction, that is machine code, in memory that
  can execute. Core Perl has no `mmap`; the one way to it is libc's
  `syscall()` (`SYS_mmap`), which the sandbox preload answers `EPERM`
  whenever a sandbox is in force (so the contained child's `syscall-fn-openat`
  row is refused there, not at the kernel), and `DynaLoader` runs code only
  from a shared object loaded from disk. So the kernel-layer rows have no
  Perl script that reaches them.
- **`spin`, in-process.** Python and Lua stop it with the bridge's
  instruction limit. Perl has no hook per op short of replacing the
  interpreter's run loop, which is interpreter layout this bridge does not
  read, so the bridge has no limit; the contained child cancels at a deadline.

What the table says:

- **In-process is the easy default and is not containment on its own.**
  On Linux the preload holds Perl's file, socket, `fork` and `DynaLoader`
  calls, and from ae 0.884.0 `system` and backticks too. Before 0.884.0
  they escaped it: Perl's `system` forks and calls `execvp`, and that
  preload neither refused the fork (a `fork()` in a sandbox with no fork
  grant succeeds, observed on 0.878.0) nor saw the exec, which glibc's
  `execvp` makes inside libc. On macOS nothing holds Perl's own calls at
  all. The host functions (`binding-*`), which check the grants themselves,
  hold everywhere, and so does `%ENV`.
- **The contained child holds every row it runs**, on Linux, and is
  stricter than the policy: the worker locks itself down with no grants
  before the script starts, so a script reaches a granted file only through
  the host and never gets a handle on libc, a socket, a fork or a shell.
- **The contained child's policy has two grants more than stunt's**: a read
  of perl's library directories (`privlibexp`, `archlibexp` and the site and
  vendor ones that exist), which `../tests/stunt_lib.sh` passes as
  `GHL_PERL_LIB` (found by `../tests/perl_env.sh`) and
  `containment/child_host.ae` adds to every policy, and a read of
  `/dev/null`. Perl cannot start without them: `spawn_socket`'s Landlock
  floor lets the loader read libraries but nothing else, `perl_parse` opens
  `/dev/null` as the "script" of the `-e` the bridge starts the interpreter
  with (without the read, `Can't open /dev/null: Permission denied` and a
  failed start), and Perl loads the modules it needs (`strict`, `JSON::PP`,
  the XS `.so` of `Socket` and `POSIX`) from the library directories before
  the worker's lockdown. Any host of `perl-worker` has to give both. No row
  reads there.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row runs expecting the call to succeed, so the day the preload
  holds it that test fails and this table is brought up to date. The
  `exec-bin-sh` row is checked on either side of ae 0.884.0
  (`AE_SPAWN_PIN` in `ci/versions.env`), from the ae the tests run on.
- FreeBSD is not tested here (the preload's `fork()` has been seen to
  answer `ENOSYS` there for Python, which would be Perl's `system` as well).

### Loading modules under a sandbox

Not a row of stunt's, but what decides whether a script runs at all: Perl
reads a module from disk the first time it is loaded.
[containment/test_imports.sh](containment/test_imports.sh) holds it:

- in-process under the preload with no grants, `require Text::Wrap` (not
  loaded yet) is refused, `Permission denied` on the open of its source;
  without the preload (macOS) it loads;
- in-process with a read grant on perl's library directories, it loads;
- in the contained child, under any policy, `require Text::Wrap` (which
  the worker did not load before its lockdown) is refused, and
  `require Socket` (which it did) works.

## Loading libperl

The bridge's C (`aether_host_perl.c`) is compiled into the program that
imports `perl` (`@source` in `module.ae`); there is no archive to build
first and no `aether.toml` entry to add. libperl is not linked: the bridge
`dlopen`s it at the first call (with `RTLD_GLOBAL`, so the XS modules Perl
loads later resolve their `Perl_*` symbols against the same library), and a
program built on one machine runs against whatever Perl 5 the machine it
runs on has.

Where the bridge looks for libperl, in order:

1. `${AETHER_PERL_SONAME}`: an exact soname or path
   (`/usr/lib/x86_64-linux-gnu/libperl.so.5.38` on Ubuntu 24.04,
   `/usr/lib/perl5/5.44/core_perl/CORE/libperl.so` on Arch,
   `/System/Library/Perl/5.34/darwin-thread-multi-2level/CORE/libperl.dylib`
   with the system's perl on macOS);
2. `libperl.so`, where the system has one (Debian and Ubuntu ship it only
   in `libperl-dev`).

There is no list of versions to try: the program that starts yours owns
that choice. `../tests/perl_env.sh` is one way to make it, from a `perl`'s
`Config` (on macOS the system's libperl is an arm64e library; an arm64
program loads it all the same):

```sh
AETHER_PERL_SONAME=$(perl -MConfig -e 'for my $d ("$Config{archlibexp}/CORE", split " ", $Config{libpth}) { my $f = "$d/$Config{libperl}"; if (-f $f) { print $f; last } }')
```

A perl built without a shared libperl (`useshrplib` off) has nothing to
load. A perl built without `MULTIPLICITY` (a non-threaded build whose API
takes no interpreter argument) is refused at load with a message
(`perl_abi.h` says how it is told).

## In a contained child

`perl/worker/worker.ae` is a std.guest worker. It takes the libperl soname
as its first argument, since it is given no environment, and a
comma-separated list of modules to load as its second; it loads libperl,
starts the interpreter and loads them (and `JSON::PP` and `Time::HiRes`,
which it uses itself) before it locks itself down. In the script, `print`,
`printf` and `say` to `STDOUT` are `SAY` (`STDOUT` is tied; the arguments
joined by `$,` with `$\` added, and one trailing newline dropped), and each
function the host mounts is a Perl sub of the same name whose call is a
`CALL` the host answers: the arguments go as JSON, the answer comes back
decoded (a granted file is `{granted => true, text => ...}`), a `FAIL` dies
with the message and a `CANCEL` dies `cancelled by host: ...`. A `SIGALRM`
handler, fired every 50 ms by `Time::HiRes::setitimer`, looks for a
`CANCEL` (Perl runs a signal handler between two ops, so a busy loop is
reached; a call in flight carries its own `CANCEL` in its reply, and the
handler leaves it be). A script that sets `$SIG{ALRM}` to something else
stops answering, and the host kills it after its grace period: the
containment is the host's, not the handler's.
[containment/child_host.ae](containment/child_host.ae) is a complete host.

What Perl's start needs under the lockdown, honestly: everything happens
before it, under `spawn_socket`'s floor, and the host's grants are what
pay for it: the library directories (to read `strict.pm`, `JSON::PP`, the
XS shared objects) and `/dev/null`. After the lockdown the worker can read
nothing, so a `require` of a module not loaded fails with `Permission
denied` (`use POSIX` in the script included): name what a script needs in
the second argument. Whatever the policy, the worker's
process can read those directories (it must, to start), and its
environment holds std.ipc's `AETHER_IPC_FD` and `AETHER_IPC_KIND` and the
`AETHER_PERL_SONAME` it set itself, which is how a script finds its own
socket (the `half-frame` row).

## Host functions and the shared map

`run_sandboxed_with_map` runs a script with the shared map a token names
(a token the host made with `aether_shared_map_new`): the script calls
`aether_map_get(key)` (undef when absent) and `aether_map_put(key, value)`
(1 when written, 0 when the key is one the host froze); `%_aether_input`
holds the host's keys, and `%_aether_output` what the script put. This is
the host's own map: aether's copy of this bridge wrote a Perl hash the host
never saw, and spliced the values into source (a value with a quote ran as
code); here nothing is spliced.

Host functions are new to this bridge (aether's had none):

```aether
import perl

@c_callback
app_add(ctx: ptr) -> int {
    perl.push_int(ctx, perl.arg_int(ctx, 1) + perl.arg_int(ctx, 2))
    return 1
}

main() {
    perl.register("app::add", app_add)
    perl.run("die 'wrong' unless app::add(2, 3) == 5")
    perl.finalize()
}
```

| Call | Effect |
|------|--------|
| `perl.register(name, fn) -> int` | defines the Perl sub `name` (letters, digits, `_` and `::`) whose calls run an Aether `@c_callback (ctx: ptr) -> int`; 0, or -1 for a bad name |
| `perl.arg_count(ctx)`, `perl.arg_str(ctx, i)`, `perl.arg_int(ctx, i)` | **inside a callback**: the arguments, 1-based; all arrive as strings (UTF-8), `arg_int` parses one |
| `perl.push_str(ctx, v)`, `perl.push_int(ctx, v)` | **inside a callback**: the return value (the last push wins; return 1 to return it, 0 for undef) |
| `perl.raise_error(ctx, msg) -> int` | **inside a callback**: `return perl.raise_error(ctx, msg)`; the sub dies with `msg` and a newline after the callback has returned, so its defers run |
| `perl.last_error() -> string` | the `$@` of the last run that failed |

A string is a C string on the Aether side, so a NUL ends it there; a Perl
string with wide characters goes through as UTF-8 and comes back whole.

A Perl sub cannot be an XSUB without reading the interpreter's argument
stack, which is interpreter layout, so a host function is a Perl sub that
sets `$main::_aether_req` and reads `$main::_aether_call`, a scalar with a
magic *get* hook (perlguts, "Magic Virtual Tables") that does the call in C
and answers through the scalar. The prelude that does it, and
`aether_map_get`/`aether_map_put`, is in `aether_host_perl.c`.

## Implementation notes

- All Perl C API access goes through a `dlsym` function-pointer table: see
  `aether_host_perl.c`. The bridge has no unresolved `Perl_*` symbols and
  needs none of perl's headers: the types it uses are in `perl_abi.h`,
  which says why they are the same across Perl 5 (opaque interpreter, SV,
  CV and MAGIC pointers; `MGVTBL`, the one structure it fills in; `SV_GMAGIC`,
  `GV_ADD` and `PERL_MAGIC_ext`, which have had their values since 5.8).
- **The context argument.** In a `MULTIPLICITY` build every `Perl_*` API
  function takes the interpreter first (`pTHX_`); the bridge passes the one
  it made. A build without `MULTIPLICITY` takes none and exports the
  interpreter's variables as globals (`PL_errgv`, which a `MULTIPLICITY`
  build does not export): the bridge looks for that at load and refuses.
- **Threads.** One interpreter, one thread in it at a time: every entry
  takes a recursive lock (a host function may call back into the bridge),
  and on a threaded perl calls `Perl_set_context(my_perl)` first, since
  perl's "current interpreter" is a per-thread key.
- `PERL_SYS_INIT3` (the function `Perl_sys_init3`) is called once per
  process before the first interpreter. macOS's perl needs it: without it
  `perl_parse` fails with status 9 and defining a sub afterwards crashes
  (aether's copy ignored the status). It is never undone, so `finalize` then
  `init` again works (the interpreter is destroyed and a new one made).
- `boot_DynaLoader` is installed by an `xs_init`, so `use POSIX`,
  `use Socket` and the rest of the XS modules load.
- `SvTRUE(ERRSV)` and friends are the layout-agnostic calls
  `Perl_get_sv("@")`, `Perl_sv_true` and `Perl_sv_2pv_flags`.
- Nothing is rewritten into a script's source: not the map's values, not
  the environment's.

## Tests

- [`test_host_perl.ae`](test_host_perl.ae): the fire-and-forget surface,
  through `import perl`: a script evaluates, the interpreter really runs
  (state persists between runs), runtime and syntax errors are failures,
  `init` is idempotent. Moved from aether unchanged but for the import and
  the `describe` name.
- [`test_host_perl_host.ae`](test_host_perl_host.ae): what this repository
  added: host functions (strings, ints, UTF-8, undef, `raise_error` caught
  and uncaught, bad names, an unregistered name, two threads at once), the
  shared map (inputs, outputs in the host's map, frozen keys, no map outside
  a run, a value that is not safe to splice), `%ENV` under a sandbox (hidden
  to the grants, restored after, patterns), XS modules, and finalize then
  init again (`test_support.c` holds its C helpers).
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh` and
  `containment/test_imports.sh`: the containment table and the parity
  table above.

Both specs skip when libperl cannot be loaded (on GitHub Actions a skip is
a failure). They assert on return codes and Perl-side state, never on the
interpreter's stdout. `../tests/run.sh` runs everything.

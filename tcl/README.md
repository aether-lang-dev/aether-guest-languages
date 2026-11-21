# tcl: embedded Tcl

`import tcl` lets an Aether program run Tcl. It is the bridge that was
aether's `contrib.host.tcl`, moved here ([PROVENANCE.md](PROVENANCE.md)): the
same functions under the same names, `tcl.run`, `tcl.run_sandboxed`,
`tcl.vm_new` and the rest. It loads the machine's libtcl at run time and
needs no Tcl dev package (no `tcl.h`) to build. Tcl 8.5, 8.6, 9.0 and 9.1
([Loading libtcl](#loading-libtcl) says what was run).

**Tcl 9.0 (checked 2026-11-21, on cachyos).** A default Tcl 9.0 keeps its script
library in a zip archive attached to `libtcl9.0.so` (zipfs), and a Tcl 9 a
distribution packages will be built that way. The bridge mounts the archive the
way `tclsh` does, so a stock Tcl 9 initialises with nothing set, and
`TCL_LIBRARY` still wins when it is ([Tcl 9 and its zipfs](#tcl-9-and-its-zipfs)).
Before 2026-11-21 it did not: `Tcl_Init` failed with "Cannot find a usable
init.tcl" and `tcl.init()` returned -1. The full Tcl set (`GUEST_LANG=tcl
GHL_REQUIRE_CHILD=1 sh tests/run.sh`: the specs, every containment row, the
parity check and the contained child) passes against three Tcls on cachyos
(ae 0.934.0, Landlock): the system's 8.6.16, a Tcl 9.0.4 built by hand with
zipfs (the default) and one built with `--disable-zipfs`; the system's 8.6.12 on
ae-x64 and the system's 8.5.9 on macOS pass the same set (the child is skipped
on macOS). CI runs Homebrew's Tcl 9.1.0 on macOS too, which is not a zipfs
build, and the runner's Tcl 8.6.18; the `macos-tcl` job ran that
8.6 in the run whose log was read (2026-11-21), not Homebrew's Tcl 9 that it
installs. What a contained Tcl 9 needs that 8.6 does not is one read grant, on
libtcl itself, under "Tcl 9 and its zipfs".

## Four ways to run Tcl from Aether

From least to most contained. The first three run Tcl inside your program,
and the fourth runs it in a separate, locked-down process. All four stay
supported: pick by how far you trust the script, not by which is newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Tcl runs | your process | your process | your process | its own process (`tcl/worker`) |
| Your host commands | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Tcl's own `open`, `socket`, `exec` and `load` | not checked | **not checked** | checked against the grants (Linux), except `$env` | refused, beyond what the host hands over |
| Tcl's library, loaded on first use (`parray`, `clock format`, encodings) | yes | yes | only with a read grant on Tcl's library directory; a default Tcl 9 always, it reads the library from memory | only what the worker's warm-up script used before its lockdown; a default Tcl 9 always |
| Platforms | everywhere libtcl loads | same | Linux | Linux with Landlock; macOS refuses to start it |

### 1. Embed: Tcl in your process, no containment

```aether
import tcl

main() {
    script = <<TCL
        set d [dict create a {1 2 3} b "it's"]
        puts [dict get $d b]
        puts [string map {a A} {C:\no\escapes}]
        puts "[expr {6 * 7}] [llength [dict get $d a]]"
    TCL
    if tcl.run(script) != 0 { println("tcl failed") }
    tcl.finalize()
}
```

A heredoc (`<<TCL ... TCL`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape
processing, so Tcl's braces, `$` and backslashes need no escaping
(`{C:\no\escapes}` reaches Tcl as written), and the indentation the lines
share is stripped. The closing marker sits on a line of its own. A plain
string works too: `tcl.run("puts hi")`. This prints:

```
it's
C:\no\escApes
42 3
```

`tcl.run` evaluates in one interpreter that lives until `tcl.finalize()`, so
variables and procs persist from one call to the next. `tcl.finalize()` at
the end flushes what the script printed. A failing script prints its error
to stderr as `[tcl] <message>` and `tcl.run` returns -1.

For a script that calls back into the host, keeps state between runs or
returns a typed value, use a persistent VM: `tcl.vm_new`, `vm_register` for
host commands, `vm_eval`, `vm_result_*`
([Bidirectional embedding](#bidirectional-embedding-904) below). That is
still this first way: in-process and uncontained.

### 2. Embed inside a sandbox block

```aether
import tcl
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    script = <<TCL
        puts $::env(HOME)
    TCL
    if tcl.run_sandboxed(reader, script) != 0 { println("tcl failed") }
    tcl.finalize()
}
```

`run_sandboxed` pushes the block's grants as a level for the length of the
call. Aether's own std calls and every host command the script reaches are
checked against them, on every platform. Tcl's own `open`, `socket`, `exec`
and `load` go straight to libc, so on their own they are **not** checked:
this program prints `$HOME`, and the same script could read `/etc/passwd`,
connect anywhere or run a shell just as well. Use this way for scripts you
trust, where the grants describe what the host lends them.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits
between Tcl and libc and holds Tcl's own calls to the same grants (Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

A script granted only `HOME`, run on CachyOS (Tcl 8.6.16),
first without the preload and then with it:

| Tcl does | without | with `LD_PRELOAD` |
|---|---|---|
| `set ::env(HOME)` | `/home/paul` | `/home/paul` (granted) |
| `set ::env(USER)` | `paul` | `paul`: **not held** |
| `open /etc/passwd` | opens it | `couldn't open "/etc/passwd": permission denied` |
| `exec true` | runs it | `couldn't execute "true": not owner` (`EPERM`) |
| `socket -async 192.0.2.1 443` | goes out (nobody answers there) | `couldn't open socket: permission denied` |
| `parray ::env HOME` (not loaded yet) | prints it | `invalid command name "parray"` |
| `load libm.so.6 X` | loads libm, finds no `X_Init` | the same: `dlopen` of a library that is not libc loads |

Two things in it are Tcl's own:

- **`$env` is not held.** Tcl copies the environment into the `env` array
  when an interpreter is made (`Tcl_Init`), before any sandbox, and
  `$::env(USER)` is an array read, which no libc interposer sees. Tcl never
  calls `getenv`, so there is nothing for the preload to hold. This is
  stunt's `env-cached` row, and the bridge does not scrub the array; `unset
  ::env(USER)` before `run_sandboxed` if the script must not see it.
- **Tcl reads its library on first use.** `parray`, `clock format`, `auto_load`
  and an encoding other than the built-in ones are found on disk the first
  time they are used (`auto_load` reads `tclIndex`, then the file). Under
  grants with no read of Tcl's library directory (`$tcl_library`,
  `/usr/lib/tcl8.6`) they fail. Use what a script needs before
  `run_sandboxed`, or grant the directory:
  `grant_fs_read("/usr/lib/tcl8.6/*")`. (`clock format` also needs the
  `msgcat` package; on this host an embedded interpreter cannot find it at
  all, preload or not: `can't find package msgcat 1.6`.) A default Tcl 9
  keeps its library in memory, not in files, so none of this applies to it:
  those commands work with no grant ([Tcl 9 and its zipfs](#tcl-9-and-its-zipfs)).

It is a library interposer, not a kernel boundary: a raw system call made
without libc is not seen (Tcl itself cannot make one without a compiled
extension), and macOS has no equivalent. `exec` is held (aether
#3580): Tcl starts a program with `vfork` and `execvp`, and glibc's `execvp`
reaches `execve` inside libc, which the preload holds as well. The containment table below has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("tcl") {
        grant_fs_read("/usr/lib/tcl8.6/*")          // Tcl_Init sources init.tcl from here (a default Tcl 9: libtcl9.0.so itself)
    }
    args = list.new()
    list.add(args, "libtcl8.6.so")
    g, err = guest.launch(perms, "build/tcl-worker", args, "tcl", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    script = <<TCL
        puts "[greet tcl] [llength {a b c}]"
        open /etc/passwd
    TCL
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

On CachyOS this prints `hello 3`, then
`ERROR couldn't open "/etc/passwd": permission denied`.

Tcl runs in `tcl/worker`, a separate process `guest.launch` starts under
Landlock and seccomp from the policy, with the preload, and talking to the
host only over a socket (`std.ipc` frames). The worker loads libtcl and makes
its interpreters (which reads Tcl's library directory) before it locks itself
down with no grants, then the script starts. So the script reaches a file
only when the host hands it a descriptor, an environment variable only
through a host command, and a part of Tcl's library only if it was used
before the lockdown. The host can cancel a script at a deadline and kills a
worker that stops answering. On macOS, where nothing can contain the child,
`guest.launch` refuses and starts nothing. Build the worker once with `ae
build tcl/worker/worker.ae -o tcl-worker`; details under
[In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as Tcl comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects,
else the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | matches | differs: exec-bin-sh stricter, fork left out |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-etc-passwd not held, write-granted-file not held, getenv-home left out | differs: getenv-home left out | differs: open-granted-file stricter, syscall-fn-openat left out, syscall-openat left out, getenv-home left out, env-cached stricter |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | matches | differs: fork left out |
| `test_stunt_host_checked` | `-` | matches | matches | differs: half-frame left out |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | differs: dlopen-libc-by-name stricter, dlopen-libc-by-path stricter |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: connect-ipv4-mapped left out, open-etc-passwd not held | matches | differs: connect-192.0.2.1 stricter, connect-ipv4-mapped stricter, syscall-socket left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped left out | matches | matches |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home left out, env-cached not held, connect-192.0.2.1 not held, connect-ipv4-mapped left out, exec-bin-sh not held | differs: getenv-home left out, env-cached escapes | differs: getenv-home left out, fork left out, syscall-fn-openat left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | differs: write-granted-file stricter |

- Column 2: macOS 27.3 arm64, ae 0.934.0, Tcl 8.5.9
- Column 3: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, Tcl 8.6.16
- Column 4: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, Tcl 8.6.16
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's
with a read grant on Tcl's library directory added, below); the containment
table below has each row. The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it
say which host produced which column.

## Boundary tests

The shared tests in `tests/boundary.py`, run in this language's worker (`tests/test_boundary.sh`): one script per case, written in tcl (`containment/boundary.tsv`), and what the host saw on the wire. `ok` means the value crossed as the same JSON value; `n/a` means the language has no way to write the case; anything else is what happened instead. Nothing in the tests lists what a worker can do.

<!-- boundary:begin: generated by sh tests/run.sh --write-parity, from build/boundary-*.tsv -->
| case | guest to host | host to guest, passed back |
|---|---|---|
| `str` | ok | ok |
| `str-empty` | ok | ok |
| `str-nonascii` | ok | ok |
| `str-nul` | differs: the host saw ["a��b"] | differs: the host saw ["a��b"] |
| `str-60k` | ok | ok |
| `int` | ok | ok |
| `int-neg` | ok | ok |
| `int-max` | ok | ok |
| `int-min` | ok | ok |
| `float` | ok | ok |
| `bool-true` | differs: the host saw ["true"] | differs: the host saw [1] |
| `bool-false` | differs: the host saw ["false"] | differs: the host saw [0] |
| `null` | n/a | differs: the host saw [""] |
| `array` | n/a | differs: the host saw ["1 a 2.5"] |
| `map` | n/a | differs: the host saw ["k v n 1"] |
| `nested` | n/a | differs: the host saw ["a {1 {b x}} c {d 2.5}"] |
| a call inside a call's argument (`nest`) | ok | |
| an error answer (`FAIL`) to a call (`refuse`) | caught | |
| a cancel while a call waits (`wait`) | heard at the call | |
| a cancel inside a guest loop (`loop`) | heard in a loop | |
| a 100,000-byte argument (`big-up`) | refused, worker killed | |
| a 100,000-byte answer (`big-down`) | refused: killed | |
<!-- boundary:end -->

## What crosses the boundary

The top-level README's first table says it in a line; this is the detail, each
claim with the spec that backs it.

- **In-process transport.** a direct C call into the embedded Tcl (`dlopen`ed libtcl); no transport.
- **Types.** everything is a string underneath. in: str, int, strlist as globals. out: str, int, double, bool (typed results). A callback reads str, int, double, bool and pushes int, str, bool, nil, double.
- **Duplex.** host calls the guest; the guest calls back mid-eval; a nested `eval` runs at global level and leaves the callback's arguments intact (tested).
- **Callbacks.** host commands by name, namespaced ok.
- **Errors and state.** a failing script is -1 with the error as the result; `raise_error`; a command limit and a time limit; one interpreter per VM handle, used by one thread (another thread is refused, not crashed).
- **Contained child.** Contained child: `std.guest` over a `std.ipc` socketpair, 4-byte length-prefixed frames, JSON arguments and values, a granted file as a descriptor, guest-initiated calls answered one at a time, no nesting (the top-level README lists the common part). strings and integers cross as JSON (the stunt rows send no other kind); a `FAIL` is a Tcl error; `CANCEL` reaches a loop through a child interpreter.
- **Tests.** `tcl/test_host_tcl_vm.ae`: "callbacks, globals, state kept between evals, namespaced commands", "typed results", "errors", "a nested eval runs at global level and leaves the callback's arguments intact", "the command limit and the time limit", "another thread is refused, and the VM works afterwards on its own".

## Containment table

What a Tcl script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Tcl way
([containment/checks.tcl](containment/checks.tcl)), under the policies of
aether's `tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`).
"Per policy" means granted where the policy grants it and refused where it
does not, as stunt expects. The columns are the ways above: "in-process,
Linux (preload)" is way 3, "in-process, macOS" is way 2 where there is no
preload, and "contained child" is way 4. Every cell is a test outcome
(`../tests/run.sh`): checked on macOS arm64 (the system's Tcl 8.5) and
CachyOS x86_64 with Landlock (Tcl 8.6.16), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`open`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`open`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`open .. w`) | per policy | **not held** | refused under every policy |
| `getenv-home` | (left out: Tcl never calls `getenv`) | (left out) | (left out) |
| `env-cached` (`$::env(HOME)`) | **escapes**: the copy Tcl took in `Tcl_Init` | **not held** | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1` (`socket -async`) | per policy | **not held** | refused under every policy: the worker's own lockdown has no tcp grant |
| `connect-ipv4-mapped` (`socket -async ::ffff:192.0.2.1`) | per policy | (left out on macOS: Tcl 8.5 there resolves no IPv6 literal) | refused under every policy |
| `exec-bin-sh` (`exec ... 2>@1`, stderr redirected, see below) | per policy | **not held** | refused under every policy: the worker's own lockdown has no `fork` grant, and `exec` forks |
| `fork` | (left out: Tcl has no fork) | (left out) | (left out) |
| `dlopen-libm` (`load`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`load`) | (child-only row) | (child-only row) | refused under every policy, `native=*` included (stricter than stunt: the worker has no native grant); `load` gets an error it can catch (below) |
| `syscall-fn-openat`, `syscall-openat`, `syscall-socket` | (child-only row) | (child-only row) | (left out: Tcl has no `syscall()` and runs no machine code) |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host commands) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: `invalid command name`, which the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `spin` (`while 1 {}`) | stopped by the time limit | same | cancelled at the deadline, from the worker's time-limit handler |
| `silent` (`expr {7**20000000}`) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `puts`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `half-frame` | (child-only row) | (child-only row) | (left out: a script cannot write to a descriptor it has no path for) |

Left out, and why: `getenv-home` because Tcl keeps its own copy of the
environment and never calls `getenv` (`env-cached` is the Tcl-shaped
version); `fork` and the three `syscall-*` rows because Tcl has neither
(`exec` forks and execs in one step, and `load` takes a path to a library,
not machine code); `half-frame` because a Tcl script has no way to write to
the worker's socket (it would need an `open` of a path for a descriptor that
has none); and `connect-ipv4-mapped` on macOS, because the system's Tcl 8.5
fails to resolve any IPv6 literal there.

What the table says:

- **In-process is the easy default and is not containment on its own.**
  On Linux the preload holds Tcl's `open`, `socket`, `exec` and the libc
  calls under them, and `exec`. `$env` is never held
  in-process. On macOS nothing holds Tcl's own calls at all. The host
  commands (`binding-*`), which check the grants themselves, hold
  everywhere.
- **The contained child holds every row it runs**, on Linux, and is
  stricter than the policy: the worker locks itself down with no grants
  before the script starts, so a script reaches a granted file only through
  the host and never gets a socket or a shell. The kernel layer from the
  policy is the same one Lua's and Python's rows rest on.
- **The contained child's policy has one grant more than stunt's**: a read
  of Tcl's library directory (`$tcl_library`; `/usr/lib/tcl8.6` on
  CachyOS), which `../tests/stunt_lib.sh` passes as `GHL_TCL_LIBRARY` and
  `containment/child_host.ae` adds to every policy. `Tcl_Init` cannot make
  an interpreter without it: it sources `init.tcl` from there
  ("Can't find a usable init.tcl"), and `spawn_socket`'s Landlock floor
  lets the loader read libraries but lists no directory. Any host of
  `tcl-worker` has to give it. Nothing else is needed to start: after
  `Tcl_Init` the worker locks itself down and reads nothing more, so the
  script has only the part of Tcl's library the worker's warm-up used (the
  worker's second argument; see below). No row reads there. For a default
  Tcl 9 the one grant is a different one, narrower and on a file: a read of
  libtcl itself (`GHL_TCL_ZIPFS_LIB`), and then the script has all of Tcl's
  library; see [Tcl 9 and its zipfs](#tcl-9-and-its-zipfs). A libtcl in a
  private prefix also needs its directory granted for the loader
  (`GHL_TCL_EXTRA_READ`, as Ruby's test host takes `GHL_RUBY_EXTRA_READ`); the
  system's needs nothing for that.
- **`load` of a refused `dlopen` is an error the script can catch.** The
  preload refuses a handle on libc by returning NULL from `dlopen` and sets
  `dlerror()` (aether #3799), so Tcl 8.6's `TclpDlopen`, which formats that
  `dlerror()` with `Tcl_ObjPrintf("%s")`, has a message to format, and `load`
  gives the script `couldn't load file "libc.so.6": ... refused by the
  sandbox`, which `catch` sees. The two `dlopen-libc-*` rows run in the
  child and are refused under every policy, `native=*` too, as in Lua's
  worker.
- **A refused `socket()` reads as "host is unreachable".** In the contained
  child with no tcp grant, seccomp refuses `socket(AF_INET, SOCK_STREAM)`
  with `EPERM`; Tcl tries each address, finds none it can open, and reports
  `EHOSTUNREACH`. `checks.tcl` takes that, from the synchronous
  `socket -async` of an address literal, as the refusal it is. A connect
  that is let through and then finds no route fails later (`fconfigure
  -error`), not there.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row runs expecting the call to succeed, so the day the preload
  holds it that test fails and this table is brought up to date.

- **A bare `exec` needs a temporary file.** Without a redirection of stderr
  Tcl's `exec` first makes a file for the command's stderr (`/tmp/tclXXXXXX`,
  or `$TMPDIR/tclXXXXXX`, with libc's `mkstemp`; it unlinks it at once). The
  preload checks a temporary file where it is made (aether
  #3751: "a temporary file needs `fs_write` where it is made"), and the
  open is for reading and writing, so it wants `fs_read` and `fs_write`
  there. Under stunt's policy `exec=/bin/sh,fork` a bare `exec` is then
  refused with `couldn't create error file for command: permission denied`.
  So the `exec-bin-sh` row is done with
  `2>@1`, which makes no file and so asks only what stunt's row asks, whether
  `exec` and `fork` alone let a shell start (they do), and the
  bare `exec` is `containment/test_exec_tmpfile.sh`, in-process, which holds
  both sides: refused under `exec=/bin/sh,fork`, and
  succeeding under the narrowest grant that lets it run, `fs_read` and
  `fs_write` on one scratch directory (`$WORK/tmp/*` there) plus `env=TMPDIR`
  with `$::env(TMPDIR)` set to it, since Tcl reads `TMPDIR` through libc's
  `getenv` and falls back to `/tmp` when that is refused. A host that runs
  Tcl scripts that call `exec` should grant that, never `/tmp`.

Tcl has not been run on FreeBSD.

## Loading libtcl

The bridge's C (`aether_host_tcl.c`) is compiled into the program that
imports `tcl` (`@source` in `module.ae`); there is no archive to build first
and no `aether.toml` entry to add. libtcl is not linked: the bridge
`dlopen`s it at the first call (with `RTLD_GLOBAL`, so the extensions Tcl
loads later resolve their `Tcl_*` symbols against the same library), and a
program built on one machine runs against whatever Tcl 8.5, 8.6 or 9.0 the
machine it runs on has.

Where the bridge looks for libtcl, in order:

1. `${AETHER_TCL_SONAME}`: an exact soname or path (`libtcl8.6.so.0` on
   Ubuntu, `/usr/lib/libtcl8.6.so` on Arch,
   `/opt/homebrew/opt/tcl-tk/lib/libtcl9.0.dylib` with Homebrew on macOS).
   When it is set, nothing else is tried.
2. Otherwise these, the first that loads: `libtcl8.6.so.0`, `libtcl8.6.so`,
   `libtcl9.0.so.0`, `libtcl9.0.so`, `libtcl9.1.so.0`, `libtcl9.1.so`,
   `libtcl.so`, Homebrew's tcl-tk
   (`/opt/homebrew/opt/tcl-tk/lib/libtcl8.6.dylib`, then `libtcl9.0.dylib`,
   then `libtcl9.1.dylib`, and the same under `/usr/local/opt`), and `/usr/lib/libtcl.dylib`, which
   is macOS's own Tcl 8.5 (in the dyld cache, with no file on disk). Debian
   and Ubuntu ship `libtcl8.6.so.0` in the runtime package (`tcl8.6`) and the
   unversioned `libtcl.so` only in the dev package, which is not needed.

`../tests/tcl_env.sh` is one way to choose among several, for the tests:
`TCL_VERSION=8.6` (or `9.0`, `9.1`, `8.5`) asks for that one, else 8.6, 9.0,
9.1, then 8.5. A machine with its own layout:

```sh
AETHER_TCL_SONAME=$(echo 'puts libtcl$tcl_version.so' | tclsh)
```

### Tcl 8.5, 8.6 and 9.0

One build serves them all; `tcl_abi.h` says why. In short: Tcl 9 widened
every length, count and index parameter from `int` to `Tcl_Size`
(`ptrdiff_t`). The bridge declares `ptrdiff_t` for the functions it looks up
(an 8.x callee reads the low 32 bits of the argument, which holds on x86-64,
arm64 and where the two types are one), and receives a command's `objc` as
`ptrdiff_t` and keeps the low 32 bits. `Tcl_Obj` is read for its `typePtr`
only, the fourth word in all three; reference counts go through
`Tcl_DbIncrRefCount` / `Tcl_DbDecrRefCount`, not the macros that write the
field. `Tcl_GetVersion` is checked at load, and a library older than 8.5 or
newer than 9.x is refused with a message.

What has run, all with the specs, every containment row, the parity check and
(on Linux) the contained child, ae 0.934.0:

| Tcl | where | when | how |
|---|---|---|---|
| 8.6.16, the system's | cachyos | 2026-11-21 | `GUEST_LANG=tcl GHL_REQUIRE_CHILD=1 sh tests/run.sh` |
| 8.6.12, the system's (Ubuntu 22.04) | ae-x64 | 2026-11-21 | the same |
| 8.5.9, the system's | macOS arm64 | 2026-11-21 | `GUEST_LANG=tcl sh tests/run.sh` (no child on macOS) |
| 9.0.4, a default build with zipfs, private prefix | cachyos | 2026-11-21 | the same as the first, with `AETHER_TCL_SONAME` set to its `libtcl9.0.so` |
| 9.0.4, `--disable-zipfs`, private prefix | cachyos | 2026-11-21 | the same, and `GHL_TCL_EXTRA_READ` set to its `lib` directory |
| 9.1.0, Homebrew's tcl-tk (library in files, not zipfs), in-process only | macOS 15 arm64, GitHub Actions | 2026-11-21 | the `macos-tcl9` job, `TCL_VERSION=9.1 GUEST_LANG=tcl sh tests/run.sh` (run 44946377149) |

The two Tcl 9.0.4 builds are from the 9.0.4 source tarball, `./configure
--prefix=<dir> --enable-shared` (and `--disable-zipfs`), `make`, `make
install`; the zipfs build's `make` needs `LD_LIBRARY_PATH` pointing at the build
directory, for the Tcl it runs while building its bundled packages. A Tcl 9
that a package manager installed was not run: cachyos packages Tcl 8.6.16
only (`pacman -Si tcl`), and no system package was installed for this. What CI
runs is in [Tests](#tests).

### Tcl 9 and its zipfs

A default build of Tcl 9.0 keeps its script library (`init.tcl`, the time zone
data, the encodings, `http` and the rest: 870 files in 9.0.4) in a zip archive
attached to `libtcl9.0.so`, and mounts it read-only at `//zipfs:/lib/tcl` (the
library itself at `//zipfs:/lib/tcl/tcl_library`) when the application
initialises Tcl the way Tcl 9 documents (`zipfs.3`): `tclsh` calls
`TclZipfs_AppHook` first thing in `main`, which calls `Tcl_FindExecutable`,
mounts a zip attached to the executable and the one attached to libtcl, and
records where the library is. A build made with `--disable-zipfs` has its
library in files, as Tcl 8 does. The bridge used to call only
`Tcl_FindExecutable(NULL)`, so nothing was mounted, `Tcl_Init` looked for files
that are not there and `tcl.init()` was -1, "Cannot find a usable init.tcl".
It now does what `tclsh` does (`tcl_process_init` in `aether_host_tcl.c`):

- It looks up `TclZipfs_AppHook` with `dlsym`, optionally. A Tcl 8 has no such
  symbol and gets `Tcl_FindExecutable(NULL)` as before, so 8.5 and 8.6 are
  unchanged.
- It passes a name with no slash that finds no executable as `argv[0]`, not
  NULL. With NULL, Tcl 9.0.4 returns from `TclpFindExecutable` before it records
  which shared library it runs from, and no zipfs is mounted (found by reading
  `unix/tclUnixFile.c` and by trying: `zipfs mount` was empty). With a name
  that finds nothing, `info nameofexecutable` is empty, which is what Tcl 8 gives
  here too. The host's own executable is not given: Tcl would open it looking
  for an attached zip, and `info nameofexecutable` would name the host program,
  which a script could `exec`.
- It is done once per process, under a mutex, before the first interpreter
  (the shared one and every VM call it): it is Tcl's own process-wide state (the
  executable name, the zipfs mounts), not the bridge's, so there is no handle to
  hang it on. `Tcl_Finalize` is final in Tcl 8.6 and 9.0 alike ("Tcl_InitSubsystems
  called while exiting" if you initialise again), so it is not undone.
- `TCL_LIBRARY` is honoured: Tcl's init script tries it before the zip and before
  the compiled-in directory. A Tcl built without zipfs finds its library on disk
  as before. A failed `Tcl_Init` on a Tcl 9 says, besides Tcl's message, that the
  library is in a zip unless built otherwise and what to do.

What Tcl 9 reads, measured on cachyos with `strace -f` on the contained worker
(Tcl 9.0.4 with zipfs): libtcl9.0.so by the loader, then the same file again
by Tcl while it initialises (a libc `open`, which the preload turns into an
`openat2` that refuses symlinks, so Tcl resolves the path first and the path to
grant is the real file; then an `mmap` of the whole file and a `close`), and
nothing else: from then on the archive is read from memory. No temporary file
is made, because the default archive has no shared object in it (`zipfs list
-glob *.so` is empty; thread, sqlite3, tdbc and itcl are ordinary files beside
libtcl, found as before).

What that does to containment, against Tcl 8.6 and a Tcl 9 without zipfs:

| | library in files (8.x, 9 without zipfs) | library in the zip (default 9) |
|---|---|---|
| grant needed to initialise inside a sandbox | read of the library directory (`$tcl_library/*`) | read of libtcl itself: the one file, as Tcl resolved it |
| the loader's read of libtcl | the Landlock floor covers a system library; a private prefix needs its directory (`GHL_TCL_EXTRA_READ`) | the same, and the grant above covers a private prefix's too |
| `clock format`, an encoding, `parray` on first use, with no grant, in-process under the preload | refused | works |
| the same in the contained child, with no warm-up script | refused | works |
| mounting an archive of the script's own (`zipfs mount`) | (no such command) | an `open` the grants hold: refused with no grant, and in the child under every policy |

- **The grant is narrower, on a file.** The narrowest grant for a default Tcl 9
  is `grant_fs_read("<libtcl9.0.so as Tcl resolved it>")`: ask a `tclsh` of that
  Tcl for `zipfs mount //zipfs:/lib/tcl`, which prints the file
  (`tests/tcl_env.sh` does, as `GHL_TCL_ZIPFS_LIB`). It is needed wherever Tcl
  is initialised under a sandbox: in the contained child always, and in an
  in-process host that makes its interpreter inside a `sandbox.enforce` block.
  A host that makes the interpreter first, as `containment/host.ae` does and as
  the other in-process ways here do, needs nothing. Measured with the preload on
  cachyos, interpreter made inside a block with no grants: `tcl.vm_new()` fails;
  with `fs_read` on exactly `libtcl9.0.so` it works; with `fs_read` on another
  file it fails; a no-zipfs build needs its library directory instead. In the
  contained child with a Tcl in a private prefix, exactly that one file is
  enough. Never grant a directory like `/usr/lib/*` or `/tmp` for this.
- **Not run: a system Tcl 9 in the contained child.** No packaged Tcl 9 was
  available (cachyos packages 8.6.16). That the one-file grant is needed there as
  well follows from the same preload check of Tcl's `open`, since the loader's
  floor makes no difference to a libc `open`; it was not seen.
- **The library is available with no grant, from memory.** In a default Tcl 9
  `clock format`, `parray`, an encoding and the time zones work in-process under
  the preload with no grants and in the contained child with no warm-up script,
  where Tcl 8.6 refuses them (`containment/test_library_first_use.sh` holds both
  sides). A script cannot reach any file by it: only what is mounted, Tcl's own
  library, is in memory.
- **A mount of the script's own is held.** `zipfs mount file.zip //zipfs:/x` is
  an `open` of `file.zip`, refused without `fs_read`, and in the child under every
  policy (`containment/test_zipfs_mount.sh`).
- **`load` of a shared object out of a mounted zip makes a temporary file.** Tcl
  copies it to `/tmp/tclXXXXXX` (or `$TMPDIR`; created, written, unlinked), then
  `dlopen`s that. Seen by hand with `strace` on a zip made with `zipfs mkzip`,
  not a row: a script has to mount that zip first, which needs the grant above,
  and the default archive has none. A host that allows it would need the
  temporary-file grants of the bare `exec` below, on one scratch directory,
  never `/tmp`. Not tried under a sandbox.
- **No stunt row changes.** The parity check (`tests/parity.sh`) agrees with the
  table's columns for all three Tcls, 20 cells each, and the bare `exec`'s
  temporary file, the two `dlopen` rows (`load` of a file on disk) and `env-cached`
  come out as under 8.6. The `exec` row's temporary file is Tcl's, not the zip's.

## In a contained child

`tcl/worker/worker.ae` is a std.guest worker. It takes the libtcl soname as
its first argument, since it is given no environment, and a Tcl warm-up
script as its optional second; it loads libtcl, makes its interpreters and
runs the warm-up before it locks itself down. In the script, `puts` is
`SAY`, and each command the host mounts is a Tcl command of the same name
whose call is a `CALL` the host answers: the arguments go as JSON (a word
that reads as a number goes as one, anything else as a string), the answer
comes back decoded (a JSON object is a dict, so a granted file is
`{granted 1 text ...}`; `true` and `false` are 1 and 0; `null` is the empty
string), a `FAIL` is an error with the host's message and a `CANCEL` is an
error `cancelled by host: ...`. The script runs in a child interpreter of the
worker's, under a 100 ms time limit that a handler renews while it finds no
`CANCEL` (std.guest's "interrupt hook"): Tcl cannot be interrupted any other
way in `while 1 {}`, which runs no command to poll from. A script that
spends its time inside one command (`expr {7**20000000}`) is not
interrupted, because Tcl checks its limits only between commands and on a
loop's jump (and in `after`, so a sleep is cancelled): it stops answering,
and the host kills it after its grace period. The containment is the host's,
not the hook's. [containment/child_host.ae](containment/child_host.ae) is a
complete host.

**What it needs to read, and when.** Before the lockdown, `Tcl_Init` reads
`init.tcl` from Tcl's library directory (`$tcl_library`): the host's policy
must grant a read on it (`grant_fs_read("/usr/lib/tcl8.6/*")` above), and
the worker needs nothing else to start. (A default Tcl 9 reads its library
from a zip in libtcl instead: the grant is a read of libtcl9.0.so itself, and
the rest of this paragraph does not apply, since the library is then in memory:
[Tcl 9 and its zipfs](#tcl-9-and-its-zipfs).) After the lockdown nothing can be
read, whatever the policy: Tcl's lazily loaded library is out of reach unless
the warm-up used it. Measured on CachyOS: `clock format`
(`couldn't read file "/usr/lib/tcl8.6/clock.tcl"`), `parray`
(`invalid command name`) and an encoding that is not built in (`unknown
encoding "iso8859-2"`) fail; `package require Tcl`, `string map` and the
rest of the core work. With `parray ::tcl_platform byteOrder` as the second
argument, `parray` works in the script. The environment of the worker's
process holds std.ipc's `AETHER_IPC_FD` and `AETHER_IPC_KIND` and the
`AETHER_TCL_SONAME` it set itself; Tcl's `$env` is made from it, so the
script sees those and no `HOME`.

## Bidirectional embedding (#904)

`tcl.run` is enough to *run* a script. A host that uses Tcl as an extension
language, calling into it and being called back, needs a **persistent VM**
that registers host commands, injects globals, and returns **typed** results:

```aether
import tcl

@c_callback
app_add(vm: ptr) -> int {
    tcl.push_int(vm, tcl.arg_int(vm, 1) + tcl.arg_int(vm, 2))
    return 1
}

main() {
    vm = tcl.vm_new()
    tcl.vm_register(vm, "app.add", app_add)
    tcl.vm_set_global_int(vm, "BASE", 100)
    tcl.vm_set_instruction_limit(vm, 1000000, 100)
    tcl.vm_set_time_limit(vm, 2000, 100)

    if tcl.vm_eval(vm, "expr {[app.add 2 3] + $BASE}") == 0 {
        println("${tcl.vm_result_int(vm)}")   // 105
    }
    tcl.vm_pop_result(vm)
    tcl.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `tcl.vm_new() -> ptr` | a new interpreter, separate from the one `tcl.run` shares |
| `tcl.vm_register(vm, name, fn_ptr) -> int` | register an Aether `(vm: ptr) -> int` `@c_callback` as the Tcl command `name`, taken verbatim: `"app.add"` is called `app.add 2 3`, and `"app::add"` lands in the namespace `::app` |
| `tcl.vm_set_global_str/_int/_strlist(vm, name, v)` | set a global variable (a string list becomes a Tcl list) |
| `tcl.vm_eval(vm, code) -> int` | run at global level; 0, or -1 with the error as the result |
| `tcl.vm_result_type(vm) -> int` | the result's tag (`TCL_T*`, below) |
| `tcl.vm_result_int/_double/_str/_bool(vm)` | read the result; valid until `vm_pop_result` or the next eval |
| `tcl.vm_pop_result(vm)` | release the result |
| `tcl.vm_set_instruction_limit(vm, budget, every) -> string` | a budget of Tcl commands per eval, checked every `every` commands; `budget` <= 0 removes it |
| `tcl.vm_set_time_limit(vm, ms, every) -> string` | a wall-clock budget per eval; `ms` <= 0 removes it |
| `tcl.arg_count/_type/_int/_double/_str/_bool(vm, i)` | **inside a callback**: the arguments, 1-based; the command's name is not one |
| `tcl.push_int/_double/_str/_bool/_nil(vm, v)` | **inside a callback**: the command's return value; the last push wins, and the callback returns 1 to return it or 0 to return nothing |
| `tcl.raise_error(vm, msg) -> int` | `return tcl.raise_error(vm, msg)`: the command fails with `msg` after the callback has returned, so its defers run; a script can `catch` it |

Tags: `TCL_TNIL`, `TCL_TBOOL`, `TCL_TINT`, `TCL_TSTR`, `TCL_TTABLE`
(a list or dict), `TCL_TERR`, `TCL_TOTHER` (an integer too wide for 64
bits), and `TCL_TFLOAT`, which Lua has not.

What is particular to Tcl:

- **Every value is a string.** A value's tag comes from what Tcl has
  already made of it: `expr {2+3}` is `TCL_TINT`, but the literal 5 in
  `set x 5` is still `TCL_TSTR`, and so are the 2 and the 40 of a call
  `add 2 40` (a host command that wants an int parses the string, as
  `containment/host.ae`'s `host_add` does). The readers convert from the
  string either way, so `vm_result_int` reads both as 5. Tcl has no nil and
  no boolean: `push_nil` returns the empty string, which reads `TCL_TNIL`,
  and `push_bool` returns 1 or 0.
- **Numbers.** An integer too wide for 64 bits (`expr {2**70}`) and a double
  out of range saturate when read as int; NaN and non-numbers read 0.
- **Threads.** An interpreter belongs to the thread that created it. Every
  `vm_*` call from another thread is refused without touching Tcl:
  `vm_eval` returns -1, with an error result saying why. Keep a VM inside
  one actor, or on one thread.
- **Two limits.** The instruction limit counts Tcl commands, so a loop that
  runs none, such as `while 1 {}` (compiled to jumps), is not bounded by it:
  the time limit is. A script cannot `catch` either limit and carry on.
  Both apply to the outermost eval; an eval a callback makes runs inside
  the same budget rather than starting a new one.
- **Nested evals.** A callback may `vm_eval` on its own VM. It runs at
  global level, not in the frame of the proc that called the callback,
  and the outer call's arguments survive it.
- **Not a sandbox.** A VM's script has Tcl's full reach (`open`, `exec`,
  `socket`). For untrusted code use `run_sandboxed` with the preload, or the
  contained child.

`tcl.run_sandboxed_with_map(perms, code, map_token)` runs a script with the
shared map of `std.sandbox` (by token) open to it as two commands,
`aether_map_get key` (the value, or the empty string) and `aether_map_put key
value`; a write to a key the host froze is dropped, and gives the script no
sign of it (the `map-overwrite-input` row reads the key back).

## Implementation notes

- All Tcl C API access goes through a `dlsym` function-pointer table: see
  `aether_host_tcl.c`. The bridge has no unresolved `Tcl_*` symbols, and
  needs none of Tcl's headers: the few types it uses are in `tcl_abi.h`.
  Tcl's stubs mechanism is for extensions loaded into a running Tcl, not
  for an embedder, so plain `dlsym` it is.
- Tcl's one-time, process-wide initialisation (`tcl_process_init`) is the one
  piece of state that is not per handle: `TclZipfs_AppHook` on a Tcl 9,
  `Tcl_FindExecutable(NULL)` otherwise, guarded and idempotent
  ([Tcl 9 and its zipfs](#tcl-9-and-its-zipfs)).
- Tcl 9 made `Tcl_Eval`, `Tcl_GetString` and `Tcl_GetStringResult` macros
  and stopped exporting them; the bridge composes them from `Tcl_EvalEx`,
  `Tcl_GetStringFromObj` and `Tcl_GetObjResult`, which exist in 8.5, 8.6 and
  9.0.
- The limit functions (`Tcl_LimitSetCommands` and friends) need Tcl 8.5. A
  limit is checked only inside compiled code, so the VM evaluates a script
  as an object (`Tcl_EvalObjEx`), not a string (`Tcl_EvalEx`), which would
  run it a command at a time.
- Tcl 9 on a platform where `long` is not 64 bits is refused at load
  (`Tcl_Time.sec` became a `long long`).

## Tests

- [`test_host_tcl.ae`](test_host_tcl.ae): the fire-and-forget surface and
  the basics of the VM tier, through `import tcl`: a script evaluates, state
  persists between runs, a failing script is a failure, host commands with
  typed values, errors, nested evals and both limits. Moved from aether
  unchanged but for the import and the `describe` name.
- [`test_host_tcl_vm.ae`](test_host_tcl_vm.ae): aether's
  `host_tcl_bidirectional` C harness as an Aether spec with `@c_callback`
  host commands: namespaced registration, every argument and result type,
  bignums, top-level `return`, errors, nested evals and their return codes,
  both limits, the thread refusal, two VMs, and memory staying flat over
  60,000 rounds.
- [`test_host_tcl_library.ae`](test_host_tcl_library.ae): Tcl's script
  library under the bridge. A libtcl that was named by `AETHER_TCL_SONAME` must
  initialise (the other specs skip when it does not, which hid the zipfs
  failure: on the old bridge a default Tcl 9 gave "0 passing, 11 skipped" and a
  green run; this spec fails there), and its library is found, readable and
  loads on first use (`clock format`, a time zone, an encoding, `parray`,
  `package require http`).
- `containment/test_tcl_library.sh`: that spec run three ways: with the build's
  own library, with the library copied out of wherever it is, and with
  `TCL_LIBRARY` naming the copy (it must win over the zip).
- `containment/test_library_first_use.sh`, `containment/test_zipfs_mount.sh`:
  what a script can do with Tcl's library and with a zip archive once the
  sandbox is in force, on either side of the zipfs line ([Tcl 9 and its
  zipfs](#tcl-9-and-its-zipfs)); the second runs only for a build with zipfs.
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: the
  containment table and the parity table above.

Both specs skip when libtcl cannot be loaded (on GitHub Actions a skip is a
failure). They assert on return codes and Tcl-side state, never on the
interpreter's stdout. `../tests/run.sh` runs everything.

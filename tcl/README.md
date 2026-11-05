# tcl: embedded Tcl

`import tcl` lets an Aether program run Tcl. It is the bridge that was
aether's `contrib.host.tcl`, moved here ([PROVENANCE.md](PROVENANCE.md)): the
same functions under the same names, `tcl.run`, `tcl.run_sandboxed`,
`tcl.vm_new` and the rest. It loads the machine's libtcl at run time and
needs no Tcl dev package (no `tcl.h`) to build. Tcl 8.5 and 8.6, and 9.0 in
principle ([Loading libtcl](#loading-libtcl) says what was run).

## Four ways to run Tcl from Aether

From least to most contained. The first three run Tcl inside your program,
and the fourth runs it in a separate, locked-down process. All four stay
supported: pick by how far you trust the script, not by which is newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Tcl runs | your process | your process | your process | its own process (`tcl/worker`) |
| Your host commands | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Tcl's own `open`, `socket`, `exec` and `load` | not checked | **not checked** | checked against the grants (Linux), except `$env` | refused, beyond what the host hands over |
| Tcl's library, loaded on first use (`parray`, `clock format`, encodings) | yes | yes | only with a read grant on Tcl's library directory | only what the worker's warm-up script used before its lockdown |
| Platforms | everywhere libtcl loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0; `exec` held from 0.884.0 | ae 0.878.0 (`std.guest`) |

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

A script granted only `HOME`, run on CachyOS (Tcl 8.6.16) under ae 0.889.0,
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
  all, preload or not: `can't find package msgcat 1.6`.)

It is a library interposer, not a kernel boundary: a raw system call made
without libc is not seen (Tcl itself cannot make one without a compiled
extension), and macOS has no equivalent. `exec` is held from ae 0.884.0
(aether #3580); before that it escaped, because Tcl starts a program with
`vfork` and `execvp`, and glibc's `execvp` reaches `execve` inside libc,
past the preload's wrapper. The containment table below has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("tcl") {
        grant_fs_read("/usr/lib/tcl8.6/*")          // Tcl_Init sources init.tcl from here
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
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | from ae 0.884.0: matches; before: differs: exec-bin-sh escapes | differs: fork left out |
| `test_stunt_host_checked` | `-` | matches | matches | differs: half-frame left out |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | differs: dlopen-libc-by-name left out, dlopen-libc-by-path left out |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: connect-ipv4-mapped left out, open-etc-passwd not held | matches | differs: connect-192.0.2.1 stricter, connect-ipv4-mapped stricter, syscall-socket left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped left out | matches | matches |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home left out, env-cached not held, connect-192.0.2.1 not held, connect-ipv4-mapped left out, exec-bin-sh not held | from ae 0.884.0: differs: getenv-home left out, env-cached escapes; before: differs: getenv-home left out, env-cached escapes, exec-bin-sh escapes | differs: getenv-home left out, fork left out, dlopen-libc-by-name left out, dlopen-libc-by-path left out, syscall-fn-openat left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | differs: write-granted-file stricter |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Tcl 8.5.9
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Tcl 8.6.16
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Tcl 8.6.16
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Tcl 8.6.16
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Tcl 8.6.16
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's
with a read grant on Tcl's library directory added, below); the containment
table below has each row. The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it
say which host produced which column.

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
CachyOS x86_64 with Landlock (Tcl 8.6.16; ae 0.889.0 and 0.878.0), and in
CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`open`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`open`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`open .. w`) | per policy | **not held** | refused under every policy |
| `getenv-home` | (left out: Tcl never calls `getenv`) | (left out) | (left out) |
| `env-cached` (`$::env(HOME)`) | **escapes**: the copy Tcl took in `Tcl_Init` | **not held** | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1` (`socket -async`) | per policy | **not held** | refused under every policy: the worker's own lockdown has no tcp grant |
| `connect-ipv4-mapped` (`socket -async ::ffff:192.0.2.1`) | per policy | (left out on macOS: Tcl 8.5 there resolves no IPv6 literal) | refused under every policy |
| `exec-bin-sh` (`exec`) | per policy, from ae 0.884.0; before it **escapes**: `execvp` reaches `execve` inside libc | **not held** | refused under every policy: the worker can create no file, and `exec` makes one before it forks |
| `fork` | (left out: Tcl has no fork) | (left out) | (left out) |
| `dlopen-libm` (`load`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`load`) | (child-only row) | (child-only row) | (left out: the worker **dies** on the refusal, below) |
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
  calls under them, and `exec` from ae 0.884.0. `$env` is never held
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
  worker's second argument; see below). No row reads there.
- **`load` of a refused `dlopen` kills the worker.** The preload refuses a
  handle on libc by returning NULL from `dlopen` without setting
  `dlerror()`. Tcl 8.6.16's `TclpDlopen` formats that `dlerror()` with
  `Tcl_ObjPrintf("%s")` and dereferences NULL: the worker dies with SIGSEGV
  and the host reports `KILLED`. The attempt is stopped, but the script
  never gets a refusal it could catch, so the two `dlopen-libc-*` rows
  cannot be answered "refused" in Tcl and are left out. (A script in Tcl
  that runs `load` on a library the preload refuses crashes the same way
  in-process.)
- **A refused `socket()` reads as "host is unreachable".** In the contained
  child with no tcp grant, seccomp refuses `socket(AF_INET, SOCK_STREAM)`
  with `EPERM`; Tcl tries each address, finds none it can open, and reports
  `EHOSTUNREACH`. `checks.tcl` takes that, from the synchronous
  `socket -async` of an address literal, as the refusal it is. A connect
  that is let through and then finds no route fails later (`fconfigure
  -error`), not there.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row runs expecting the call to succeed, so the day the preload
  holds it that test fails and this table is brought up to date. The
  `exec-bin-sh` row is checked on either side of ae 0.884.0
  (`AE_SPAWN_PIN` in `ci/versions.env`), from the ae the tests run on.

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
   `libtcl9.0.so.0`, `libtcl9.0.so`, `libtcl.so`, Homebrew's tcl-tk
   (`/opt/homebrew/opt/tcl-tk/lib/libtcl8.6.dylib`, then `libtcl9.0.dylib`,
   and the same under `/usr/local/opt`), and `/usr/lib/libtcl.dylib`, which
   is macOS's own Tcl 8.5 (in the dyld cache, with no file on disk). Debian
   and Ubuntu ship `libtcl8.6.so.0` in the runtime package (`tcl8.6`) and the
   unversioned `libtcl.so` only in the dev package, which is not needed.

`../tests/tcl_env.sh` is one way to choose among several, for the tests:
`TCL_VERSION=8.6` (or `9.0`, `8.5`) asks for that one, else 8.6 then 9.0
then 8.5. A machine with its own layout:

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

What has run: **8.5.9 on macOS** (the system's) and **8.6.16 on CachyOS**,
both the specs and every containment row. **Tcl 9.0 has not been run
against this build** (no Tcl 9 was available where it was made); the
bridge as it stood in aether was used with 9.0 on macOS/Homebrew, and what
this move changed (the widths above) is argued, not exercised. CI runs
macOS with Homebrew's tcl-tk.

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
the worker needs nothing else to start. After the lockdown nothing can be
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
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: the
  containment table and the parity table above.

Both specs skip when libtcl cannot be loaded (on GitHub Actions a skip is a
failure). They assert on return codes and Tcl-side state, never on the
interpreter's stdout. `../tests/run.sh` runs everything.

# go: Go as a separate process

`import go` lets an Aether program run a Go program. It is the bridge that
was aether's `contrib.host.go`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `go.run`, `go.run_script`,
`go.run_sandboxed`, `go.run_script_sandboxed` and the two `_with_map`
variants.

Go is the one language here that is never in your process. Its runtime and
scheduler cannot share a process with another runtime, so every way below
starts a **child**: a built Go binary (`go.run(binary)`), or `go run
script.go` (`go.run_script(path)`), which makes the `go` toolchain the child
and your program its child in turn. The bridge loads nothing and links
nothing: it needs the `go` command only to build your program (Go 1.18 or
later; the tests use `go.mod` with `go 1.18`), and, for `run_script`, to run
it. A built binary needs no Go installation at all.

What that does to the "four ways" the other languages offer: the first
three are one process each time, differing in what is around it, and the
fourth is the only one where Go speaks to its host, because a child has no
host functions of the kind an embedded interpreter has (a subprocess
returns an exit status; the bridge gives it no arguments, captures nothing
and has no way to call back).

## Four ways to run Go from Aether

From least to most contained. All four stay supported: pick by how far you
trust the program, not by which is newest.

| | 1. Child, no containment | 2. Child under a grant list | 3. The same, with the preload (Linux) | 4. Contained child |
|---|---|---|---|---|
| Where Go runs | its own process (`go.run`) | its own process (`go.run_sandboxed`) | its own process (the same call) | its own process (`go/worker`, started by `guest.launch`) |
| How the grants reach it | none | `AETHER_SANDBOX_SHM`; nothing on macOS: it does not start | `LD_PRELOAD` and `AETHER_SANDBOX_SHM` in the child's environment | Landlock and seccomp from the grant list, the preload, and a socket |
| Your host functions | none | none | none | checked against the grants; each call is a message the host answers |
| Go's own file, socket and process calls | not checked | not checked | **not checked**: Go makes the system calls itself | refused, beyond what the grants allow |
| What it calls in libc (cgo) | not checked | not checked | checked against the grants (cgo's `getenv`; `dlopen` and `fork` are child-only rows) | checked, and the kernel layer beside them |
| Platforms | everywhere Go runs | Linux (macOS: starts nothing, returns -1) | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0 | ae 0.878.0 (`std.guest`) |

Ways 2 and 3 are one call. aether's bridge has no sandboxed run without the
preload: `go.run_sandboxed` always starts the child with
`libaether_sandbox.so` in `LD_PRELOAD`, and where the library does not
exist (macOS) it starts nothing. They are separate columns because they are
separate columns of the tables below, and because that is what is true on
macOS and on Linux.

### 1. A plain child: no containment

```aether
import go

main() {
    // go build -o build/go-hello ./go/examples/hello
    rc = go.run("build/go-hello")                 // the child's exit status; -1 if it could not start
    println("exit ${rc}")
}
```

`go.run(binary)` forks and executes the binary with the host's environment
and its standard streams. It returns the child's exit status, or -1 when
the program cannot start (no such file, no permission) or the child was
killed by a signal: never a 127 that a program which ran could also give.
`go.run_script(path)` does the same with `go run path`, using `$GO_BIN` or
`go` on `PATH`; note that `go run` exits 1 for a program that exits with any
non-zero status (and prints `exit status N` itself). Nothing contains the
child: it has everything the host process has.
[examples/hello](examples/hello/main.go) is the program, and
[examples/ways.ae](examples/ways.ae) starts it the first three ways.

### 2. A child under a grant list

```aether
import go
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    rc = go.run_sandboxed(reader, "build/go-hello")
    println("exit ${rc}")
}
```

What `go.run_sandboxed` does, read from `aether_host_go.c`:

1. It looks for `libaether_sandbox.so` (`AETHER_SANDBOX_PRELOAD`, beside the
   program, under `AETHER_HOME`). If there is none, which is always the case
   on macOS, it prints why and **returns -1 having started nothing**: it fails
   closed, as `guest.launch` does there. (The README of aether's copy says
   "on macOS the Go subprocess runs without preload and grants are advisory
   only"; the code never did that, and `test_host_go.ae` and
   `containment/test_ways.sh` check what it does.)
2. It writes the grant list, as `category:pattern` lines, to POSIX shared
   memory (`/aether_host_go_<pid>`). An empty grant list is written too, and
   means nothing is granted (aether's copy returned -1 for it: see
   [PROVENANCE.md](PROVENANCE.md)).
3. It forks, and in the child, calling only `execve`, starts the program
   with the host's environment plus `LD_PRELOAD=<the library>` and
   `AETHER_SANDBOX_SHM=<the segment>` (and `AETHER_MAP_SHM` with a map).
4. The host waits, reads back what the child wrote to the map, removes the
   segments and returns the exit status.

No grant is checked in the host's process. The one thing the enclosing
level does is gate the start: inside a `sandbox.enforce` block that grants
no `exec`, the call returns -1 and nothing starts
([containment/test_ways.sh](containment/test_ways.sh) and the nested
test). The child is held by whatever the preload holds, which is the next
section.

### 3. The same, with the preload: what it holds in a Go child

On Linux the call above is already this. `libaether_sandbox.so` interposes
libc: `open`, `connect`, `execve`, `fork`, `getenv`, `dlopen`, `system` and
the rest. A Go program, though, mostly does not call libc.

- **A static Go binary** (built with `CGO_ENABLED=0`, or importing neither
  cgo nor `net` nor `os/user`, whose cgo versions make a binary dynamic) has
  no dynamic loader at all, so `LD_PRELOAD` is ignored. Nothing the preload interposes is
  ever called.
- **A dynamic one** (cgo) loads the preload, but `os.Open`, `net.Dial`,
  `os/exec` and `syscall` make the `openat`, `connect`, `clone` and
  `execve` system calls themselves, with the `syscall` instruction, not
  through libc. So they are not seen either. Only what a program reaches
  through C is held: cgo's `getenv`, and in the contained child `dlopen` and `fork`.

So for Go the preload is a way to hold cgo calls, and a library
interposer: not a kernel boundary, and not a boundary for Go. `os.Getenv` is
also never held: the Go runtime copies the environment when the program
starts. The containment table has every row.

```sh
GUEST_LANG=go sh tests/run.sh            # the rows, ways 2 and 3 as "inproc"
```

Pass what the child needs through the environment: the bridge gives it no
arguments. `containment/host.ae` does it for the checks (`GHL_ROW`,
`GHL_EXPECT`). For typed values both ways there is the shared map
(`go.run_sandboxed_with_map(perms, binary, token)`): `AETHER_MAP_SHM` names a
POSIX shared memory segment in the format `go/rows/cgo.go` reads and writes
(frozen-count, then key/value pairs; the pairs after the frozen inputs are
read back as outputs when the child exits). On macOS the child cannot write
outputs back: macOS gives a shared memory object one size, set when it is
made, and the host sizes it for the inputs.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox

main() {
    // CGO_ENABLED=0 go build -o build/go-example-worker ./go/examples/worker
    perms = sandbox.new("go") {}              // a static Go worker needs nothing from disk
    g, err = guest.launch(perms, "build/go-example-worker", null, "go", "")
    if err != "" { println("not started: ${err}") return }     // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    guest.start(&g, "")
    while true {
        e = guest.next(&g)
        if e.kind == "SAY" {
            println(guest.printable(e.text))
        } else if e.kind == "CALL" {
            guest.answer(&g, guest.json_string("hello, Go"))
        } else {
            println("${e.kind} ${e.code} ${guest.printable(e.text)}")
            break
        }
    }
}
```

On CachyOS this prints `the host answered VALUE "hello, Go"`, then
`open /etc/passwd: permission denied`, then `DONE 0`
([examples/contained.ae](examples/contained.ae) is the host,
[examples/worker](examples/worker/main.go) the worker, and
`containment/test_ways.sh` runs both).

The worker is a Go program that speaks `std.guest`'s protocol over the
socket `guest.launch` gives it (`AETHER_IPC_FD`): `std.ipc` frames of
`VERB text`, JSON arguments. Package [`guestchild`](guestchild/child.go)
is that protocol, the counterpart of `std.guest.child` for a worker in Go:
`Open`, `ReadScript` (the host's `MOUNT`, `PROBE`, `LOGLEVEL` and `SCRIPT`),
`Say`, `Audit`, `Log`, `CallHost` (a mounted function; a granted file
arrives as a descriptor and is read for you), `Poll` (a `CANCEL`), `Done` and
`Error`. It is the only thing a worker needs from this repository, and it
imports nothing but the standard library.

Two walls hold the worker, as for the other languages, and one of them is
missing:

- **The process sandbox is there.** `guest.launch` is `sandbox.spawn_socket`:
  Landlock and seccomp from the grant list, plus the preload, and the
  socket. These are the kernel's, so they hold Go's own system calls, which
  is what the preload cannot do for Go (the `os.Open` of `/etc/passwd` above
  is `EACCES` from Landlock). A static worker (`CGO_ENABLED=0`) is the
  smallest case: it needs nothing from disk after it starts.
- **The worker's own lockdown is not.** `std.guest.child.run` enters
  `sandbox.enforce` with no grants before the script's first step, so
  python-worker and the other Aether workers have *less* authority than
  the grant list they were launched with. A Go program cannot enter an
  Aether sandbox block. So the grants on the spawn are the Go worker's own
  authority: a file Landlock lets through is a file Go can open itself, and
  a `tcp` grant is a socket Go can make. **Launch a Go worker with the
  smallest grant list that lets it work**, and let it ask the host for the
  rest (`CallHost`, a host's `grant_file`). The containment table shows
  where that differs from the other languages.

The host can cancel at a deadline and kills a worker that stops answering
(`silent`, `oversize` and `half-frame` are the rows that hold it); the
cancel reaches a Go worker only if it calls `Poll`, or is waiting in
`CallHost`: Go has no interrupt hook for a running goroutine.

`worker/worker.go` is a complete worker for the tests, with the rows of
stunt's catalogue compiled in. A Go program is compiled, so there is no
script to run: the worker takes the host's `SCRIPT` as a command list for
the Go code in it (`say`, `call`, `fact`, `row`, and the four host-checked
rows), and a worker of your own does whatever it likes with `h.Source`.
Built with cgo, as the tests build it, because the libc rows are cgo's; the
`containment/test_ways.sh` rows also run a `CGO_ENABLED=0` build. Build the
worker once with `go build -o go-worker ./go/worker` from `go/`.

## Stunt parity

Each of stunt's tests, as Go comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how. Column 2 is macOS, where the bridge's
sandboxed run starts nothing and the rows run through `go.run`, to show what
is not held; column 3 is Linux with the preload.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | differs: open-etc-passwd escapes | matches |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: binding-read-granted-file left out, open-etc-passwd not held, binding-read-etc-passwd left out, write-granted-file not held, binding-getenv-home left out | differs: binding-read-granted-file left out, open-etc-passwd escapes, binding-read-etc-passwd left out, write-granted-file escapes, binding-getenv-home left out | differs: getenv-home stricter, env-cached stricter |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | differs: exec-bin-sh escapes, open-etc-passwd escapes | matches |
| `test_stunt_host_checked` | `-` | differs: spin left out | differs: spin left out | matches |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | differs: open-etc-passwd escapes | matches |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, binding-read-etc-passwd left out, open-granted-file not held, binding-read-granted-file left out | differs: open-etc-passwd escapes, binding-read-etc-passwd left out, open-granted-file escapes, binding-read-granted-file left out | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | differs: open-etc-passwd escapes | matches |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | differs: connect-192.0.2.1 escapes, connect-ipv4-mapped escapes | differs: connect-192.0.2.1 escapes, connect-ipv4-mapped escapes |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home not held, env-cached not held, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held, binding-read-etc-passwd left out, binding-read-granted-file left out, binding-getenv-home left out, callback-greet left out, callback-add left out, callback-unregistered left out, map-get left out, map-set left out, map-overwrite-input left out | differs: open-etc-passwd escapes, open-granted-file escapes, write-granted-file escapes, env-cached escapes, connect-192.0.2.1 escapes, connect-ipv4-mapped escapes, exec-bin-sh escapes, binding-read-etc-passwd left out, binding-read-granted-file left out, binding-getenv-home left out, callback-greet left out, callback-add left out, callback-unregistered left out | matches |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held, binding-read-granted-file left out | differs: open-granted-file escapes, binding-read-granted-file left out | matches |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Go 1.27.3
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Go 1.27.3-X:nodwarf5
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Go 1.27.3-X:nodwarf5
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Go 1.27.3-X:nodwarf5
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Go 1.27.3-X:nodwarf5
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies. The table is generated
from a run (`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it say
which host produced which column. Go reaches the system the same way before
and after ae 0.884.0 (`AE_SPAWN_PIN`), so no cell has two sides.

Read the table with these in mind: a **left out** row is one a child
process cannot do, not one that failed (below); an **escapes** row is one
the preload is there to hold and does not, for Go; a row that **matches** in
column 4 mostly does because the policy granted it, not because it was
refused.

## Containment table

What a Go program gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Go way
([rows](rows/rows.go), run by [containment/checks.go](containment/checks.go)
for ways 1 to 3 and by [worker/worker.go](worker/worker.go) for way 4),
under the policies of aether's `tests/integration/contrib_host_stunt`
(`../tests/test_stunt_*.sh`). "Per policy" means granted where the policy
grants it and refused where it does not, as stunt expects. The columns are
the ways above: "child, Linux (preload)" is ways 2 and 3, "child, macOS"
is way 2 where there is no preload (the bridge starts nothing; the rows
here ran through `go.run`), and "contained child" is way 4. Every cell is
a test outcome (`../tests/run.sh`): checked on macOS arm64 (Go 1.27) and
CachyOS x86_64 with Landlock (Go 1.27; ae 0.889.0 and 0.878.0), and in CI
(Go 1.22).

| Row | Child, Linux (preload) | Child, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd`, `open-granted-file`, `write-granted-file` (`os.ReadFile`, `os.WriteFile`) | **escapes**: the `openat` system call is Go's own, not libc's | **not held** | per policy: Landlock from the spawn's grants, `EACCES` otherwise |
| `getenv-home` (cgo `C.getenv`) | per policy | **not held** | refused under every policy: the worker has no environment; `getenv` is a host function |
| `env-cached` (`os.Getenv`) | **escapes**: the copy the Go runtime took at start | **not held** | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1`, `connect-ipv4-mapped` (`syscall.Connect`, the call `net` makes) | **escapes**: Go's own `connect` | **not held** | refused without a tcp grant (seccomp refuses `socket`); with **any** tcp grant, **every address escapes**: Landlock does not filter addresses and the address check is the preload's, on libc's `connect` |
| `exec-bin-sh` (`os/exec`) | **escapes**, on every ae: Go's `clone` and `execve` are its own | **not held** | per policy (seccomp lets `execve` through only with an exec grant) |
| `fork` (cgo `fork`) | (child-only row) | (child-only row) | per policy: the preload refuses it without a fork grant |
| `dlopen-libm` (cgo `dlopen`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (cgo `dlopen`) | (child-only row) | (child-only row) | per policy: refused without `native=*`, by the preload |
| `syscall-fn-openat` (libc's `syscall()`, through cgo) | (child-only row) | (child-only row) | refused |
| `syscall-openat`, `syscall-socket` (`syscall.Syscall`: the instruction itself; Go's usual way) | (child-only row) | (child-only row) | per policy: Landlock refuses the open, seccomp lets `socket(AF_INET)` through only with a tcp grant |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home`, `callback-greet`, `callback-add`, `callback-unregistered` (host functions) | **left out**: a child process has none | **left out** | work, per policy; a granted file goes down as a descriptor (`CallHost`) |
| `map-get`, `map-set` (`AETHER_MAP_SHM`) | work (cgo's `shm_open`) | **left out**: `go.run_sandboxed_with_map` starts nothing, and a macOS shared memory object cannot grow to take an output | work (the host's map, over `CallHost`) |
| `map-overwrite-input` | refused (the frozen inputs are read-only; the host checks its own map afterwards) | **left out** | refused (the host's `aether_map_put` answers false) |
| `spin` | **left out**: the bridge waits for the child to exit | **left out** | cancelled at the deadline: the worker calls `Poll` |
| `silent` (never looks) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `Say`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `half-frame` (a frame's header and part of its payload, written to `AETHER_IPC_FD`) | (child-only row) | (child-only row) | killed: the host times out inside the frame |

Built without cgo (`CGO_ENABLED=0`, or no C compiler) the libc rows are
left out: `getenv-home`, `fork`, the `dlopen` rows, `syscall-fn-openat` and
the `map-*` rows, with the reason printed. A catalogue run on a machine like
that is smaller, and CI never runs one.

What the table says:

- **A plain Go child is not held by the preload, at all, on Linux** except
  where it calls C. `go.run_sandboxed` is no sandbox for a Go program that
  does not use cgo, and a weak one for one that does: the table has seven
  escapes in the first column and one held row (`getenv-home`). This is the
  main finding of the move, and aether's README for the bridge ("grants apply
  to libc calls the Go binary makes ... exactly the same way they apply to
  any other process") was true only of the second half.
- **The contained child holds Go's system calls**, because the kernel does
  (Landlock for files; seccomp for sockets and for `execve`), and those are
  the calls Go makes. It does not hold *less* than python-worker holds, it
  holds what the grant list says: with no grants (`test_stunt_no_grants`)
  everything is refused, and the table's "matches" cells are mostly policies
  that granted the row. The difference from the other languages is that a
  grant is not narrowed again inside the worker (above, "4. A contained
  child"). Where a row is refused, or comes out the same however the grants
  are chosen, it is because the worker has no environment (`getenv-home`,
  `env-cached`).
- **A tcp grant is a grant for every address** in the contained child. Any
  `tcp` grant opens `socket(AF_INET)` for the process (seccomp cannot see
  an address), Landlock's network rules are by port, and the per-address
  check is the preload's on libc's `connect`, which Go's connect does not
  call. `test_stunt_net_other` shows it (column 4: `connect-192.0.2.1`
  escapes). A Go worker that must reach one host should be given no tcp
  grant and ask the host for a connection.
- **The nested test** (`test_stunt_nested`) is only partly the other
  languages' test. The bridge hands the child the *inner* grants and
  ignores the enclosing level, except that a block with no `exec` grant
  stops the child starting at all (`go.run_sandboxed` returns -1). The host
  adds the exec grant to the outer level, so the rows run, and every one of
  them is an open that escapes the preload anyway.
- The tests hold these cells, including the gaps: an **escapes** or
  **not held** row runs expecting the call to succeed, so the day the
  preload holds it that test fails and this table is brought up to date.

### Beside the catalogue

[containment/test_ways.sh](containment/test_ways.sh) holds what the four
ways are for a Go program, and the programs in this README:

- way 1: `go.run` is not contained (cgo `getenv`, refused under the preload
  with no grants, succeeds under `go.run`);
- way 2 on macOS: `go.run_sandboxed` returns -1 and starts nothing;
- way 3 with a static binary (`CGO_ENABLED=0`): `ldd` says it is not
  dynamic, and `open-etc-passwd` and `exec-bin-sh` succeed under the
  preload and no grants;
- way 4 with a static worker: Landlock and seccomp refuse the open, the
  socket, the connect and the shell with no libc in the process, the host
  functions work, and a granted file opens;
- `examples/ways.ae` and `examples/contained.ae`: the outputs this README
  quotes.

## Gaps

What this move did not do, and what stays open:

- **Nested grants.** `go.run_sandboxed` inside `sandbox.enforce` gives the
  child the inner list and ignores the outer (above). A subprocess bridge
  should hand the child the intersection; aether's `contrib/host/aether`
  bridge has the same serialiser and the same gap.
- **The preload on a Go child.** Nothing in this repository can make the
  preload hold Go's own system calls. The kernel layer (way 4) does; for a
  child you started yourself, `unshare` or a container is the Linux answer
  and is outside this package.
- **Arguments and output.** The bridge passes the child no arguments and
  does not capture its output; both go through the environment, the shared
  map and the child's inherited standard streams.
- **A map output on macOS.** The child cannot write outputs back to a
  `_with_map` run on macOS (and the run does not start there anyway);
  `aether_shared_map_to_shm` would need to size the object for outputs.
- **Windows.** The bridge's stubs return -1 with a message, as aether's did;
  untested here.
- **A static worker and a granted shell.** With `exec=/bin/sh,fork`, the
  cgo worker starts `/bin/sh` and a `CGO_ENABLED=0` one gets `EACCES` from
  its `execve`. Not investigated; `lang.sh` records it as the one row where
  a build without cgo differs from the table beyond the rows it leaves out.
- **Go below 1.18** is not tried; the modules say `go 1.18`.

## Finding the toolchain

`go.run_script` runs `$GO_BIN`, else `go` on `PATH`. The tests find the
toolchain in `../tests/go_env.sh`: `GO`, then `go` on `PATH`, then
Homebrew's and the official tarball's locations, and use `GO_BIN` for the
bridge. cgo must work for the libc rows (`CGO_ENABLED=1` and a C compiler,
which `go env` and `$CC` say); the tests check both and leave the libc rows
out without.

```sh
GUEST_LANG=go sh tests/run.sh              # specs, rows, parity
GUEST_LANG=go GO=/usr/local/go/bin/go sh tests/run.sh
```

## Implementation notes

- The bridge is `aether_host_go.c`, unchanged but for the include paths and
  the empty grant list ([PROVENANCE.md](PROVENANCE.md)). Everything the
  child execs with is made before the fork (the environment with the
  preload, the grant segment and the map), and the child calls only
  `execve`, which a child of a threaded program may do safely.
- A program that cannot be executed is -1, found through a close-on-exec
  pipe the child writes its `errno` to when `execve` fails.
- `go/` is one Go module (`go.mod`, `go 1.18`, no dependencies): `rows`
  (the catalogue), `guestchild` (the std.guest protocol), `worker` and
  `containment` (the two programs the tests build), and `examples`.
- `rows` has cgo files (`getenv`, `dlopen`, `fork`, libc's `syscall()`,
  `shm_open`) and non-cgo stubs; the Linux raw system call rows are
  `syscall.Syscall` and need no machine code.
- The worker reads frames with exact reads, because a `FILE` reply is a
  frame followed by one byte that carries the descriptor (`SCM_RIGHTS`):
  buffered reading would take the byte and lose the descriptor.

## Tests

- [`test_host_go.ae`](test_host_go.ae): the bridge's calls: exit statuses,
  a program that cannot start (-1), `run_sandboxed` with grants and with
  an empty list on Linux (and -1 on macOS), and `run_script` with a real
  `go run`. aether had no test of this bridge ("this host has no dedicated
  integration test of its own").
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh` and
  `containment/test_ways.sh`: the containment table and the parity table
  above.

`../tests/run.sh` runs everything.

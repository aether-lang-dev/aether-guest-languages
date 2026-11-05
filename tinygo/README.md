# tinygo: in-process Go, from a c-shared library

`import tinygo` lets an Aether program load a Go library and call its
exported functions directly: no subprocess, no IPC, no marshalling. It is
the bridge that was aether's `contrib.host.tinygo`, moved here
([PROVENANCE.md](PROVENANCE.md)): the same functions under the same names,
`tinygo.load`, `tinygo.call_int_int_int`, `tinygo.call_str_str_owned` and
the rest. It `dlopen`s a library built with `go build -buildmode=c-shared`
and calls its exported symbols; it needs no Go toolchain to build or to
run, only to make the library.

> **Toolchain note.** The module is named `tinygo` for the small-footprint,
> embedded-friendly story it started with, and the name stays for
> compatibility. But `tinygo build -buildmode=c-shared` supports only wasm
> targets today ("buildmode c-shared is only supported on wasm at the
> moment"). The **native** libraries this bridge loads come from standard
> `go build -buildmode=c-shared` (Go 1.21 or later, with cgo and a C
> compiler). The bridge only does `dlopen` and `dlsym`: it does not care
> which compiler produced the library. The tests here use `go`, and
> `tinygo` only if one is installed, for nothing but the version line.

Go is not a scripting language, and that changes the containment story
more than for any other language in this repository. A library built with
`-buildmode=c-shared` carries **its own runtime**: a garbage collector, a
scheduler and the threads under it, started when the library is loaded.
The Go code is machine code in your process. On Linux Go makes its system
calls with the `syscall` instruction itself, so nothing that interposes on
libc, which is what aether's sandbox preload does, sees an `os.Open`, a
`net.Dial` or an `exec.Command`. [Four ways](#four-ways-to-run-go-from-aether)
below says what each way holds for it, with measurements.

## Four ways to run Go from Aether

From least to most contained. The first three run the Go library inside
your program, the fourth runs it in a separate, locked-down process. All
four stay supported: pick by how far you trust the library, not by which
is newest.

| | 1. Load | 2. Load, call in a sandbox block | 3. Load + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Go runs | your process | your process | your process | its own process (`tinygo/worker`) |
| Your host functions | none: the bridge has none | none | none | none |
| What the bridge itself checks | nothing | nothing | nothing | nothing |
| Go's own file, socket and process calls (`os`, `net`, `os/exec`, `syscall`) | not checked | **not checked** | **not checked on Linux**: raw system calls, past libc | held by the kernel layer the policy builds (Landlock, seccomp) |
| Calls Go makes through libc, by cgo (`getenv`, `dlopen`, `syscall()`) | not checked | **not checked** | checked against the grants, on the thread that holds the level | refused beyond the policy, on every thread |
| `os.Getenv`, `os.Environ` | the process's | the process's | **not held**: Go's copy from load time | the worker has no environment |
| Goroutines on other threads | | | **not held**: grant levels are per thread | |
| Platforms | everywhere the library loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0 | ae 0.878.0 (`std.guest`) |

The short version: for a Go library, **ways 2 and 3 are not containment**.
Way 2 adds nothing, because the bridge checks nothing and the library cannot
call anything the block holds. Way 3 holds only the calls Go makes through
cgo. The kernel layer of way 4 is the only thing that holds what Go does
with the `syscall` instruction, and it is the policy's, not the worker's
own lockdown.

### 1. Load: Go in your process, no containment

```go
// tinygo/examples/greet.go
package main

import "C" // required by -buildmode=c-shared

//export Answer
func Answer() int32 { return 42 }

//export Add
func Add(a, b int32) int32 { return a + b }

//export Greet
func Greet(name *C.char) *C.char {
	return C.CString("hello, " + C.GoString(name)) // malloc'd by cgo: call it with call_str_str_owned
}

func main() {} // c-shared still requires a main(); an empty body is fine
```

```sh
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.so    greet.go   # Linux
CGO_ENABLED=1 go build -buildmode=c-shared -o libgreet.dylib greet.go   # macOS
```

```aether
// tinygo/examples/way1_embed.ae
import tinygo
import std.os

main() {
    handle, err = tinygo.load(os.getenv("TINYGO_LIB"))
    if handle == null {
        println("load failed: ${err}")
        return
    }
    println("Answer = ${tinygo.call_int_void(handle, "Answer")}")
    println("Add(2, 40) = ${tinygo.call_int_int_int(handle, "Add", 2, 40)}")
    println(tinygo.call_str_str_owned(handle, "Greet", "world"))
    tinygo.unload(handle)
}
```

which prints `Answer = 42`, `Add(2, 40) = 42` and `hello, world`
([containment/test_examples.sh](containment/test_examples.sh) builds and runs it). The name on the
Aether side is the exported C symbol, case-sensitive; mark each function
with `//export`. `Greet` returns a `C.CString`, which cgo allocates with
`malloc`: the `_owned` call copies it and frees the C pointer
([Memory ownership](#memory-ownership)).

### 2. Load, and call in a sandbox block

```aether
// tinygo/examples/way2_sandbox.ae (the Go side is examples/probe.go)
import tinygo
import std.sandbox
import std.os

main() {
    handle, err = tinygo.load(os.getenv("TINYGO_LIB"))
    reader = sandbox.new("reader") { grant_env("HOME") }
    sandbox.enforce(reader) callback {
        println("os.Getenv(USER)            ${tinygo.call_s_s_owned(handle, "GetenvGo", "USER")}")
        println("os.ReadFile(/etc/passwd)   ${tinygo.call_s_s_owned(handle, "OpenFile", "/etc/passwd")}")
        println("os/exec /bin/sh            ${tinygo.call_s_v_owned(handle, "RunShell")}")
        // ... the rest in the file
    }
}
```

Unlike the scripting languages here, the bridge has **no sandboxed entry
point** (`run_sandboxed`): `sandbox.enforce` is the host's own. Read the
bridge's C ([aether_host_tinygo.c](aether_host_tinygo.c)) and `std.dl`'s: the
only things they do are `dlopen`, `dlsym` and the call through the function
pointer, and neither consults a grant (`grep sandbox` finds nothing in
either). So what the block holds is **Aether code in the block**, and a Go
function has no way to call that: it runs as machine code, not through
Aether's `std`. The grants described by `reader` do not reach it at all:

```
os.Getenv(USER)            paul
libc getenv(USER) (cgo)    paul
libc getenv(HOME) (cgo)    /home/paul
os.ReadFile(/etc/passwd)   opened
os/exec /bin/sh            ran /bin/sh
net.Dial 192.0.2.1:443     let through (timed out)
dlopen libc (cgo)          loaded
```

(CachyOS, ae 0.889.0, Go 1.27.3, no preload; macOS prints the same.) Use
this way for a library you trust, where the block's only role is to hold
what the *host's* code does around the call. It is also how the stunt
tests below call the library, so that their policies are in force.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits between
the program and libc (Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

The same program and library, granted only `HOME`, run on CachyOS (Go
1.27.3, ae 0.889.0), first without the preload and then with it:

| Go does | without | with `LD_PRELOAD` |
|---|---|---|
| `os.Getenv("USER")` | `paul` | `paul`: **not held** |
| `C.getenv("USER")` (cgo, libc's) | `paul` | `NULL` |
| `C.getenv("HOME")` (granted) | `/home/paul` | `/home/paul` |
| `os.ReadFile("/etc/passwd")` | opens it | opens it: **not held** |
| `exec.Command("/bin/sh")` | runs it | runs it: **not held** |
| `net.DialTimeout` to 192.0.2.1:443 | goes out | goes out: **not held** |
| `C.dlopen("libc.so.6")` (cgo) | loads | loads (stunt's `dlopen-libc-*` are child rows: no in-process hold) |

This is the finding for Go. The preload interposes on **libc**, and Go does
not call libc: on Linux the `os`, `net`, `os/exec` and `syscall` packages
make their system calls with the `syscall` instruction directly (it is
what lets a Go binary be static). So the file, the socket, the shell and
the fork a Go library reaches for all go past the preload, **under every
policy**, and only what the library does through cgo is held:
`C.getenv`, `C.dlopen`, `C.syscall`. `os.Getenv` and `os.Environ` are not
held either, for the reason they are not in Python: the copy of the
environment the runtime took when the library loaded.

A second thing in it is Go's own: the grant levels of a `sandbox.enforce`
block are **per thread**, and Go runs goroutines on threads of its own. A
cgo call made from a `go func()` is on a thread with no level in force,
where everything is allowed: stunt's `getenv-home` is refused with no
grants and succeeds from a goroutine (`tinygo/containment/test_threads.sh`,
`getenv-home-goroutine`). The library does not even have to mean to:
anything a goroutine that blocked and was moved to another thread does next
is on a thread the block does not cover.

It is a library interposer, not a kernel boundary, and a Go library is the
case where that matters most. On macOS Go calls libSystem, but there is no
preload there either.

### 4. A contained child, over `std.guest`

```aether
// tinygo/examples/way4_child.ae
import std.guest
import std.sandbox
import std.list
import std.os

main() {
    lib = os.getenv("TINYGO_LIB")
    perms = sandbox.new("go") {
    }
    sandbox.grant_fs_read(perms, lib)   // the loader reads the system's libraries; the library itself is granted
    args = list.new()
    list.add(args, lib)
    g, err = guest.launch(perms, os.getenv("TINYGO_WORKER"), args, "go", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.start(&g, "call GetenvGo USER\ncall GetenvLibc HOME\ncall OpenFile /etc/passwd\ncall RunShell\ncall Connect\ncall DlopenLibc libc.so.6\n")
    while true {
        e = guest.next(&g)
        if e.kind == "SAY" {
            println(guest.printable(e.text))
        } else if e.kind == "CALL" {
            guest.fail(&g, "no host function")
        } else {
            println("${e.kind} ${guest.printable(e.text)}")
            break
        }
    }
}
```

On CachyOS this prints, one line per call:

```
(empty)
(NULL)
open /etc/passwd: permission denied
fork/exec /bin/sh: operation not permitted
dial tcp 192.0.2.1:443: socket: operation not permitted
refused
DONE
```

The Go library runs in `tinygo/worker`, a separate process `guest.launch`
starts under Landlock and seccomp from the policy, with the preload, talking
to the host only over a socket (`std.ipc` frames). The worker loads the
library, which starts the Go runtime, **before** it locks itself down, then
runs the script's calls. Build the worker once with
`ae build tinygo/worker/worker.ae -o tinygo-worker`; details under
[In a contained child](#in-a-contained-child) below. On macOS, where nothing
can contain the child, `guest.launch` refuses and starts nothing.

What holds Go's calls here is the **kernel layer the policy builds**
(Landlock for files, seccomp for sockets, process creation and
exec): it sees the `syscall` instruction, which the preload cannot. Unlike
the scripting languages' workers, whose own lockdown (an empty grant level on
the main thread) is stricter than any policy because their interpreter calls
libc, a Go library's lockdown is the policy's: a file the policy grants to
read, Go reads. That is why the table below has so few rows "stricter in the
child", and why one hole is open in it (`connect-*`, [below](#what-the-table-says)).

## Stunt parity

Each of stunt's tests, as Go comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects,
else the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | differs: open-etc-passwd escapes | matches |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: binding-read-granted-file left out, open-etc-passwd not held, binding-read-etc-passwd left out, write-granted-file not held, binding-getenv-home left out | differs: binding-read-granted-file left out, open-etc-passwd escapes, binding-read-etc-passwd left out, write-granted-file escapes, binding-getenv-home left out | differs: binding-read-granted-file left out, binding-read-etc-passwd left out, getenv-home stricter, env-cached stricter, binding-getenv-home left out |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | differs: exec-bin-sh escapes, open-etc-passwd escapes | matches |
| `test_stunt_host_checked` | `-` | differs: spin left out | differs: spin left out | matches |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | differs: open-etc-passwd escapes | differs: dlopen-libc-by-name stricter, dlopen-libc-by-path stricter |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, binding-read-etc-passwd left out, open-granted-file not held, binding-read-granted-file left out | differs: open-etc-passwd escapes, binding-read-etc-passwd left out, open-granted-file escapes, binding-read-granted-file left out | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | differs: open-etc-passwd escapes | matches |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | differs: connect-192.0.2.1 escapes, connect-ipv4-mapped escapes | differs: connect-192.0.2.1 escapes, connect-ipv4-mapped escapes |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home not held, env-cached not held, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held, binding-read-etc-passwd left out, binding-read-granted-file left out, binding-getenv-home left out, callback-greet left out, callback-add left out, callback-unregistered left out, map-get left out, map-set left out, map-overwrite-input left out | differs: open-etc-passwd escapes, open-granted-file escapes, write-granted-file escapes, env-cached escapes, connect-192.0.2.1 escapes, connect-ipv4-mapped escapes, exec-bin-sh escapes, binding-read-etc-passwd left out, binding-read-granted-file left out, binding-getenv-home left out, callback-greet left out, callback-add left out, callback-unregistered left out, map-get left out, map-set left out, map-overwrite-input left out | differs: binding-read-etc-passwd left out, binding-read-granted-file left out, binding-getenv-home left out, callback-greet left out, callback-add left out, callback-unregistered left out, map-get left out, map-set left out, map-overwrite-input left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held, binding-read-granted-file left out | differs: open-granted-file escapes, binding-read-granted-file left out | differs: binding-read-granted-file left out |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Go 1.27.3
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Go 1.27.3-X:nodwarf5
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Go 1.27.3-X:nodwarf5
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Go 1.27.3-X:nodwarf5
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Go 1.27.3-X:nodwarf5
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's
with a read grant on the library added). The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it
say which host produced which column. The row names the cells list are
[the containment table's](#containment-table); the kinds are:

- **not held**: with no preload (column 2) the row is expected to be
  refused and nothing refuses it, so it runs expecting to succeed;
- **escapes**: the same, under the preload (column 3) or in the child
  (column 4): something is there to refuse it, and the Go code goes past it;
- **stricter**: the contained child refuses where the policy would allow;
- **left out**: the bridge cannot carry the row.

Column 3 is long because it is the finding: every row Go reaches the system
with by itself **escapes** the preload.

## Containment table

What a Go library gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Go way
([containment/checks.go](containment/checks.go): an exported
`check_<row>` function per row, built into a c-shared library and called by
symbol), under the policies of aether's `tests/integration/contrib_host_stunt`
(`../tests/test_stunt_*.sh`). "Per policy" means granted where the policy
grants it and refused where it does not, as stunt expects. The columns are
the ways above: "in-process, Linux (preload)" is way 3, "in-process, macOS"
is way 2 where there is no preload, and "contained child" is way 4. Every
cell is a test outcome (`../tests/run.sh`): checked on macOS arm64 (Go
1.27) and CachyOS x86_64 with Landlock (Go 1.27; ae 0.889.0 and 0.878.0),
and in CI.

| Row | Done in Go with | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|---|
| `open-etc-passwd` | `os.ReadFile` | **escapes** | **not held**: reads it | refused (Landlock) |
| `open-granted-file` | `os.ReadFile` | **escapes** | **not held** | per policy (Landlock) |
| `write-granted-file` | `os.WriteFile` | **escapes** | **not held** | per policy (Landlock) |
| `getenv-home` | `C.getenv` (cgo) | per policy, on the thread with the level | **not held** | refused under every policy: the worker has no environment |
| `env-cached` | `os.Getenv`, `os.Environ` | **escapes**: Go's copy from load | **not held** | refused under every policy: the worker has no environment |
| `connect-192.0.2.1` | `net.DialTimeout` | **escapes** | **not held** | no tcp grant: refused (seccomp, at `socket`); granted: per policy; a grant for **another** address: **escapes** |
| `connect-ipv4-mapped` | `syscall.Socket(AF_INET6)`, `syscall.Connect` | **escapes** | **not held** | the same as `connect-192.0.2.1` |
| `exec-bin-sh` | `os/exec` and `syscall.ForkExec` | **escapes** (both sides of ae 0.884.0) | **not held** | per policy (seccomp: refused without an exec grant, and without a fork grant) |
| `fork` | `clone(SIGCHLD)` by `syscall.RawSyscall` | (child-only row) | (child-only row) | per policy (seccomp: refused without a fork grant) |
| `dlopen-libm` | `C.dlopen` (cgo) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` | `C.dlopen` (cgo) | (child-only row) | (child-only row) | refused under every policy, `native=*` included: the worker's lockdown, on the thread that took it |
| `syscall-fn-openat` | libc's `syscall()` by cgo | (child-only row) | (child-only row) | refused |
| `syscall-openat`, `syscall-socket` | `syscall.Syscall6`, the instruction | (child-only row) | (child-only row) | per policy: Landlock refuses the open, seccomp lets `socket(AF_INET)` through only with a tcp grant |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` | | left out | left out | left out |
| `callback-greet`, `callback-add`, `callback-unregistered` | | left out | left out | left out |
| `map-get`, `map-set`, `map-overwrite-input` | | left out | left out | left out |
| `spin` | a `for {}` loop | left out | left out | cancelled at the deadline: the worker runs the call on a thread of its own and polls |
| `silent` | the same loop, called where the worker cannot poll | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` | a function returning a 1 MiB string | (child-only row) | (child-only row) | killed: the frame is refused |
| `half-frame` | `syscall.Write` of a frame's header and part of its payload to `AETHER_IPC_FD`, then `time.Sleep` | (child-only row) | (child-only row) | killed: the host times out inside the frame |

Left out, because the bridge has nothing to carry them: the `binding-*`,
`callback-*` and `map-*` rows need host functions and a shared map, which
this bridge does not have (a Go library cannot call back into Aether); and
`spin` in-process, because a Go call runs to its end on the caller's thread
and the bridge has no instruction limit (the worker runs it on a thread and
looks for the host's CANCEL).

Three rows beyond stunt's are in the tests, not the table:
`getenv-home-goroutine` and `dlopen-libc-by-name-goroutine` (the same libc
calls from a goroutine) and `threads` (16 goroutines each holding a thread
of their own), in
[containment/test_threads.sh](containment/test_threads.sh).

### What the table says

- **In-process is not containment for Go.** On Linux the preload holds
  what a Go library does through cgo (`C.getenv`, `C.dlopen`, libc's
  `syscall()`), on the thread that holds the level, and **nothing else**:
  `os`, `net`, `os/exec` and `syscall` make their system calls without
  libc, so the file, the socket, the shell and the environment copy all
  escape. This is true on both sides of ae 0.884.0 (`AE_SPAWN_PIN`), which
  made the preload hold `system()` and `posix_spawn`: Go does not call
  them. The tests run these rows expecting success, so the day Go goes
  through libc or the preload grows a seccomp layer, they go red and this
  table is brought up to date. On macOS nothing holds Go's calls at all.
- **The contained child holds every row but one**, by the kernel. The
  worker's own lockdown (an empty level on its main thread, which holds
  libc's calls) adds little for Go: only the rows through cgo
  (`dlopen-libc-*`, the environment) are stricter than the policy. Raw
  system calls follow the policy.
- **The hole: a tcp grant for one address lets the child connect to any
  other** (`stunt_net_other`: `connect-192.0.2.1` and `connect-ipv4-mapped`
  escape). The address check is the preload's, at libc's `connect()`; the
  kernel layer only lets an `AF_INET` socket be made when the policy has
  some tcp grant, and Go's `connect(2)` is never seen by the preload. The
  same row done with a few bytes of machine code from Python or Lua would
  do it. It is recorded as **escapes**, run expecting success. No Landlock
  rule can narrow it by address.
- **Goroutines are on other threads.** Grant levels are per thread, so the
  worker's lockdown and a host's `sandbox.enforce` do not cover a
  goroutine's cgo calls: `dlopen` of libc from a goroutine succeeds under
  `native=*` where the worker's main thread is refused, and a goroutine's
  `getenv` is not held in-process. The process-wide grants of the policy
  still apply (with no native grant the same `dlopen` is refused).

## Calling-convention surface

The original wrappers, named for their shapes:

| Aether call | Matches the c-shared signature |
|---|---|
| `tinygo.call_int_void(h, "F")` | `int F(void)` |
| `tinygo.call_int_int(h, "F", a)` | `int F(int)` |
| `tinygo.call_int_int_int(h, "F", a, b)` | `int F(int, int)` |
| `tinygo.call_void_int(h, "F", a)` | `void F(int)` |
| `tinygo.call_str_str(h, "F", s)` | `const char* F(const char*)` |

Beside them, `call_<ret>_<args>` covers every combination in
[`module.ae`](module.ae)'s export list, up to three arguments, with
one letter per type: `v` void, `i` int32, `l` int64, `d` double,
`s` string, `p` ptr. `call_s_i(h, "F", 7)` calls
`const char* F(int)`. Adding a shape is one line in
[`aether_host_tinygo.c`](aether_host_tinygo.c) plus a matching
`extern` and wrapper in [`module.ae`](module.ae).

Every string-returning wrapper has an `_owned` twin for a function
that returns `C.CString(...)`; see [Memory ownership](#memory-ownership):

| Borrows the result | Takes it over and frees the C pointer |
|---|---|
| `call_str_str` | `call_str_str_owned` |
| `call_s_v` | `call_s_v_owned` |
| `call_s_s` | `call_s_s_owned` |
| `call_s_i` | `call_s_i_owned` |
| `call_s_s_s` | `call_s_s_s_owned` |
| `call_s_s_s_s` | `call_s_s_s_s_owned` |

For any other signature, `call_dynamic` dispatches through libffi
when the bridge is built with `AETHER_HAS_LIBFFI` defined. Without
libffi it returns 0 and `last_error()` says libffi is unavailable,
so the module itself needs nothing beyond `std.dl`.


## Memory ownership

A string-returning wrapper gives back the pointer the Go function
returned, and Aether only borrows it: nothing on this side frees it.
The bridge cannot tell from a signature who owns the result, so the
caller says so, per call site (#2569):

- **`C.CString(...)` result: call the `_owned` twin.** cgo allocates
  a `C.CString` with `malloc` on the C heap, outside the Go collector,
  so through a borrowing wrapper every call leaks one string.
  `call_str_str_owned` and the other `_owned` wrappers copy the result
  into a string Aether owns, `free()` the C pointer, and Aether frees
  the copy like any string it made. A nil result, or a symbol that
  does not resolve, gives `""`.
- **Static or long-lived result: call the plain wrapper.** A pointer
  the library keeps (a constant, a buffer it reuses) must never reach
  `free()`, so it is borrowed, and stays valid for as long as the
  library keeps it.

The `_owned` wrappers free with the C runtime the Aether program uses,
so the library has to allocate with the same one. On Linux and macOS
there is one. On Windows, MSVCRT and UCRT keep separate heaps, so
build the library with a `gcc` of the same runtime as the one `ae`
builds the program with: cgo uses the `gcc` on `PATH`, and `ae` uses
`$AE_CC`, `$CC`, the `gcc` on `PATH`, or its own WinLibs UCRT copy,
in that order.

`call_dynamic` with return kind `'s'` writes the library's pointer to
`result_out` as it is; for a `C.CString`, free it once you are done.

The plain wrappers keep their behaviour. Whether a `C.CString` result
should instead be taken over by default, with a borrowed form for the
static case, is still open in #2569, since it would change what the
existing call sites do.

## Limitations

- **Single Go runtime per process.** `dlopen` of two distinct c-shared Go
  libraries in the same process can collide on runtime state. Stick to one
  library per Aether process. A Go library cannot be unloaded either:
  `tinygo.unload` is a `dlclose`, and Go's runtime does not support being
  unloaded, so treat a loaded library as loaded for good.
- **No goroutines that outlive the Aether call.** The Go scheduler runs
  inside the call; a goroutine that blocks on I/O and returns to the Aether
  caller before it completes is undefined behaviour. (Its thread is also
  not under the caller's grant levels: [way 3](#3-the-same-under-ld_preload).)
- **`tinygo build -buildmode=c-shared` is wasm-only today.** Native
  c-shared libraries come from standard `go build` (the toolchain note at
  the top). The bridge is purely a `dlopen` and a symbol lookup: it loads
  whatever c-shared the toolchain produced.
- **Symbol export is `//export Name`.** Neither standard Go nor TinyGo
  exports a package-level `Name` automatically: annotate each function you
  want callable from Aether.
- **No host functions, no shared map.** Unlike the scripting-language
  bridges here, a Go library cannot call back into Aether: there is no
  `register`, and the shared map's `aether_map_get` is not exported to it.
  Data goes in as arguments and comes back as results. (A library could be
  handed a C function pointer as an argument with `call_v_p`, which the
  bridge does not wrap further.)
- **The Go runtime starts at load.** Its threads and its copy of the
  environment are made when the library is `dlopen`ed, so a host that loads
  the library inside a sandbox block has it created under the block's
  restrictions: load first, enforce after (the tests do).

## In a contained child

`tinygo/worker/worker.ae` is a std.guest worker. It takes the path of the
c-shared library as its argument, since it is given no environment, loads
it (which starts the Go runtime) and then locks itself down. The script is
lines, each a verb and its arguments:

| Verb | Effect |
|---|---|
| `call SYMBOL [ARG1[<TAB>ARG2]]` | calls the exported `char* SYMBOL(char*, char*)` on a **thread of its own**, and says (`SAY`) the string it returns; the worker's main thread looks for the host's `CANCEL` every 5 ms, and ends the run at one with `ERROR cancelled by host: ...` |
| `call_here SYMBOL [ARG1[<TAB>ARG2]]` | the same call on the worker's main thread, which cannot hear a `CANCEL` until it returns: a function that never does is killed by the host after its grace period (`silent`) |
| the end | `DONE 0`, or `ERROR` for a verb it does not know |

The result is a `C.CString`, which the worker frees. A symbol that does not
resolve answers the empty string. A function with no arguments is called
the same way: the two string arguments are ignored by the C calling
convention. There are no host functions to mount.
[containment/child_host.ae](containment/child_host.ae) is a complete host.

What a Go library needs of the process after the lockdown, and whether the
sandbox's spawn allows it, from the tests on CachyOS (ae 0.889.0 and
0.878.0):

- **Threads: yes.** The Go runtime starts several at load (the scheduler's
  `sysmon`, the GC's workers) and more as goroutines block; the contained
  child's seccomp filter refuses `clone` only as a way to make a process
  (`fork`, and `ForkExec`'s `CLONE_VFORK`), not as a thread. `check_threads`
  holds 16 goroutines, each locked to a thread of its own, in the child.
- **The library file: a read grant.** `spawn_socket`'s Landlock floor lets
  the loader read the system's libraries and nothing else, so the child's
  policy is the stunt test's plus `fs_read` of the library
  (`containment/child_host.ae` adds it to every policy; any host of
  `tinygo-worker` has to). No row reads there.
- **Memory: yes.** The runtime maps and unmaps memory freely.
- **Nothing from the environment.** The worker has no environment but
  std.ipc's `AETHER_IPC_FD` and `AETHER_IPC_KIND`, which Go copies at load
  (it is how the `half-frame` row finds the socket).
- **The call thread takes the lockdown's level.** Grant levels are per
  thread; the main thread's is the worker's lockdown (`child.run`), so the
  thread a cancellable call runs on pushes the same empty level
  (`worker_support.c`). A goroutine the Go code starts does not get one.

## Tests

```sh
GUEST_LANG=tinygo sh tests/run.sh               # specs, containment rows, parity check
GUEST_LANG=tinygo GHL_REQUIRE_CHILD=1 sh tests/run.sh   # Linux with Landlock: a missing child mode fails
GO=/usr/local/go/bin/go GUEST_LANG=tinygo sh tests/run.sh
```

`../tests/tinygo_env.sh` finds the toolchain (`$GO`, then `PATH`, then the
usual install places; a `tinygo` on `PATH` is only noted) and builds the
c-shared libraries into `build/`; without Go the specs, rows and parity
check skip, and on GitHub Actions a skip is a failure. A C compiler is
needed too (cgo).

- [test_host_tinygo.ae](test_host_tinygo.ae): the bridge against
  `examples/greet.go` as a real Go library: `Answer = 42`, `Add(2, 40) = 42`,
  `Negate(7) = -7`, `hello, world` through `call_str_str_owned` (a real
  `C.CString`, freed by the bridge), an unresolved symbol, a missing library.
  Moved from aether's `tests/integration/host_tinygo`.
- [test_host_tinygo_owned.ae](test_host_tinygo_owned.ae) needs no Go: it
  loads `test_support/fake_cshared.c`, a C library with the signatures cgo
  generates and `C.CString`'s `malloc`'d results, and checks every `_owned`
  wrapper, the nil and unresolved-symbol results, a static result through the
  borrowed wrappers, and the heap over rounds of 100 owned calls of a 64 KiB
  result (`mem.steady_growth`). Moved from the same place.
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: stunt's
  catalogue, held to Go by name ([Stunt parity](#stunt-parity)).
- [containment/test_threads.sh](containment/test_threads.sh): the
  goroutine and thread rows. [containment/test_examples.sh](containment/test_examples.sh):
  the four examples above, built and run.

## See also

- [`std/dl/module.ae`](https://github.com/aether-lang-dev/aether/blob/main/std/dl/module.ae):
  the cross-platform `dlopen` shim this bridge sits on top of.
- [TinyGo's cgo guide](https://tinygo.org/docs/guides/cgo/).
- The other languages' READMEs: [python](../python/README.md#four-ways-to-run-python-from-aether),
  the template this follows.
aelane: 'sh ./.lin-parity.sh' on cachyos exited 0 after 22s

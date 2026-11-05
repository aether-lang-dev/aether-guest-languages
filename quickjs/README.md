# quickjs: embedded JavaScript (QuickJS, ES2023)

`import quickjs` lets an Aether program run JavaScript: [QuickJS](https://github.com/quickjs-ng/quickjs)
(quickjs-ng 0.17.0), an embeddable ES2023 engine. It is the bridge that was
aether's `contrib.host.quickjs`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `quickjs.run`,
`quickjs.run_sandboxed` and `quickjs.run_sandboxed_with_map`.

It sits beside [`duktape`](../duktape/README.md), the other JavaScript engine
here, under its own name (the engine's, not the language's). Choose by the
language you need: Duktape is ES5.1, QuickJS is ES2023 (classes with private
fields, `async`/`await`, `Map`/`Set`, `?.`, `??`, spread, modules).

QuickJS has **no I/O of its own here**: the engine's file, network and OS
modules (`quickjs-libc`) are not compiled in, so there is no `std`, no `os`,
no `require`, no environment, no processes, no sockets, no way to load a
library or call C. What a script can reach is what the host gives it. The
bridge gives it `print` and `console.log` (stdout) and, in a run with the
shared map, `aether_map_get` and `aether_map_put`; anything else is a host
function you register through aether's engine module
([Host functions](#host-functions)), which checks the grants itself.

## The engine: aether's `contrib.quickjs`, not a copy

The bridge compiles no QuickJS of its own. `module.ae` imports aether's
engine module, `contrib.quickjs`, whose C and pinned amalgamation (quickjs-ng
0.17.0, `amalgamation.lock`) every aether release ships under
`share/aether/contrib/quickjs/` (since well before this repository's
`AE_PIN`; checked from 0.778.0 on). So there is **one copy of QuickJS and one
pin, aether's**: this repository neither vendors the amalgamation nor repeats
its version, and a newer `ae` brings a newer engine to this bridge unchanged.
Nothing is loaded at run time (no `dlopen`, no soname, no environment
variable) and nothing needs installing: no package, no dev headers. The
price is that the bridge's C declares the engine's `qjs_*` functions by hand
(there is no shared header) and must match them; the specs here fail the
build if they ever do not. [PROVENANCE.md](PROVENANCE.md) says how to keep in
step with aether if the engine's C changes shape.

## Four ways to run JavaScript from Aether

From least to most contained. The first three run QuickJS inside your
program, and the fourth runs it in a separate, locked-down process. All four
stay supported: pick by how far you trust the script, not by which is newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where JavaScript runs | your process | your process | your process | its own process (`quickjs/worker`) |
| What a script can reach | `print`, `console.log` | the same | the same | `print`, `console.log` (SAY) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Time limit | 5 s per run | 5 s per run | 5 s per run | the host's deadline, then a kill |
| Platforms | everywhere aether runs | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0 | ae 0.878.0 (`std.guest`) |

### 1. Embed: QuickJS in your process, no containment

```aether
import quickjs

main() {
    script = <<JS
        const t = ['a', "b", 'c'];
        t.forEach((x, i) => print(i + 1, x));
        class Point { #x; constructor(x) { this.#x = x; } get x() { return this.#x; } }
        print(JSON.stringify({ q: 'quote"d', n: new Point(7).x ?? 0 }), [3, 1, 2].toSorted().join());
    JS
    if quickjs.run(script) != 0 { println("js failed") }
}
```

A heredoc (`<<JS ... JS`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape processing,
so JavaScript's quotes, backslashes and template strings need no escaping,
and the indentation the lines share is stripped. A plain string works too:
`quickjs.run("print('hi')")`, but a `${}` in it is Aether's. This program
([examples/way1_embed.ae](examples/way1_embed.ae)) prints

```
1 a
2 b
3 c
{"q":"quote\"d","n":7} 1,2,3
```

`quickjs.run` makes a fresh runtime per call (32 MB heap cap, 5 s limit per
run), so nothing carries from one call to the next, and there is nothing to
free. A thrown error or a syntax error makes it return `-1` and print
`[js] <the error and its stack>` on stderr. The script can only compute and
print, so this way is safe to run on a script you did not write, as far as
the machine goes; what it cannot hold is memory (a script can allocate up to
the cap and then fail) and the wall clock (the limit is 5 s).

For a script that calls back into the host, keeps state between runs, or
returns a typed value, use aether's engine directly, `import contrib.quickjs
as engine` (runtimes, handles, closures, values, promises, bytes; its README
has the model). That is still this first way, in-process; hold its host
functions to grants with `sandbox.enforce`.

### 2. Embed inside a sandbox block

```aether
import quickjs
import contrib.quickjs as engine
import std.sandbox
import std.os

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }

    // The bridge's own run: a script that computes, and says so.
    if quickjs.run_sandboxed(reader, "print('computed', 6 * 7)") != 0 { println("js failed") }

    // A host function is how a script reaches anything. This one is held to
    // the grants in force, here by the std call it makes.
    q = engine.runtime(8192, 0)
    getenv = engine.function(q, "env", 1, |q2: ptr, this_h: int, args: int| {
        v = os.getenv(engine.arg_string(q2, args, 0))
        if v == "" { return engine.new_undefined(q2) }
        return engine.new_string(q2, v)
    })
    engine.set_global(q, "env", getenv)
    engine.release(q, getenv)
    sandbox.enforce(reader) callback {
        v, e = engine.eval(q, "'HOME=' + env('HOME') + ' USER=' + env('USER')", "demo.js")
        if e != "" { println("js failed: ${e}") } else { println(engine.to_string(q, v)) engine.release(q, v) }
    }
    engine.dispose(q)
}
```

([examples/way2_sandbox.ae](examples/way2_sandbox.ae).) `run_sandboxed` pushes
the block's grants as a level for the length of the call and pops it again,
also when the script throws. The script it runs has no way to ask for
anything, so on its own it changes nothing a script can do: the grants matter
for the host functions a script calls, which are checked on every platform.
The program prints

```
computed 42
HOME=/Users/paul USER=undefined
```

`USER` is refused by the grants and the host function answers `undefined`: a
refusal and an unset variable look the same to the script unless the host
throws, as the containment tests' host functions do.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits between
the process and libc and holds its libc calls to the same grants (Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

The same program, run on CachyOS under ae 0.889.0 with and without the
preload:

| The script gets | 2. block, no preload | 3. block + `LD_PRELOAD` |
|---|---|---|
| `print('computed', 6 * 7)` | `computed 42` | `computed 42` |
| `env('HOME')` | `/home/paul` (granted) | `/home/paul` (granted) |
| `env('USER')` | `undefined` | `undefined` |

The preload changes nothing, and that is the finding, not a failure: QuickJS
without `quickjs-libc` reaches libc for nothing a script can ask for, and the
host function had already asked the grants, so the preload has nothing left to
refuse. It is a second line for the libc calls under a host function and for
any C you link into the same process; it is not what holds QuickJS. It is a
library interposer, not a kernel boundary, and macOS has no equivalent.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("js") {}
    args = list.new()
    g, err = guest.launch(perms, "build/quickjs-worker", args, "js", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    guest.start(&g, "print(greet('js'))")
    while true {
        e = guest.next(&g)
        if e.kind == "SAY" {
            println(guest.printable(e.text))
        } else if e.kind == "CALL" {
            guest.answer(&g, guest.json_string("hello"))
        } else {
            break
        }
    }
}
```

([examples/way4_child.ae](examples/way4_child.ae); it prints `hello`.)
JavaScript runs in `quickjs/worker`, a separate process `guest.launch` starts
under Landlock and seccomp from the policy, with the preload, and talking to
the host only over a socket (`std.ipc` frames). The worker's script has
`print`/`console.log` (a `SAY`) and the functions the host mounted, each a
global whose call is a `CALL` the host answers; it reaches a file only when
the host hands it a descriptor, and an environment variable only through a
host function. The engine is part of the worker's program and QuickJS has no
standard library on disk and no module loader, so there is **nothing to load
before the lockdown**: the worker needs to read nothing after it starts, takes
no arguments and needs no environment (Lua's and Python's workers have to
load a library first; Duktape's loads libduktape). The host can cancel a
script at a deadline and kills a worker that stops answering. On macOS, where
nothing can contain the child, `guest.launch` refuses and starts nothing.
Build the worker once with
`ae build quickjs/worker/worker.ae -o build/quickjs-worker`; details under
[In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as QuickJS comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: exec-bin-sh left out, open-etc-passwd left out | differs: exec-bin-sh left out, open-etc-passwd left out | differs: exec-bin-sh left out, fork left out, open-etc-passwd left out |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-granted-file left out, open-etc-passwd left out, write-granted-file left out, getenv-home left out, env-cached left out | differs: open-granted-file left out, open-etc-passwd left out, write-granted-file left out, getenv-home left out, env-cached left out | differs: open-granted-file left out, open-etc-passwd left out, syscall-fn-openat left out, syscall-openat left out, write-granted-file left out, getenv-home left out, env-cached left out |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh left out, open-etc-passwd left out | differs: exec-bin-sh left out, open-etc-passwd left out | differs: fork left out, exec-bin-sh left out, open-etc-passwd left out |
| `test_stunt_host_checked` | `-` | matches | matches | differs: half-frame left out |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd left out | differs: open-etc-passwd left out | differs: dlopen-libc-by-name left out, dlopen-libc-by-path left out, open-etc-passwd left out |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd left out, open-granted-file left out | differs: open-etc-passwd left out, open-granted-file left out | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, open-etc-passwd left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, open-etc-passwd left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, syscall-socket left out, open-etc-passwd left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd left out, open-granted-file left out, write-granted-file left out, getenv-home left out, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, exec-bin-sh left out, dlopen-libm left out | differs: open-etc-passwd left out, open-granted-file left out, write-granted-file left out, getenv-home left out, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, exec-bin-sh left out, dlopen-libm left out | differs: open-etc-passwd left out, open-granted-file left out, write-granted-file left out, getenv-home left out, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, exec-bin-sh left out, fork left out, dlopen-libc-by-name left out, dlopen-libc-by-path left out, dlopen-libm left out, syscall-fn-openat left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: write-granted-file left out, open-granted-file left out | differs: write-granted-file left out, open-granted-file left out | differs: write-granted-file left out, open-granted-file left out |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, QuickJS quickjs-ng 0.17.0
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, QuickJS quickjs-ng 0.17.0
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, QuickJS quickjs-ng 0.17.0
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, QuickJS quickjs-ng 0.17.0
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, QuickJS quickjs-ng 0.17.0
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt` at
`AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in `../tests/test_stunt_*.sh`
with the same policies; the containment table below has each row. The table is
generated from a run (`sh tests/run.sh --write-parity`) and checked against
every run (`tests/parity.sh`), in the columns the host can run; the lines under
it say which host produced which column.

Almost everything that differs is **left out**: QuickJS as embedded cannot
express the row (no files, environment, processes or sockets, no `syscall()`,
no way to load a library), so there is nothing to hold and nothing to test.
That is the honest result for an engine with no I/O surface of its own, and
for a bridge that adds none, not a gap in the table. The rows that remain are
the host functions, the shared map and the host-checked rows.

## Containment table

What a JavaScript script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the QuickJS way
([containment/checks.js](containment/checks.js)), under the policies of aether's
`tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`). "Per
policy" means granted where the policy grants it and refused where it does not,
as stunt expects. The columns are the ways above: "in-process, Linux (preload)"
is way 3, "in-process, macOS" is way 2 where there is no preload, and
"contained child" is way 4. Every cell is a test outcome (`../tests/run.sh`):
checked on macOS arm64 and on CachyOS x86_64, kernel 7.3 with Landlock
(quickjs-ng 0.17.0), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: a `ReferenceError` the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `spin` (`for (;;) {}`) | stopped: the bridge's 5 s time limit (`InternalError: interrupted`) | same | killed: no answer to CANCEL within the grace period |
| `silent` (`for (;;) {}`) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `print`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `open-etc-passwd`, `open-granted-file`, `write-granted-file` | left out: no file API; a file comes through a host function | | |
| `getenv-home`, `env-cached` | left out: no environment API; a variable comes through a host function | | |
| `exec-bin-sh`, `fork` | left out: no process API (`os.exec` is quickjs-libc's) | | |
| `connect-192.0.2.1`, `connect-ipv4-mapped` | left out: no sockets | | |
| `syscall-fn-openat`, `syscall-openat`, `syscall-socket` | left out: no `syscall()`, no way to call C | | |
| `dlopen-libm`, `dlopen-libc-by-name`, `dlopen-libc-by-path` | left out: no way to load a library | | |
| `half-frame` | left out: a script cannot write part of a frame | | |

What the table says:

- **Containment is by construction, not by the preload.** Nothing a script
  can write reaches libc except through a host function you registered, so
  there is no row that only the preload can hold and macOS therefore does
  not. (The table has no "not held" and no "escapes".)
- **The rows that exist are the host's.** Host functions check the grants
  themselves (a function that did not would be a hole, whatever the engine
  does), and the shared map's inputs are frozen by the runtime. Outside a
  sandbox block, `quickjs.run` checks nothing, but a script run by it has no
  host functions either.
- **The contained child holds every row it runs**, on Linux. Unlike Duktape's,
  its script is given no less than the in-process one is (there are no
  `env`/`readFile` bindings to take away), so no row is "stricter".
- **A loop is ended by a time limit in-process and by a kill in the child.**
  The bridge's 5 s limit per run ends `for (;;) {}` in a program (the engine's
  interrupt handler); aether's `contrib.quickjs` offers that time limit but no
  callback, so the worker has no hook to look for the host's CANCEL from inside
  a loop. The host's deadline and grace period still end it, by killing the
  worker: `spin` and `silent` come out the same in the child. A script that
  calls the host, though, sees a CANCEL in the reply.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row would run expecting the call to succeed, so the day something
  stops being held this goes red. There are none to hold today.

## Host functions

The bridge's `run_sandboxed_with_map(perms, code, map_token)` adds
`aether_map_get(key)` and `aether_map_put(key, value)` to the script,
bound to a shared map and freezing the host's inputs before it runs (a
`put` to a frozen key is ignored). For anything else, register a function
with aether's engine module:

```aether,fragment
import contrib.quickjs as engine

q = engine.runtime(8192, 0)
read = engine.function(q, "read_file", 1, |q2: ptr, this_h: int, args: int| {
    path = engine.arg_string(q2, args, 0)
    text, err = fs.read(path)                       // checked against the grants in force
    if err != "" { return engine.throw_error(q2, "Error", err) }
    return engine.new_string(q2, text)
})
engine.set_global(q, "read_file", read)
engine.release(q, read)
sandbox.enforce(grants) callback { v, e = engine.eval(q, "read_file('/etc/hosts')", "x.js") }
```

This is aether's engine API and is documented there
(`contrib/quickjs/README.md` in aether). `containment/host.ae` has the host
functions the tests use, `read_file`, `getenv`, `greet` and `add`, checking
the grants themselves with `aether_sandbox_check_slow`.

## In a contained child

`quickjs/worker/worker.ae` is a std.guest worker. In the script, `print` and
`console.log` are `SAY` (arguments space-joined), and each function the host
mounts is a global of the same name whose call is a `CALL` the host answers:
the arguments go as a JSON array and the answer is `JSON.parse`d back (a
granted file comes back as `{granted: true, text: ...}`). A `FAIL` throws an
`Error` with the host's message (`refused: ...` when the grants said no); a
`CANCEL` throws `cancelled by host: ...`, which the script can catch: the
host's kill after its grace period is what holds it. The worker's runtime has
a 64 MiB heap cap. [containment/child_host.ae](containment/child_host.ae) is a
complete host.

## Implementation notes

- The bridge's `aether_host_quickjs.c` declares the engine's `qjs_*` C
  functions by hand (`qjs_new`, `qjs_eval`, `qjs_function`, ...), matching
  aether's `contrib/quickjs/aether_quickjs.c`; the closure passed to
  `qjs_function` has the compiler's `{fn, env}` layout. It is compiled into
  the importing program (`@source` in `module.ae`), together with the
  engine's C (`import contrib.quickjs`).
- Each `run` makes a runtime (32 MB heap cap, 5000 ms per entry), runs the
  script and frees it. `run_sandboxed*` push and pop the grants level around
  the run, also when the script throws.
- `print`/`console.log` are the one addition to aether's copy of the bridge
  (PROVENANCE.md): aether's scripts could only compute and write to the
  shared map.
- Known, kept from aether's copy: `run_sandboxed_with_map` keeps the current
  map token in a process-wide variable, so two such runs on different threads
  at once would share it. The bridge is not safe for concurrent runs.
- A thrown error is printed to stderr as `[js] ...` and the call returns `-1`;
  there is no way to get the message back or to read a value out of a script.
  That is what `contrib.quickjs` is for.

## Tests

- [`test_host_quickjs.ae`](test_host_quickjs.ae): the bridge, through
  `import quickjs`: a script evaluates, a thrown error is a failure, host
  functions are held to the grants in force, the shared map round trips, the
  grant stack is left as found, `init`/`finalize` are idempotent. Moved from
  aether but for the import and the file the host function reads
  (`/etc/hosts`: `/etc/hostname` is not on macOS).
- [`test_quickjs_bridge.ae`](test_quickjs_bridge.ae): `print` and
  `console.log`, the absence of `quickjs-libc`'s names, and ES2023.
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: the containment
  table above.

The specs skip when `contrib.quickjs` cannot be used (on GitHub Actions a skip
is a failure). They assert on return codes, never on the interpreter's
stdout, which is not reliably flushed when an Aether program exits.
`../tests/run.sh` runs everything: `GUEST_LANG=quickjs sh tests/run.sh`.

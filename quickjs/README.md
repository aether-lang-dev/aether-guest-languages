# quickjs: embedded JavaScript (QuickJS, ES2023)

`import quickjs` lets an Aether program run JavaScript: [QuickJS](https://github.com/quickjs-ng/quickjs)
(quickjs-ng 0.18), an embeddable ES2023 engine. It is the bridge that was
aether's `contrib.host.quickjs`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `quickjs.run`,
`quickjs.run_sandboxed` and `quickjs.run_sandboxed_with_map`, and the
two-way VM the Lua, Python, Duktape and Tcl bridges have, with the same
names and meanings: make and free an interpreter, register host functions by
name, set globals, evaluate with a typed result ([The two-way
VM](#the-two-way-vm)).

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
function you register on a VM (`quickjs.vm_register`), which checks the
grants itself.

## The engine: aether's `contrib.quickjs`, not a copy

The bridge compiles no QuickJS of its own. `module.ae` imports aether's
engine module, `contrib.quickjs`, whose C and pinned amalgamation (quickjs-ng
0.18, `amalgamation.lock`) every aether release ships under
`share/aether/contrib/quickjs/`. So there is **one copy of QuickJS and one
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
| Time limit | 5 s per run; a VM's own time limit and cancel cell | the same | the same | the host's deadline: its `CANCEL` stops a loop; a kill after the grace period |
| Platforms | everywhere aether runs | same | Linux | Linux with Landlock; macOS refuses to start it |

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
returns a typed value, make a VM ([The two-way VM](#the-two-way-vm)). That is
still this first way, in-process; hold its host functions to grants with
`sandbox.enforce` (way 2).

### 2. Embed inside a sandbox block

```aether
import quickjs
import std.sandbox
import std.os

// env(name): the variable, or undefined. os.getenv is held to the grants in
// force, so a variable the block does not grant reads as unset.
@c_callback
host_env(vm: ptr) -> int {
    v = os.getenv(quickjs.arg_str(vm, 1))
    if v == "" { return 0 }
    quickjs.push_str(vm, v)
    return 1
}

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }

    // The bridge's own run: a script that computes, and says so.
    if quickjs.run_sandboxed(reader, "print('computed', 6 * 7)") != 0 { println("js failed") }

    // A host function is how a script reaches anything.
    vm = quickjs.vm_new()
    quickjs.vm_register(vm, "env", host_env)
    sandbox.enforce(reader) callback {
        if quickjs.vm_eval(vm, "'HOME=' + env('HOME') + ' USER=' + env('USER')") == 0 {
            println(quickjs.vm_result_str(vm))
        } else {
            println("js failed: ${quickjs.vm_result_str(vm)}")
        }
    }
    quickjs.vm_free(vm)
}
```

([examples/way2_sandbox.ae](examples/way2_sandbox.ae).) `run_sandboxed` pushes
the block's grants as a level for the length of the call and pops it again,
also when the script throws. The script it runs has no way to ask for
anything, so on its own it changes nothing a script can do: the grants matter
for the host functions a script calls, which are checked on every platform
(here inside `sandbox.enforce`, around the VM's eval).
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

The same program, run on CachyOS with and without the
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
script at a deadline, and the worker hears it inside a loop as well as in a
call's reply ([In a contained child](#in-a-contained-child)); the host kills
a worker that stops answering. On macOS, where
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

- Column 2: macOS 27.3 arm64, ae 0.934.0, QuickJS quickjs-ng 0.18.0
- Column 3: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, QuickJS quickjs-ng 0.18.0
- Column 4: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, QuickJS quickjs-ng 0.18.0
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

## Boundary tests

The shared tests in `tests/boundary.py`, run in this language's worker (`tests/test_boundary.sh`): one script per case, written in quickjs (`containment/boundary.tsv`), and what the host saw on the wire. `ok` means the value crossed as the same JSON value; `n/a` means the language has no way to write the case; anything else is what happened instead. Nothing in the tests lists what a worker can do.

<!-- boundary:begin: generated by sh tests/run.sh --write-parity, from build/boundary-*.tsv -->
| case | guest to host | host to guest, passed back |
|---|---|---|
| `str` | ok | ok |
| `str-empty` | ok | ok |
| `str-nonascii` | ok | ok |
| `str-nul` | ok | ok |
| `str-astral` | ok | ok |
| `str-60k` | ok | ok |
| `int` | ok | ok |
| `int-neg` | ok | ok |
| `int-max` | differs: the host saw [9223372036854776000] | differs: the host saw [9223372036854776000] |
| `int-min` | differs: the host saw [-9223372036854776000] | differs: the host saw [-9223372036854776000] |
| `float` | ok | ok |
| `bool-true` | ok | ok |
| `bool-false` | ok | ok |
| `null` | ok | ok |
| `array` | ok | ok |
| `map` | ok | ok |
| `nested` | ok | ok |
| a call inside a call's argument (`nest`) | ok | |
| an error answer (`FAIL`) to a call (`refuse`) | caught | |
| a cancel while a call waits (`wait`) | heard at the call | |
| a cancel inside a guest loop (`loop`) | heard in a loop | |
| a 100,000-byte argument (`big-up`) | refused, worker killed | |
| a 100,000-byte answer (`big-down`) | refused: killed | |
<!-- boundary:end -->

## Boundary tests, in-process

The same cases (`tests/boundary.py run-inproc`) against this language's bridge in this process, through the API it really has (`containment/boundary_host.ae`, an Aether program written against it). There is no wire: "what the host saw" is what its typed readers made of the argument, so `differs` says what was lost. `ok` means the value came back as the same JSON value; `n/a` means the bridge has no call that could express the case; `refused` is a deliberate, documented refusal. Nothing in the tests lists what a bridge can do. For a table the host can build one but not read one, so a table read back in the host is a tag. The columns are Linux's; the macOS columns read `same` where macOS gave the same result and say what it gave where it did not.

<!-- boundary-inproc:begin: generated by sh tests/run.sh --write-parity, from build/boundary-inproc-*.tsv -->
| case | guest to host | host to guest, passed back | macOS: guest to host | macOS: host to guest |
|---|---|---|---|---|
| `str` | ok | ok | same | same |
| `str-empty` | ok | ok | same | same |
| `str-nonascii` | ok | ok | same | same |
| `str-nul` | ok | ok | same | same |
| `str-astral` | ok | ok | same | same |
| `str-60k` | ok | ok | same | same |
| `int` | ok | ok | same | same |
| `int-neg` | ok | ok | same | same |
| `int-max` | differs: the host saw [9.22337e+18] | differs: the host saw [9.22337e+18] | same | same |
| `int-min` | ok | ok | same | same |
| `float` | ok | ok | same | same |
| `bool-true` | ok | ok | same | same |
| `bool-false` | ok | ok | same | same |
| `null` | ok | ok | same | same |
| `array` | ok | ok | same | same |
| `map` | ok | ok | same | same |
| `nested` | ok | ok | same | same |
| a call inside a call's argument (`nest`) | ok | | same | |
| an error raised by the host function (`refuse`) | caught | | same | |
| a cancel while a call waits (`wait`) | n/a | | same | |
| a limit on a guest loop (`loop`) | stopped by the limit | | same | |
| a 100,000-byte argument (`big-up`) | crossed | | same | |
| a 100,000-byte answer (`big-down`) | crossed | | same | |
<!-- boundary-inproc:end -->

## Exporting functions to the host

A script that exports functions ends with `READY` and serves its exports to the
host (`guest.invoke`, then `guest.next` until `RETURN` or `RAISE`; `guest.end`
when the host is done), with the script's state as the script left it. The host
can reach into a guest three ways, as equals; pick by what the application needs:

- **A direct call with arguments.** The invocation carries its input: any JSON
  value the language can read, up to the frame limit (the grid below says what
  crosses).
- **Notify, then fetch.** A small notification, an id only, and the export phones
  home through a mounted host function for the details: the smallest message
  that says "something is ready for you", a practice message buses settled on.
- **Pull, with no verb.** The script loops on a mounted `next_event()` and
  dispatches itself; it needs nothing of the verb and works with any worker.

In JavaScript (QuickJS): `export_fn("name", fn)` (or `export_fn(fn)` for a named function): `export` is a reserved word in JavaScript, so the prelude's name says what it does. `export_fn` is the worker's name, so a host cannot mount a function called `export_fn`.

```javascript
export_fn("total", (items, rate) =>     // a direct call: total([...], 0.2)
  items.reduce((s, i) => s + i.price, 0) * (1 + rate));

let seen = 0;                            // notify, then fetch: on_txn("123456")
export_fn("on_txn", (id) => {
  const d = txn_details(id);             // a mounted host function, mid-invocation
  seen++;
  return `${seen} ${d.kind} ${d.amount}`;
});

for (let ev; (ev = next_event()) !== null; ) handle(ev.name, ev.args);   // pull
```

The names are fixed when the script ends; an exception in an export is `RAISE` with the language's own error text and the worker serves on; a `CANCEL` or deadline during an invocation ends the worker, as in a script. The grid below is generated from the tests.

## Host calls guest

The other direction (`tests/boundary.py run-invoke`, run by `tests/test_invoke.sh`): a script per case that exports functions, written in quickjs (the `x:` snippets in `containment/boundary.tsv`), run in this language's worker with a plan of invocations from the host in `tests/lib/invoke`. A type crosses as a `RETURN` value when the export returns it written in the language, and as an `INVOKE` argument when the export hands back what it was given. The second table holds the host-calls-guest safety rows by name: `held`, `stricter` (refused earlier than the row asks), `left out` (the language cannot express it), or `not held`, which fails the run. Nothing in the tests lists what a worker can do.

<!-- invoke:begin: generated by sh tests/run.sh --write-parity, from build/invoke-*.tsv -->
| case | guest to host, as a `RETURN` value | host to guest, as an `INVOKE` argument, returned |
|---|---|---|
| `str` | ok | ok |
| `str-empty` | ok | ok |
| `str-nonascii` | ok | ok |
| `str-nul` | ok | ok |
| `str-astral` | ok | ok |
| `str-60k` | ok | ok |
| `int` | ok | ok |
| `int-neg` | ok | ok |
| `int-max` | differs: the host saw 9223372036854776000 | differs: the host saw 9223372036854776000 |
| `int-min` | differs: the host saw -9223372036854776000 | differs: the host saw -9223372036854776000 |
| `float` | ok | ok |
| `bool-true` | ok | ok |
| `bool-false` | ok | ok |
| `null` | ok | ok |
| `array` | ok | ok |
| `map` | ok | ok |
| `nested` | ok | ok |
| an export that calls a mounted host function (`relay`) | ok | |
| state between invocations (a counter, three calls) (`state`) | state carries | |
| an export that raises, then another call (`raise`) | RAISE: Error: boom | |
| a cancel inside an export's loop (`loop`) | heard in a loop | |
| a cancel while an export waits on a host call (`wait`) | heard at the call | |
| a 100,000-byte `RETURN` (`big`) | refused, worker killed | |
| three arguments of different types (`args`) | ok | |
| a 100,000-byte `INVOKE` argument (`big-arg`) | refused: killed | |
| `INVOKE` of a name the guest did not export (`unexported`) | refused by the host, nothing sent; the guest serves on | |
| `END` (`end`) | DONE 0 | |
| a script that exports nothing (`none`) | DONE, as before exports | |
| notify, then fetch: two transactions, an unknown id (`txn`) | ok; an unknown id raises: Error: no transaction 999999 | |

| host-calls-guest row | outcome |
|---|---|
| `guest-export-called` | held |
| `guest-export-slow` | held |
| `guest-export-silent` | held |
| `guest-export-oversize` | held |
| `guest-export-raises` | held |
| `guest-reenters-while-called` | held |
| `guest-export-flood` | held |
| `guest-export-malformed` | held |
| `guest-after-end` | held |
<!-- invoke:end -->

## What crosses the boundary

The top-level README's first table says it in a line; this is the detail, each
claim with the spec that backs it.

- **In-process transport.** the engine (quickjs-ng, Aether's `contrib.quickjs`) is compiled into your program: a direct call; no transport.
- **Types.** in: str, int, strlist as globals, and any JSON value (`vm_set_global_json`); callbacks read str, int, bool, float, bytes (a `Uint8Array`), nil and any value as JSON. out: str, int, float, bool, bytes (length-aware, never a string), nil, an array or object as a tag (`QJS_TTABLE`) and as JSON (`vm_result_json`), a BigInt, function or symbol as `QJS_TOTHER`. JavaScript has one number type: a number with no fraction that fits 64 bits reads as an int (so `2.0` is an int), any other as a float; an int past 2**53 is not exact, as in any JavaScript. The tags are Python's numbering.
- **Duplex.** host calls the guest, the guest calls back mid-eval, a callback may `eval` again (nested, tested), and the promise jobs an eval queues run before it returns.
- **Callbacks.** registered by name (dotted ok, an empty part refused); they push int, str, bool, nil, double, bytes or JSON, the last push being the value, and return 1 to return it or 0 for undefined.
- **Errors and state.** `vm_eval` returns -1 with `Type: message` (String(e), without the stack) as the result; `raise_error` is an `Error` the script can catch, thrown after the callback returns; an unregistered name is a `ReferenceError`; a time limit per outermost eval (nested evals and promise jobs included, uncatchable, removable) and a cancel cell any thread may set; the instruction limit is reported unavailable (QuickJS counts no operations); a heap cap (`vm_new_capped`); one runtime per VM, many VMs per process sharing nothing, each used by one thread at a time; `vm_free` is `@frees`, so a use of a freed VM is a compile error, and a VM freeing itself from its own callback is refused.
- **Contained child.** Contained child: `std.guest` over a `std.ipc` socketpair, 4-byte length-prefixed frames, JSON arguments and values, a granted file as a descriptor, guest-initiated calls answered one at a time, no nesting (the top-level README lists the common part). the worker is the bridge's VM; mounted functions take and return any JSON value (`JSON.stringify` up, `JSON.parse` down); a `FAIL` throws an `Error` with the host's message; `print` is `SAY`; `CANCEL` heard inside a loop (a watcher thread sets the VM's cancel cell, which the script cannot catch) and in a call's reply.
- **Tests.** `quickjs/test_host_quickjs_vm.ae` (the Lua, Python, Tcl and Duktape VM specs case for case, then QuickJS's own and the refusals): "each argument's tag and reading, as a callback sees it", "each pushed type, and the callback's return count", "results: numbers, sizes past 64 bits, tables", "bytes: length-aware both ways, never a string", "raise_error, caught and uncaught; a nested eval", "the time limit: stops a loop, cannot be caught, per eval, nested, removed", "two VMs share nothing: globals, host functions, definitions", "a cancel cell set from another thread stops a loop, uncatchably", "a VM freed from its own callback is refused, and frees afterwards"; `quickjs/containment/test_vm_free.sh` (a use after `vm_free` does not compile); the child: the shared boundary tests (`../tests/test_boundary.sh`) and `quickjs/containment/test_worker.sh` ("cancel-catch", "loglevel", "fail-uncaught", "script-error", "unmounted", "host-types").

## Array.find and the deadline

quickjs-ng 0.18.0 frees an array element twice when a deadline or interrupt
stops `Array.prototype.find` (or `findIndex`, `findLast`, `findLastIndex`) after
its first step; the array then holds a freed element and the next collection
dies. This bridge sets a deadline (5 s, per script) and compiles aether's
`contrib.quickjs`, so it has the fault wherever the engine does. Aether
ships the engine patched (upstream's e22ce4de43, as
`contrib/quickjs/patches/0001-array-find-clear-value-after-release.patch`,
applied to the amalgamation by aether's fetch script), so nothing here copies or
applies a patch: the engine is the patched one.
`test_quickjs_find_interrupt.ae` has the write-up's repro (200,001 and 200,000
objects, a 20 ms deadline, every element checked afterwards) and the bridge's
own deadline; on the unpatched engine it aborts in `gc_decref_child`. The
contained child's worker sets no deadline, but its cancel cell stops a `find`
when the host cancels, which on the patched engine is harmless
(`containment/test_find.sh` runs the loops to their end in the bridge and in
the worker and checks every element).

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
(quickjs-ng 0.18), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: a `ReferenceError` the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `spin` (`for (;;) {}`) | stopped: the VM's time limit (`InternalError: interrupted: time limit exceeded`) | same | answered: the worker hears the deadline's `CANCEL` inside the loop and ends `ERROR cancelled by host` |
| `silent` (one long native call: `Array(64).fill(10n ** 300000n).join()`) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period (the engine's interrupt cannot reach inside the call) |
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
- **A loop is ended by a time limit in-process and by the host's CANCEL in
  the child.** A VM's time limit (the run's 5 s, or `vm_set_time_limit`) ends
  `for (;;) {}` in a program, through the engine's interrupt handler. In the
  child the worker's watcher thread sets the VM's cancel cell when the
  deadline's `CANCEL` comes, and the same handler stops the loop where it is:
  `spin` answers, as Lua's, Python's, Perl's, Ruby's and Tcl's do, and unlike
  Duktape's, which has no such handler. What a script cannot do is catch it, or
  switch it off (Lua's and Python's scripts can remove their hook, which is
  their `silent`): QuickJS's `silent` is one native call long enough to
  outlast the grace period, which the handler cannot reach inside (Tcl's is a
  bignum `expr`, the same kind), and the host's kill ends it.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row would run expecting the call to succeed, so the day something
  stops being held this goes red. There are none to hold today.

## The two-way VM

The VM is the shape the Lua, Python, Duktape and Tcl bridges have (aether's
#904): the same function names after the prefix, the same meanings, and the
tags in Python's numbering, so a host written against one reads the same
against another. It is built on aether's `contrib.quickjs` and copies none of
it: every value is a handle in the engine's table, and the VM releases each
one it takes.

```aether
import quickjs
import std.list

// app.add(a, b): two ints, or an Error the script can catch.
@c_callback
add(vm: ptr) -> int {
    if quickjs.arg_type(vm, 1) != quickjs.QJS_TINT || quickjs.arg_type(vm, 2) != quickjs.QJS_TINT {
        return quickjs.raise_error(vm, "add takes two ints")
    }
    quickjs.push_int(vm, quickjs.arg_int(vm, 1) + quickjs.arg_int(vm, 2))
    return 1
}

main() {
    vm = quickjs.vm_new()
    quickjs.vm_register(vm, "app.add", add)
    keys = list.new()
    list.add(keys, "k1")
    quickjs.vm_set_global_strlist(vm, "KEYS", keys)
    list.free(keys)
    quickjs.vm_set_global_int(vm, "BASE", 100)
    quickjs.vm_set_time_limit(vm, 1000, 1)
    if quickjs.vm_eval(vm, "app.add(2, 3) + BASE + KEYS.length") == 0 {
        println(quickjs.vm_result_int(vm))              // 106
    } else {
        println(quickjs.vm_result_str(vm))              // "Error: add takes two ints", say
    }
    quickjs.vm_pop_result(vm)
    quickjs.vm_free(vm)
}
```

([examples/vm.ae](examples/vm.ae).) Who owns what: the caller owns the VM,
from `vm_new` to `vm_free`, and nothing else. `vm_free`'s parameter is
`@frees`, so a use or a second free of the local afterwards does not compile
(E0903; `containment/test_vm_free.sh`); there is no table of live VMs to look
a handle up in. Every engine handle is the VM's: the held result (until the
next eval or `vm_pop_result`), a callback's arguments (the engine's, for the
call), a callback's pushed value (the engine's when the callback returns 1,
released when it returns 0 or raises), the host functions' closures (freed
with the runtime). The rest, call by call:

| Call | What it does | Ownership |
|---|---|---|
| `vm_new()`, `vm_new_capped(kb)` | a VM on a runtime of its own, with `print` and `console.log`; the second caps the heap | the caller's VM; null if it cannot be made |
| `vm_free(vm)` | frees the runtime, every value and host function | `@frees`: the caller's hold ends; refused from inside one of the VM's own evals (the caller of that eval frees it) |
| `vm_register(vm, name, fn)` | a `@c_callback (vm: ptr) -> int` as the function `name`; dotted names make objects; 0 or -1 | `fn` is a code address, not owned; the closure is the VM's |
| `vm_set_global_str`, `_int`, `_strlist`, `_json` | `globalThis[name] = ...`; each returns "" when set, else why not (a name with a NUL, JSON text with a raw NUL, a JSON parse error) | the value is copied in; `_strlist`'s list stays the caller's; a string keeps its NUL |
| `vm_eval(vm, code)` | runs code and the promise jobs it queues; 0 or -1 (and -1, refused, for code with a NUL in it) | the result is the VM's, released by the next eval or `vm_pop_result` |
| `vm_result_type`, `_int`, `_double`, `_bool` | the result, read | nothing handed out |
| `vm_result_str`, `vm_result_json` | the result as a string (a string, or an error's message), or as JSON | a new Aether string, the caller's, with the text's length (a NUL is kept) |
| `vm_result_bytes` | a `Uint8Array` result | a new `std.bytes` handle, the caller's to free |
| `vm_pop_result(vm)` | releases the result | |
| `vm_set_time_limit(vm, ms, every)` | per outermost eval; "" when set | |
| `vm_set_instruction_limit(...)` | reported unavailable | the message is the bridge's |
| `vm_set_cancel_cell(vm, cell)` | a `std.sync` cell that stops the script when nonzero, set from any thread | the cell stays the caller's: free it after `vm_free` or after setting null |
| `vm_set_user(vm, p)`, `vm_user(vm)` | a pointer of the host's for its callbacks | the pointer stays the caller's |
| `vm_busy(vm)` | 1 while one of its evals runs | |
| `arg_count`, `arg_type`, `arg_int`, `arg_double`, `arg_bool` | a callback's arguments (1-based) | nothing handed out |
| `arg_str`, `arg_json` | a string argument, or any as JSON | a new Aether string, the caller's, with the text's length (a NUL is kept) |
| `arg_bytes` | a `Uint8Array` argument | a new `std.bytes` handle, the caller's to free |
| `push_int`, `_str`, `_bool`, `_nil`, `_double`, `_json` | the callback's value (the last push) | copied in, a string with its length; `push_json` returns 0, or -1 for bad JSON (and for a raw NUL, which also raises an Error in the script) |
| `push_bytes(vm, b)` | a `Uint8Array` of a copy of `b` | `b` stays the caller's |
| `raise_error(vm, msg)` | `return` it: the script sees `Error(msg)` after the callback returns, a NUL in `msg` kept | a push before it is dropped |

### Strings, NUL and surrogates

A JavaScript string holds a NUL like any other character, so every string
that crosses here carries its length, in both directions: `arg_str`,
`push_str`, `vm_result_str`, `vm_set_global_str`, `_strlist`, `raise_error`'s
message, an error's message in a result, and the JSON readers and writers
(which spell a NUL `\u0000`, as JSON does). The grid under "Boundary tests,
in-process" reads `ok` for `str-nul`, and the contained child's reads `ok`.
Before 2026-11-21 these were C strings and cut a string at its NUL, silently.

How, because `contrib.quickjs` is only half length-aware (Aether 0.934.0):
it reads a string out with its length (`to_string`) but takes strings in as C
strings (`new_string`, `eval`, `throw_error`, `parse_json`) and keeps an
error as C text. So the bridge reads out by length and returns an Aether
string made from the bytes; makes a string with a NUL from the pieces between
its NULs, joined by a small JavaScript function the VM keeps; reads an error
by catching it again in JavaScript (a function the VM keeps calls the engine's
`rethrow` inside a `try` and gives back `String(e)`), which also means a
message that itself holds `\n    at ` is no longer cut; and throws a
`raise_error` message with a NUL through a second function. None of these is
visible to a script (they are handles in the VM, not globals), and the VM
releases them. The ask for the engine to take lengths itself is
[asks/contrib-quickjs-length-aware-strings.md](../asks/contrib-quickjs-length-aware-strings.md).

What cannot cross is **refused, loudly, naming the reason** (`refused: it
holds an embedded NUL; ...`, the words Tcl's refusal uses, which the grid
reads as `refused`), never cut:

- script text given to `vm_eval`, `run`, `run_sandboxed` and
  `run_sandboxed_with_map` (the engine evaluates a C string; -1 and the reason
  as the error; write `\0` inside a string literal instead);
- JSON text with a raw NUL, which is not JSON (`vm_set_global_json` says so;
  `push_json` returns -1 and also raises an Error in the script, since the
  callback may not look at the -1);
- the name of a global (`vm_set_global_*` returns the reason) or of a host
  function (`vm_register` returns -1): cut, it would be another name;
- a key or a value of the shared map, in the script's `aether_map_get` and
  `aether_map_put` (an Error the script can catch): Aether's runtime stores C
  strings there, not this bridge ([asks/shared-map-nul.md](../asks/shared-map-nul.md)).

UTF-16: a character beyond the BMP crosses as **real four-byte UTF-8** in both
directions (checked in-process and in the child; QuickJS never produces
CESU-8, which Duktape's bridge had to be taught to avoid). A **lone surrogate**
cannot be UTF-8; the engine writes it as the three bytes of the half (WTF-8),
and the bridge hands the host **U+FFFD** instead, so every string the host
reads is valid UTF-8. In-process and in the child alike, because it is one
bridge. The JSON readers (`vm_result_json`, `arg_json`) keep the half exactly,
as its `\udXXX` escape, and `push_json` and `vm_set_global_json` read it
back: JSON is the lossless way to move a string that has one. A host string
is decoded as the engine decodes any: a bad byte is U+FFFD, a pair written as
two three-byte halves (CESU-8) is joined into one character, and the three
bytes of a lone half make a lone half in the script. Backed by the specs
"a NUL crosses, both ways, at every string crossing", "an error message keeps
its NUL, in the script's error and the host's", "what cannot carry a NUL is
refused, saying why, and nothing is cut" and "a character past the BMP is
UTF-8, both ways, and a lone surrogate is U+FFFD" (`test_host_quickjs_vm.ae`),
the shared map's case in `test_host_quickjs.ae`, and the worker's `strings`
case.

How it compares with the four, function for function (`yes` is the same call
and meaning, `-` is none):

| | Lua | Python | Duktape | Tcl | QuickJS |
|---|---|---|---|---|---|
| `vm_new`, `vm_free`, `vm_register`, `vm_eval`, `vm_pop_result` | yes | yes | yes | yes | yes (`vm_free` is `@frees`) |
| `vm_set_global_str`, `_int`, `_strlist` | yes | yes | yes | yes | yes (returns "" or why not) |
| `vm_result_type`, `_int`, `_str`, `_bool` | yes | yes | yes | yes | yes |
| `vm_result_double` | - | yes | - | yes | yes |
| `vm_result_bytes`, `arg_bytes`, `push_bytes` | - | yes | - | - | yes (a `Uint8Array`) |
| `arg_count`, `arg_type`, `arg_str`, `arg_int`, `arg_bool` | yes | yes | yes | yes | yes |
| `arg_double` | - | yes | - | yes | yes |
| `push_int`, `_str`, `_bool`, `_nil`, `_double`, `raise_error` | yes | yes | yes | yes | yes |
| instruction limit | yes | yes | unavailable | yes (commands) | unavailable |
| time limit (`vm_set_time_limit`) | - | - | - | yes | yes |
| a table from a callback | table builder | - | - | - | `push_json` |
| `vm_set_global_json`, `vm_result_json`, `arg_json`, `push_json` | - | - | - | - | yes |
| `vm_set_cancel_cell` (any thread) | - | - | - | - | yes |
| `vm_set_user`, `vm_user`, `vm_busy`, `vm_new_capped` | - | - | - | - | yes |
| tags | 0-6 | 0-8 | 0-6 | 0-7 (`TFLOAT` is 7) | 0-8, Python's |

Where the four differ, QuickJS follows the richest, Python: the last push is
the value (Lua returns a count), `arg_str` converts nothing (`arg_str` of 5 is
""), `arg_bool` and `result_bool` are 1 only for `true`, a number that does
not fit reads saturated, and an error's text is `Type: message`.

The bridge's `run_sandboxed_with_map(perms, code, map_token)` is a VM with
two host functions, `aether_map_get(key)` and `aether_map_put(key, value)`,
bound to the map by the VM's own state (not the process's) and freezing the
host's inputs before it runs (a `put` to a frozen key is ignored).
`containment/host.ae` has the host functions the tests use, `read_file`,
`getenv`, `greet` and `add`, checking the grants themselves with
`aether_sandbox_check_slow`.

## In a contained child

`quickjs/worker/worker.ae` is a std.guest worker on the bridge's VM. In the
script, `print` and `console.log` are `SAY` (arguments String()'d and
space-joined), and each function the host mounts is a global of the same
name whose call is a `CALL` the host answers: the arguments go as a JSON
array (`JSON.stringify`: strings, numbers, booleans, null, arrays, objects)
and the answer is `JSON.parse`d back (a granted file comes back as
`{granted: true, text: ...}`). A `FAIL` throws an `Error` with the host's
message (`refused: ...` when the grants said no), which the script can catch;
an error the script does not catch, its own or a `FAIL`, ends it with one
`ERROR` frame, `Type: message`.

A `CANCEL` is heard inside a loop: the worker runs a watcher thread
(`worker/worker_support.c`) that waits on the socket while the script runs.
The host speaks unasked only to cancel or to set the log level, so when
something other than a `LOGLEVEL` arrives outside a call, the watcher sets
the VM's cancel cell, the engine's interrupt handler stops the JavaScript
where it is (the script cannot catch it), and the worker reads the frame and
ends with `ERROR cancelled by host: <the host's reason>`, as the Lua, Python,
Perl, Ruby and Tcl workers do. The watcher only peeks; every frame is read by
`std.guest.child` on the worker's own thread, and a call marks itself under
the watcher's lock before its `CALL` goes out, so a reply is never taken for a
cancel. A `CANCEL` that comes as a call's reply sets the cell too. One long
native call (a regexp is not one: it is interrupted) runs to its end before
the handler is consulted, and the host's kill after its grace period is what
holds that. The worker's runtime has a 64 MiB heap cap.
[containment/child_host.ae](containment/child_host.ae) is a complete host.

## Implementation notes

- The bridge's `aether_host_quickjs.c` declares the engine's `qjs_*` C
  functions by hand (`qjs_new`, `qjs_eval`, `qjs_get_index`, ...), matching
  aether's `contrib/quickjs/aether_quickjs.c`. `vm_register` is Aether
  (module.ae): it makes the engine's host function with a closure that
  captures the VM and the callback's address and calls the bridge's dispatch,
  so the closure's layout stays the compiler's business. The bridge is
  compiled into the importing program (`@source` in `module.ae`), together
  with the engine's C (`import contrib.quickjs`).
- `run` is a VM (32 MB heap cap, 5 s time limit) made, run and freed.
  `run_sandboxed*` push and pop the grants level around the run, also when the
  script throws. `run_sandboxed_with_map`'s map token is on its VM, so two
  runs on two threads at once each use their own map.
- `print`/`console.log` are the bridge's addition to aether's copy
  (PROVENANCE.md): aether's scripts could only compute and write to the
  shared map.
- A thrown error from `run` is printed to stderr as `[js] ...` with its
  stack and the call returns `-1`; a VM's `vm_result_str` has it without the
  stack. (A message with a NUL is printed whole, the stack lines after it.)
- The VM keeps four JavaScript functions no script can see, made when the VM
  is: `rethrow` (a host function whose body is the engine's `rethrow`) and
  three small ones, a catcher (`String(e)` of what `rethrow` throws), a thrower
  (`throw new Error(m)`) and a joiner (`a.join('\0')`): the strings section
  says what for. A VM whose heap is too small for them keeps working and falls
  back to the engine's own C text for an error (cut at a NUL).

## Tests

- [`test_host_quickjs.ae`](test_host_quickjs.ae): the bridge, through
  `import quickjs`: a script evaluates, a thrown error is a failure, host
  functions are held to the grants in force, the shared map round trips and
  refuses a NUL, script text with a NUL is refused, the
  grant stack is left as found, `init`/`finalize` are idempotent. Moved from
  aether but for the import and the file the host function reads
  (`/etc/hosts`: `/etc/hostname` is not on macOS).
- [`test_host_quickjs_vm.ae`](test_host_quickjs_vm.ae): the two-way VM, case
  for case with `lua/test_host_lua_vm.ae`, `python/test_host_python_vm.ae`,
  `tcl/test_host_tcl_vm.ae` and `duktape/test_host_duktape_vm.ae` where QuickJS
  has the thing (28 cases, four of them the strings section above): typed arguments and results, bytes, JSON, errors
  and `raise_error`, nested evals, the time limit, a cancel cell set from
  another thread, two VMs that share nothing, memory flat over 10,000 rounds;
  and the refusals: an unregistered name, bad names, wrong argument types, a
  VM freed from its own callback, a null VM.
- [`test_quickjs_bridge.ae`](test_quickjs_bridge.ae): `print` and
  `console.log`, the absence of `quickjs-libc`'s names, and ES2023.
- `containment/test_vm_free.sh`: a use or a second free of a freed VM does
  not compile (E0903).
- `containment/test_worker.sh` (contained child, Linux): the worker's own
  cases beside the shared boundary tests: a cancel the script tries to catch,
  a `LOGLEVEL` during a loop that is not a cancel, an uncaught `FAIL` and the
  script's own error as `ERROR`, an unmounted name, the JSON types of a host's
  answer, and `strings`: a NUL, an emoji and a lone surrogate through `print`
  and an error, byte for byte.
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh` and
  `../tests/test_boundary.sh`: the containment table and the boundary grid
  above.
- [`sanitize.sh`](sanitize.sh) (Linux, clang): all of the above built with
  AddressSanitizer and LeakSanitizer; the worker's cases a second time with
  the worker uncontained, where LeakSanitizer can run (the child's seccomp
  filter refuses the fork it needs).

The specs skip when `contrib.quickjs` cannot be used (on GitHub Actions a skip
is a failure). They assert on return codes, never on the interpreter's
stdout, which is not reliably flushed when an Aether program exits.
`../tests/run.sh` runs everything: `GUEST_LANG=quickjs sh tests/run.sh`.

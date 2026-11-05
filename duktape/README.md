# duktape: embedded JavaScript (Duktape)

`import duktape` lets an Aether program run JavaScript: [Duktape](https://duktape.org),
an embeddable ES5.1 engine. It is the bridge that was aether's
`contrib.host.duktape`, moved here ([PROVENANCE.md](PROVENANCE.md)): the same
functions under the same names, `duktape.run`, `duktape.run_sandboxed`,
`duktape.vm_new` and the rest. `import js` is kept as an alias
([../js/README.md](../js/README.md)).

The module is named for the engine, not the language, because "JS" is the one
slot where several runtimes could serve it (Duktape, QuickJS, V8); another
engine can sit beside this one under its own name.

Duktape is ES5.1 and nothing more: no `let`, `const`, arrow functions or
template strings, no modules, and **no I/O of its own**: no files, no
environment, no processes, no sockets, no way to load a library or call C.
That shapes everything below. What a script can reach is what the bridge gives
it as globals (`print`, `env`, `readFile`, `fileExists`, `writeFile`, `exec`,
each checking the sandbox's grants itself) and the host functions you register.
In-process Duktape is contained by construction for everything but those.

## Four ways to run JavaScript from Aether

From least to most contained. The first three run Duktape inside your
program, and the fourth runs it in a separate, locked-down process. All four
stay supported: pick by how far you trust the script, not by which is newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where JavaScript runs | your process | your process | your process | its own process (`duktape/worker`) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| The bridge's `env`, `readFile`, `writeFile`, `exec` | **not checked** | checked against the grants (every platform) | checked against the grants; `system()` held a second time | not given to the script at all |
| Platforms | everywhere libduktape loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0 | ae 0.878.0 (`std.guest`) |

### 1. Embed: Duktape in your process, no containment

```aether
import duktape

main() {
    script = <<JS
        var t = ['a', "b", 'c'];
        for (var i = 0; i < t.length; i++) print(i + 1, t[i]);
        print(JSON.stringify({ q: 'quote"d' }), Duktape.version);
    JS
    if duktape.run(script) != 0 { println("js failed") }
}
```

A heredoc (`<<JS ... JS`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape processing,
so JavaScript's quotes and backslashes need no escaping, and the indentation
the lines share is stripped. The closing marker sits on a line of its own. A
plain string works too: `duktape.run("print('hi')")`. This program
([examples/way1_embed.ae](examples/way1_embed.ae)) prints

```
1 a
2 b
3 c
{"q":"quote\"d"} 20700
```

`duktape.run` pushes no sandbox level, so the bridge's bindings are not
checked: the script can `readFile('/etc/passwd')` or `exec('rm ...')`
([examples/way1_uncontained.ae](examples/way1_uncontained.ae)). Use it for
scripts you wrote.

For a script that calls back into the host, keeps state between runs or
returns a typed value, use a persistent VM: `duktape.vm_new`, `vm_register`
for host callbacks, `vm_eval`, `vm_result_*`
([Bidirectional embedding](#bidirectional-embedding-904) below). That is
still this first way: in-process and uncontained, and the VM's script has the
same bindings, unchecked until you run it inside `sandbox.enforce`.

### 2. Embed inside a sandbox block

```aether
import duktape
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    script = <<JS
        print('HOME     ', env('HOME'));
        print('USER     ', env('USER'));
        print('passwd   ', readFile('/etc/passwd') === undefined ? 'refused' : 'read it');
        print('exec     ', exec('true'));
    JS
    if duktape.run_sandboxed(reader, script) != 0 { println("js failed") }
}
```

`run_sandboxed` pushes the block's grants as a level for the length of the
call. Aether's own std calls, the bridge's bindings and every host function the
script reaches are checked against them, on every platform. Unlike Lua or
Python, Duktape has no way to reach libc that does not go through one of
those, so this way is containment on its own, preload or none
([examples/way2_sandbox.ae](examples/way2_sandbox.ae)):

```
HOME      /Users/paul
USER      undefined
passwd    refused
exec      -1
```

`env`, `readFile` and `fileExists` answer `undefined` or `false`, `writeFile`
answers `false`, `exec` answers `-1`: a refusal and a failure look the same to
the script. What this way does not hold is the script's use of the machine
itself: a loop that never ends, or allocation until the process is out of
memory (libduktape as packaged has no execution limit or heap limit; see
[Implementation notes](#implementation-notes)).

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits between
the process and libc and holds its libc calls to the same grants (Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

The same program, granted only `HOME`, run on CachyOS under ae 0.889.0, without
the block, inside it, and inside it with the preload:

| JavaScript does | 1. `run`, no block | 2. block | 3. block + `LD_PRELOAD` |
|---|---|---|---|
| `env('HOME')` | `/home/paul` | `/home/paul` (granted) | `/home/paul` (granted) |
| `env('USER')` | `paul` | `undefined` | `undefined` |
| `readFile('/etc/passwd')` | the file | `undefined` | `undefined` |
| `exec('true')` | `0` | `-1` | `-1` |

The preload changes nothing here, and that is the finding, not a failure: the
bridge's bindings had already asked the grants before they reached libc, so
the preload has nothing left to refuse. It is a second line for the libc calls
under them (`system()` for `exec`, `fopen` for the file bindings), and for any C
you link into the same process; it is not what holds Duktape. It is a library
interposer, not a kernel boundary: a raw system call made without libc is not
seen, and macOS has no equivalent. The containment table below has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("js") {}
    args = list.new()
    list.add(args, "libduktape.so.207")
    g, err = guest.launch(perms, "build/duktape-worker", args, "js", "")
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
JavaScript runs in `duktape/worker`, a separate process `guest.launch` starts
under Landlock and seccomp from the policy, with the preload, and talking to
the host only over a socket (`std.ipc` frames). The worker loads libduktape and
then locks itself down with no grants before the script starts, and removes the
bridge's `env`, `readFile`, `fileExists`, `writeFile` and `exec` from the
script's globals: it reaches a file only when the host hands it a descriptor,
and an environment variable only through a host function. Duktape has no
standard library on disk and no module loader, so there is nothing to load
before the lockdown: the worker needs to read nothing after it starts. The host
can cancel a script at a deadline and kills a worker that stops answering. On
macOS, where nothing can contain the child, `guest.launch` refuses and starts
nothing. Build the worker once with
`ae build duktape/worker/worker.ae -o build/duktape-worker`; details under
[In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as Duktape comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | matches | matches | differs: exec-bin-sh stricter, fork left out |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: env-cached left out | differs: env-cached left out | differs: open-granted-file stricter, syscall-fn-openat left out, syscall-openat left out, getenv-home stricter, env-cached left out |
| `test_stunt_fork` | `fork` | matches | matches | differs: fork left out |
| `test_stunt_host_checked` | `-` | differs: spin left out | differs: spin left out | differs: spin left out, half-frame left out |
| `test_stunt_native` | `native=*` | matches | matches | differs: dlopen-libc-by-name left out, dlopen-libc-by-path left out |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | matches | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, syscall-socket left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out |
| `test_stunt_no_grants` | `-` | differs: env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, dlopen-libm left out | differs: env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, dlopen-libm left out | differs: env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, fork left out, dlopen-libc-by-name left out, dlopen-libc-by-path left out, dlopen-libm left out, syscall-fn-openat left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | matches | matches | differs: write-granted-file stricter |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Duktape libduktape.207.20700.so
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Duktape libduktape.so.207
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Duktape libduktape.so.207
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Duktape libduktape.so.207
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Duktape libduktape.so.207
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt` at
`AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in `../tests/test_stunt_*.sh`
with the same policies; the containment table below has each row. The table is
generated from a run (`sh tests/run.sh --write-parity`) and checked against
every run (`tests/parity.sh`), in the columns the host can run; the lines under
it say which host produced which column.

Most of what differs is **left out**: Duktape cannot express the row (no
sockets, no `fork`, no `syscall()`, no way to load a library), so there is
nothing to hold and nothing to test. That is the honest result for an engine
with no I/O surface of its own, not a gap in the table. The rows that remain
are the bridge's bindings, which match stunt in-process on every platform; and
the contained child, which is stricter than the policy where the bridge's
bindings would have allowed a thing and the worker does not give them to the
script.

## Containment table

What a JavaScript script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Duktape way
([containment/checks.js](containment/checks.js)), under the policies of aether's
`tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`). "Per
policy" means granted where the policy grants it and refused where it does not,
as stunt expects. The columns are the ways above: "in-process, Linux (preload)"
is way 3, "in-process, macOS" is way 2 where there is no preload, and
"contained child" is way 4. Every cell is a test outcome (`../tests/run.sh`):
checked on macOS arm64 (Duktape 2.7.0) and on CachyOS x86_64, kernel 7.3 with
Landlock (Duktape 2.7.0), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`readFile`) | refused | refused | refused: the script has no `readFile` |
| `open-granted-file` (`readFile`) | per policy | per policy | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`writeFile`) | per policy | per policy | refused under every policy: no `writeFile` |
| `getenv-home` (`env`) | per policy | per policy | refused under every policy: no `env`, and the worker has no environment; `getenv` is a host function |
| `exec-bin-sh` (`exec('/bin/sh')`) | per policy | per policy | refused under every policy: no `exec`, and the worker's lockdown has no exec grant |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: a `ReferenceError` the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `silent` (`for (;;) {}`) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `print`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `spin` | left out: no instruction limit in libduktape as packaged | | |
| `env-cached` | left out: Duktape keeps no copy of the environment | | |
| `connect-192.0.2.1`, `connect-ipv4-mapped` | left out: no sockets | | |
| `fork` | left out: no fork | | |
| `syscall-fn-openat`, `syscall-openat`, `syscall-socket` | left out: no `syscall()`, no way to call C | | |
| `dlopen-libm`, `dlopen-libc-by-name`, `dlopen-libc-by-path` | left out: no way to load a library | | |
| `half-frame` | left out: a script cannot write part of a frame | | |

What the table says:

- **In-process, the bridge's bindings hold on every platform, with or
  without the preload.** They are not Duktape's: they are four functions
  (`env`, `readFile`/`fileExists`, `writeFile`, `exec`) the bridge adds, each
  of which asks `aether_sandbox_check` first. Nothing in Duktape's own library
  reaches libc, so unlike Lua or Python there is no row that only the preload
  can hold and that macOS therefore does not. (The table has no "not held" and
  no "escapes".) Outside a sandbox block, `duktape.run` checks nothing.
- **`exec` grants name the command line the script passes**, not a program:
  `exec('/bin/sh')` is held by `exec=/bin/sh`; `exec('ls -l')` would need a
  grant matching `ls -l`. The script's string goes to `system()` after the
  check, so a granted pattern is as wide as the shell makes it.
- **The contained child holds every row it runs**, on Linux, and is stricter
  than the policy: the script is not given the bridge's bindings at all, so a
  script reaches a granted file only through the host (`read_file`, answered
  with a descriptor), a variable only through `getenv`, and never starts a
  process. There is nothing to load before the lockdown, which Lua's and
  Python's workers have to do.
- **Nothing here ends a loop.** The `spin` row is left out: libduktape as
  packaged has no execution limit, so the in-process bridge cannot stop a
  script, and the worker has no hook to look for the host's CANCEL from inside
  one. In the child the host's deadline and grace period still end it, by
  killing the worker: that is the `silent` row (a script that never answers).
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row would run expecting the call to succeed, so the day the preload
  stops holding something this goes red. There are none to hold today.

## Loading libduktape

The bridge's C (`aether_host_duktape.c`) is compiled into the program that
imports `duktape` (`@source` in `module.ae`); there is no archive to build
first and no `aether.toml` entry to add. libduktape is not linked: the bridge
`dlopen`s it at the first call, so a program built on one machine runs against
whatever Duktape 2.x library the machine it runs on has. The Duktape dev
package (`duktape.h`) is not needed to build; `duktape_abi.h` carries the
types.

Where the bridge looks for libduktape, in order:

1. `${AETHER_DUKTAPE_SONAME}`: an exact soname or path (`libduktape.so.207`
   on Debian and Ubuntu, `libduktape.so.207` or `.so.2.x` on Arch,
   `/opt/homebrew/lib/libduktape.so` on macOS: Homebrew installs a `.so`);
2. `libduktape.so.207`, `libduktape.so.2`, `libduktape.so`;
3. Homebrew's and `/usr/local`'s, `.dylib` then `.so`.

`../tests/duktape_env.sh` is the probe the tests use: `brew --prefix duktape`
on macOS, the loader's cache on Linux. Duktape is embedded-only (there is no
`duktape` command to ask; Homebrew's `duk` is the stand-alone shell, which the
bridge does not use).

## In a contained child

`duktape/worker/worker.ae` is a std.guest worker. It takes the libduktape
soname as its one argument, since it is given no environment, and loads it
before it locks itself down. In the script, `print` is `SAY`, and each
function the host mounts is a global of the same name whose call is a `CALL`
the host answers: the arguments go as a JSON array and the answer is
`JSON.parse`d back (a granted file comes back as
`{granted: true, text: ...}`). A `FAIL` throws an `Error` with the host's
message (`refused: ...` when the grants said no); a `CANCEL` throws
`cancelled by host: ...`, which the script can catch: the host's kill after
its grace period is what holds it. [containment/child_host.ae](containment/child_host.ae)
is a complete host.

## Bidirectional embedding (#904)

The fire-and-forget `run_sandboxed` is enough to *run* a script, but a host
that uses JavaScript as a first-class, callback-driven extension surface needs
a **persistent, host-owned VM** that registers host callbacks the script can
call, injects globals, and returns a **typed** result. That tier:

```aether,fragment
import duktape

@c_callback
app_add(vm: ptr) -> int {
    a = duktape.arg_int(vm, 1)
    b = duktape.arg_int(vm, 2)
    duktape.push_int(vm, a + b)
    return 1
}

main() {
    vm = duktape.vm_new()
    duktape.vm_register(vm, "app.math.add", app_add)
    duktape.vm_set_global_str(vm, "NAME", "Aether")
    duktape.vm_set_global_int(vm, "BASE", 100)

    status = duktape.vm_eval(vm, "app.math.add(2, 3) + BASE")
    t = duktape.vm_result_type(vm)
    if t == duktape.DUK_TINT { reply = duktape.vm_result_int(vm) }
    duktape.vm_pop_result(vm)
    duktape.vm_free(vm)
}
```

A host callback is a `@c_callback` Aether fn `(vm: ptr) -> int` that reads its
arguments (1-based) and pushes its result. It returns `1` when it pushed a
value (Duktape reads a C function's result as the stack top, so pushing two
and returning `2` gives the second), `0` for `undefined`, or
`return duktape.raise_error(vm, msg)` to throw a JavaScript `Error`: the error
is thrown into the script after the callback returns, so the callback's
`defer`s still run.

| Call | Effect |
|------|--------|
| `duktape.vm_new() -> ptr` | create a persistent host-owned VM handle, with the bindings below |
| `duktape.vm_register(vm, name, fn_ptr) -> int` | register an Aether `(vm: ptr) -> int` `@c_callback` as a JS function (dotted names like `"app.math.add"` create/reuse parent objects); `-1` for a name over 255 characters |
| `duktape.vm_set_global_str/_int/_strlist(vm, name, v)` | inject a global before a run (string / number / string array) |
| `duktape.vm_eval(vm, code) -> int` | run; leaves the single return value (or the thrown value, made an `Error`) on the VM stack |
| `duktape.vm_result_type(vm) -> int` | typed tag of the result (`DUK_T*`) |
| `duktape.vm_result_int/_str/_bool(vm)` | read the typed result (a number outside int64 saturates, NaN and non-numbers read as 0) |
| `duktape.vm_pop_result(vm)` | clear the stack after reading |
| `duktape.vm_set_instruction_limit(vm, budget, every) -> string` | returns `"execution limit unavailable: libduktape built without DUK_USE_EXEC_TIMEOUT_CHECK"`: system libduktape packages disable that build option, so there is no limit to set |
| `duktape.arg_count/_type/_str/_int/_bool(vm, i)` | **inside a callback**: read args (1-based) |
| `duktape.push_int/_str/_bool/_nil/_double(vm, v)` | **inside a callback**: push a result |
| `duktape.raise_error(vm, msg) -> int` | raise a JS `Error` from a callback: `return duktape.raise_error(vm, msg)` |

Typed-value tags exported as consts: `DUK_TNIL`, `DUK_TBOOL`, `DUK_TINT`,
`DUK_TSTR`, `DUK_TTABLE` (any object, arrays included), `DUK_TERR`,
`DUK_TOTHER`. `DUK_TINT` is every JavaScript number: Duktape has one numeric
type.

There is **no table builder** (Lua's `table_begin` and friends, #910). To
return a structure from a callback, build it in JavaScript: push a JSON string
and let the script `JSON.parse` it (`host.keys()` returning
`'["k1","k2"]'`), or read one with `JSON.stringify` and `arg_str`.

### The bindings a script is given

`duktape.vm_new` and the first `run` each give the script these globals; the
last two come only with `run_sandboxed_with_map`:

| Global | What it does | Checked against |
|---|---|---|
| `print(...)` | writes its arguments, space-separated, and a newline to stdout | nothing (in the worker it is `SAY`) |
| `env(name)` | `getenv`: the value or `undefined` | `env` grant |
| `readFile(path)`, `fileExists(path)` | the file's text or `undefined`; `true` or `false` | `fs_read` grant |
| `writeFile(path, text)` | `true` or `false` | `fs_write` grant |
| `exec(command)` | `system()`: the exit status, or `-1` | `exec` grant, on the command string |
| `aether_map_get(key)`, `aether_map_put(key, value)` | the host's shared map | the map's token: inputs the host froze cannot be overwritten |

Checks apply only inside a sandbox level (`run_sandboxed`, or the VM inside
`sandbox.enforce`); `duktape.run` and a VM run bare check nothing.

## Implementation notes

- All Duktape C-API access goes through a `dlsym` function-pointer table: see
  `aether_host_duktape.c`. The bridge has no unresolved Duktape symbols, and
  needs none of Duktape's headers: the types and constants it uses are in
  `duktape_abi.h` (Duktape 2.x's own definitions under the default
  `duk_config.h`: `duk_int_t` is `int`, `duk_idx_t` and `duk_ret_t` too,
  `duk_bool_t` and `duk_uarridx_t` `unsigned int`, `duk_double_t` `double`).
  A libduktape built with a different `duk_config.h` is not supported.
- Duktape's headers `#define` a few convenience macros over real functions,
  which the bridge re-implements as inline helpers over the table:
  `duk_create_heap_default()` is `duk_create_heap(NULL, NULL, NULL, NULL, NULL)`,
  `duk_peval_string(c, s)` is `duk_eval_raw(c, s, 0, FLAGS)` and
  `duk_safe_to_string(c, i)` is `duk_safe_to_lstring(c, i, NULL)`. The
  `DUK_COMPILE_*` flags `duk_peval_string` uses are in `duktape_abi.h`; they
  are Duktape's public compile-flags interface. `DUK_VARARGS` is `-1`, what
  `duk_push_c_function` takes for a function that accepts any number of
  arguments.
- Every other function the bridge calls is exported by the library; a missing
  one is reported by name when libduktape is first loaded.
- A host callback is called through one trampoline function. The callback's
  address rides on the JS function object as a hidden property
  (`"\xFFhost_fn"`, an internal key a script cannot name) and is read back with
  `duk_push_current_function`.
- `raise_error` pushes the Error and returns a sentinel; the trampoline
  throws after the Aether callback has returned, because `duk_throw_raw` is a
  `longjmp` and would skip the callback's `defer`s.
- A JavaScript number read as int64 is checked first: NaN and non-numbers read
  as 0 and a value out of range saturates, since a C cast of either is
  undefined behaviour.
- A thrown non-Error (`throw "x"`) is made an `Error` on the stack by
  `vm_eval`, so `vm_result_type` is `DUK_TERR` whatever was thrown.
- Known, kept from aether's copy: libduktape packages are built without
  `DUK_USE_EXEC_TIMEOUT_CHECK`, so `vm_set_instruction_limit` only says so.
  Nothing bounds a script's time, and nothing bounds its memory (the heap uses
  `malloc`).

## Tests

- [`test_host_duktape.ae`](test_host_duktape.ae): the fire-and-forget surface
  and the two-way tier, through `import duktape`: a script evaluates, state
  persists, runtime and syntax errors are failures, `init` is idempotent, a
  persistent VM with dotted callbacks and typed values (the cases of aether's
  `host_duktape_bidirectional` harness), errors, numbers outside int64 and a
  callback's cleanup when it raises. Moved from aether but for the import.
- [`test_host_duktape_vm.ae`](test_host_duktape_vm.ae): the callback-side
  readers and `push_double`, the execution-limit message, and the bindings
  `run_sandboxed` gives a script, each checked against the grants.
- [`../js/test_js_alias.ae`](../js/test_js_alias.ae): the alias.
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: the containment
  table above.

The specs skip when libduktape cannot be loaded (on GitHub Actions a skip is a
failure). They assert on return codes and on state read back from JavaScript,
never on the interpreter's stdout, which is not reliably flushed when an
Aether program exits. `../tests/run.sh` runs everything:
`GUEST_LANG=duktape sh tests/run.sh`.

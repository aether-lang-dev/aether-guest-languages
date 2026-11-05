# lua: embedded Lua

`import lua` lets an Aether program run Lua in its own process, or in a
contained child. It is the bridge that was aether's `contrib.host.lua`,
moved here ([PROVENANCE.md](PROVENANCE.md)): the same functions under the
same names, `lua.run`, `lua.run_sandboxed`, `lua.vm_new` and the rest.

| | In-process (`import lua`) | Contained child (`lua-worker`, `std.guest`) |
|---|---|---|
| Where Lua runs | in the Aether program | in a separate process, `lua/worker` |
| What holds Lua's own `io`/`os` calls | `libaether_sandbox.so` in `LD_PRELOAD`, Linux only | Landlock and seccomp from the policy, plus the worker's own lockdown with no grants |
| macOS, Windows | runs; nothing holds Lua's `io`/`os` calls | `guest.launch` refuses and starts nothing |
| Host functions | `vm_register` callbacks, called directly | `guest.mount`; each call is a frame the host answers |
| Needs | ae 0.875.0 | ae 0.878.0 (std.guest), Linux with Landlock |

## Containment table

What a Lua script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Lua way
([containment/checks.lua](containment/checks.lua)), under the policies of
aether's `tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`).
"Per policy" means granted where the policy grants it and refused where it
does not, as stunt expects. Every cell is a test outcome
(`../tests/run.sh`): checked on macOS arm64 (Lua 5.5.1 and 5.4.9) and on
CachyOS x86_64, kernel 7.3 with Landlock (Lua 5.5.1 and 5.4.9), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`io.open`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`io.open`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`io.open(.., "w")`) | per policy | **not held** | refused under every policy |
| `getenv-home` (`os.getenv`) | per policy | **not held** | refused under every policy: the worker has no environment; `getenv` is a host function |
| `exec-bin-sh` (`os.execute`) | per policy, from ae 0.884.0; before it **escapes**: `/bin/sh` runs with no exec grant | **not held** | refused under every policy, from ae 0.884.0: the worker's lockdown has no exec grant; before it, per policy (seccomp) |
| `dlopen-libm` (`package.loadlib`) | loads | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` | (child-only row) | (child-only row) | refused under every policy, `native=*` included |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work | work |
| `callback-unregistered` | refused: a nil global, an error the script can catch | same | same |
| `map-get`, `map-set` | work | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same | same |
| `spin` (`while true do end`) | stopped by the instruction limit | same | cancelled at the deadline, from the worker's count hook |
| `silent` (`debug.sethook()` then a loop) | (child-only row) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `print`) | (child-only row) | (child-only row) | killed: the frame is refused |
| `env-cached` | left out: Lua keeps no copy of the environment | | |
| `connect-192.0.2.1`, `connect-ipv4-mapped` | left out: no sockets in Lua's standard library | | |
| `fork` | left out: no fork in Lua's standard library | | |
| `syscall-fn-openat`, `syscall-openat`, `syscall-socket` | left out: no `syscall()` in Lua's standard library | | |
| `half-frame` | left out: a script cannot write part of a frame | | |

What the table says:

- **In-process is the easy default and is not containment on its own.**
  On Linux the preload holds Lua's file and environment calls, and from
  ae 0.884.0 `os.execute` and `io.popen` too: they need the fork grant and
  exec of `/bin/sh` (aether #3580). Before 0.884.0 they escaped it: glibc's
  `system()` starts `/bin/sh` with `posix_spawn`, whose `clone` and `execve`
  happen inside libc, where the preload's `fork` and `execve` wrappers never
  saw them. On macOS nothing holds them at all. The host functions
  (`binding-*`), which check the grants themselves, hold everywhere.
- **The contained child holds every row it runs**, on Linux, and is
  stricter than the policy: the worker locks itself down with no grants
  before the script starts, so a script reaches a granted file only through
  the host (`read_file`, answered with a descriptor) and never gets a handle
  on libc. From ae 0.884.0 that includes process start: `os.execute` is
  refused under every policy, because the worker's own lockdown has no exec
  grant and the preload now holds `system()` to it. Before 0.884.0 process
  start was the one row that followed the policy (seccomp lets `execve`
  through only with an exec grant).
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row runs expecting the call to succeed, so the day the preload
  holds it that test fails and this table is brought up to date. The
  `exec-bin-sh` row is checked on either side of ae 0.884.0
  (`AE_SPAWN_PIN` in `ci/versions.env`), from the ae the tests run on.

## Using it

```aether
import lua

main() {
    lua.run("print('hello from lua')")
}
```

The bridge's C (`aether_host_lua.c`) is compiled into the program that
imports `lua` (`@source` in `module.ae`); there is no archive to build
first and no `aether.toml` entry to add. liblua is not linked: the bridge
`dlopen`s it at the first call, so a program built on one machine runs
against whatever Lua 5.3, 5.4 or 5.5 the machine it runs on has.

Where the bridge looks for liblua, in order:

1. `${AETHER_LUA_SONAME}`: an exact soname or path (`liblua5.4.so.0` on
   Debian and Ubuntu, `liblua.so.5.5` on Arch,
   `/opt/homebrew/lib/liblua.dylib` on macOS);
2. `liblua.so`, the unversioned name, where the system has one.

There is no list of versions to try: the program that starts yours owns
that choice. `../tests/lua_env.sh` is one way to make it.

### Sandboxed, in-process

```aether
import lua
import std.sandbox

main() {
    worker = sandbox.new("worker") { grant_env("HOME") }
    lua.run_sandboxed(worker, "print(os.getenv('HOME'))")
}
```

`run_sandboxed` pushes the grants as a level for the length of the call;
std's own calls and any host function you give the VM are checked against
them everywhere, and Lua's own `io`/`os` calls only when the program runs
with `LD_PRELOAD=<ae's lib dir>/libaether_sandbox.so` (Linux). Read the
table above before relying on it.

### In a contained child

`lua/worker/worker.ae` is a std.guest worker: build it once
(`ae build lua/worker/worker.ae -o lua-worker`) and start it from a host
with `guest.launch`. The worker takes the liblua soname as its one
argument, since it is given no environment; it loads liblua before it
locks itself down. In the script, `print` is `SAY`, and each function the
host mounts is a Lua global of the same name whose call is a `CALL` the
host answers (a granted file comes back as `{granted = true, text = ...}`).
[containment/child_host.ae](containment/child_host.ae) is a complete host.

```aether
import std.guest
import std.sandbox
import std.list

main() {
    perms = sandbox.new("lua") {}
    args = list.new()
    list.add(args, "liblua5.4.so.0")
    g, err = guest.launch(perms, "build/lua-worker", args, "lua", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    guest.start(&g, "print(greet('lua'))")
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

## Bidirectional embedding: a Redis-class host (#904)

The fire-and-forget `run_sandboxed` is enough to *run* a script, but a host
that uses Lua as a first-class, callback-driven extension surface (Redis-style
`EVAL`/`FCALL`) needs a **persistent, host-owned VM** that registers host
callbacks the script can call, injects globals, and returns a **typed** result.
That tier:

```aether,fragment
import lua

// A host callback: reads typed args (1-based), pushes a typed result,
// returns the number of values pushed (the Lua C-function rule). It must be
// `@c_callback` so its symbol is addressable as a `ptr` value (see
// aether's docs/c-interop.md "Exporting an Aether Function as a C Callback").
@c_callback
redis_call(vm: ptr) -> int {
    cmd = lua.arg_str(vm, 1)
    // ...dispatch the command in the host, marshal the reply...
    lua.push_str(vm, "OK")
    return 1
}

main() {
    vm = lua.vm_new()                                   // persistent VM handle
    lua.vm_register(vm, "redis.call", redis_call)       // pass the @c_callback fn
    lua.vm_set_global_strlist(vm, "KEYS", keys)         // inject KEYS/ARGV
    lua.vm_set_global_strlist(vm, "ARGV", argv)
    lua.vm_set_instruction_limit(vm, 100000000, 1000)   // script-timeout guard

    status = lua.vm_eval(vm, source)                    // 0 ok, -1 error
    t = lua.vm_result_type(vm)                          // LUA_TINT/TSTR/TERR/…
    if t == LUA_TSTR { answer = lua.vm_result_str(vm) }
    else { if t == LUA_TINT { n = lua.vm_result_int(vm) } }
    lua.vm_pop_result(vm)
    lua.vm_free(vm)
}
```

| Call | Effect |
|------|--------|
| `lua.vm_new() -> ptr` | create a persistent host-owned VM (libs opened) |
| `lua.vm_register(vm, name, fn_ptr) -> int` | register an Aether `(vm: ptr) -> int` callback as a Lua global (dotted names like `"redis.call"` create/reuse the prefix table) |
| `lua.vm_set_global_str/_int/_strlist(vm, name, v)` | inject a global before a run (string / int / string-array) |
| `lua.vm_eval(vm, code) -> int` | run; leaves the single return value on the VM stack |
| `lua.vm_result_type(vm) -> int` | typed tag of the result (`LUA_T*`) |
| `lua.vm_result_int/_str/_bool(vm)` | read the typed result |
| `lua.vm_pop_result(vm)` | clear the stack after reading |
| `lua.vm_set_instruction_limit(vm, budget, every)` | count-hook guard; abort after `budget` VM instructions |
| `lua.arg_count/_type/_str/_int/_bool(vm, i)` | **inside a callback**: read args (1-based) |
| `lua.push_int/_str/_bool/_nil(vm, v)` | **inside a callback**: push a result |
| `lua.push_double(vm, v)` | push a RESP3 Double (integral → Lua number, else precise string) |
| `lua.push_tagged_table(vm, "ok"\|"err", msg)` | push a Redis-style `{ok=…}`/`{err=…}` table |
| `lua.raise_error(vm, msg) -> int` | raise a Lua error from a callback (`return lua.raise_error(vm, m)`) |

Typed-value tags exported as consts: `LUA_TNIL`, `LUA_TBOOL`, `LUA_TINT`,
`LUA_TSTR`, `LUA_TTABLE`, `LUA_TERR`, `LUA_TOTHER`.

### Returning arrays / maps / nested tables from a callback (#910)

A faithful `redis.call` returns RESP multibulks: arrays (`KEYS`, `LRANGE`,
`SMEMBERS`), maps (`HGETALL`), and nested arrays-of-arrays. The table builder
constructs them inside a callback. **The table being built is the top of the
stack:** `table_begin` pushes a fresh one, the `seti_*`/`set_*` ops write into
it, and nesting is just another `table_begin` closed with `table_end_seti` /
`table_end_setfield`.

| Call | Effect |
|------|--------|
| `lua.table_begin(vm, narr, nrec)` | push a fresh table (`narr`/`nrec` are size hints) |
| `lua.table_seti_str/_int/_bool(vm, i, v)` | array set: `t[i] = v` (1-based) |
| `lua.table_set_str/_int/_bool(vm, key, v)` | map set: `t[key] = v` |
| `lua.table_end_seti(vm, i)` | close the current child table into its parent at `[i]` |
| `lua.table_end_setfield(vm, key)` | close the current child table into its parent at `[key]` |
| `lua.table_finish(vm)` | finish the outermost table (it stays on the stack as the return) |

```aether,fragment
// redis.call('KEYS', pattern) -> {"k1","k2","k3"}
@c_callback
redis_keys(vm: ptr) -> int {
    lua.table_begin(vm, 3, 0)
    lua.table_seti_str(vm, 1, "k1")
    lua.table_seti_str(vm, 2, "k2")
    lua.table_seti_str(vm, 3, "k3")
    lua.table_finish(vm)
    return 1                      // the table is the single return value
}

// array-of-arrays: { {"a","b"}, {"c"} }
@c_callback
nested(vm: ptr) -> int {
    lua.table_begin(vm, 2, 0)     // outer
    lua.table_begin(vm, 2, 0)     // inner #1 (now on top)
    lua.table_seti_str(vm, 1, "a")
    lua.table_seti_str(vm, 2, "b")
    lua.table_end_seti(vm, 1)     // outer[1] = inner#1
    lua.table_begin(vm, 1, 0)     // inner #2
    lua.table_seti_str(vm, 1, "c")
    lua.table_end_seti(vm, 2)     // outer[2] = inner#2
    lua.table_finish(vm)
    return 1
}
```

This is the next tier above the factor bridge's two-way persistent-VM + shared-
map model: **host-callbacks-with-typed-marshalling**, in both directions.

## Implementation notes

- All Lua C-API access goes through a `dlsym` function-pointer
  table: see `aether_host_lua.c`. The bridge has no unresolved Lua
  symbols, and needs none of Lua's headers: the types it uses are in
  `lua_abi.h`.
- Lua's headers `#define` many macros that wrap real functions
  (e.g. `luaL_dostring(L,s)` = `luaL_loadstring(L,s) || lua_pcall(…)`,
  `lua_pop(L,n)` = `lua_settop(L, -(n)-1)`). The bridge re-implements
  these as inline static functions over the dlopen table, only the
  underlying real functions need dlsym entries.
- Symbol versioning (`lua_close@@LUA_5.4`): dlsym strips version
  tags, so `dlsym(h, "lua_close")` works against 5.3, 5.4 or 5.5
  indifferently.
- Lua 5.5 exports `luaL_openselectedlibs` and no `luaL_openlibs`, and
  moved `LUA_REGISTRYINDEX` (which a host callback's upvalue is read
  relative to). Which open function the library exports tells the
  bridge which of the two it has.
- Known, kept from aether's copy: the instruction limit counts hook
  calls, not instructions. `vm_set_instruction_limit(vm, budget, every)`
  stops after `budget` calls of a hook that fires every `every`
  instructions, so after about `budget * every` instructions.
- `aether_map_put` returns nothing to Lua, so a script cannot see that a
  write to a frozen input was refused (the containment table's
  `map-overwrite-input` row reads the key back).

## Tests

- [`test_host_lua.ae`](test_host_lua.ae): the fire-and-forget surface,
  through `import lua`: a script evaluates, the interpreter really runs
  (state persists between runs), runtime and syntax errors are failures,
  `init` is idempotent. Moved from aether unchanged but for the import.
- [`test_host_lua_vm.ae`](test_host_lua_vm.ae): the two-way tier (#904)
  and the table builder (#910), with `@c_callback` host functions:
  aether's `host_lua_bidirectional` and `host_lua_table_builder` C
  harnesses as an Aether spec.
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: the
  containment table above.

Both specs skip when liblua cannot be loaded (on GitHub Actions a skip is a
failure). They assert on return codes and Lua-side state, never on the
interpreter's stdout, which is not reliably flushed when an Aether program
exits. `../tests/run.sh` runs everything.

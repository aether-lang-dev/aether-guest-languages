# lua: embedded Lua

`import lua` lets an Aether program run Lua. It is the bridge that was
aether's `contrib.host.lua`, moved here ([PROVENANCE.md](PROVENANCE.md)):
the same functions under the same names, `lua.run`, `lua.run_sandboxed`,
`lua.vm_new` and the rest.

## Four ways to run Lua from Aether

From least to most contained. The first three run Lua inside your
program, and the fourth runs it in a separate, locked-down process. All
four stay supported: pick by how far you trust the script, not by which is
newest.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Lua runs | your process | your process | your process | its own process (`lua/worker`) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Lua's own `io` / `os` calls | not checked | **not checked** | checked against the grants (Linux) | refused, beyond what the host hands over |
| Platforms | everywhere liblua loads | same | Linux | Linux with Landlock; macOS refuses to start it |

### 1. Embed: Lua in your process, no containment

```aether
import lua

main() {
    script = <<LUA
        local t = { "a", 'b', [[c]] }
        for i, v in ipairs(t) do print(i, v) end
        print(string.format("%q", 'quote"d'))
    LUA
    if lua.run(script) != 0 { println("lua failed") }
}
```

A heredoc (`<<LUA ... LUA`, aether's language reference, "Heredoc strings")
carries the script verbatim: no `${}` interpolation and no escape
processing, so Lua's quotes, `[[long strings]]` and backslashes need no
escaping, and the indentation the lines share is stripped. The closing
marker sits on a line of its own. A plain string works too:
`lua.run("print('hi')")`.

For a script that calls back into the host, keeps state between runs or
returns a typed value, use a persistent VM: `lua.vm_new`, `vm_register`
for host callbacks, `vm_eval`, `vm_result_*`
([Bidirectional embedding](#bidirectional-embedding-a-redis-class-host-904)
below). That is still this first way: in-process and uncontained.

### 2. Embed inside a sandbox block

```aether
import lua
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    if lua.run_sandboxed(reader, "print(os.getenv('HOME'))") != 0 { println("lua failed") }
}
```

`run_sandboxed` pushes the block's grants as a level for the length of the
call. Aether's own std calls and every host function the script reaches are
checked against them, on every platform. Lua's own `io`, `os` and
`package.loadlib` go straight to libc, so on their own they are **not**
checked: this program prints `$HOME`, and the same script could read
`/etc/passwd` or run a shell just as well. Use this way for scripts you
trust, where the grants describe what the host lends them.

### 3. The same, under `LD_PRELOAD`

Run the program from way 2 with aether's sandbox preload, which sits
between Lua and libc and holds Lua's own calls to the same grants
(Linux):

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

The same script, granted only `HOME`, run on CachyOS,
first without the preload and then with it:

| Lua does | without | with `LD_PRELOAD` |
|---|---|---|
| `os.getenv("HOME")` | `/home/paul` | `/home/paul` (granted) |
| `os.getenv("USER")` | `paul` | `nil` |
| `io.open("/etc/passwd")` | opens it | refused |
| `os.execute("true")` | runs `/bin/sh` | refused: `Operation not permitted` |

It is a library interposer, not a kernel boundary: a raw system call made
without libc is not seen, and macOS has no equivalent. `os.execute` and
`io.popen` are held (aether #3580). The containment table below has every row.

### 4. A contained child, over `std.guest`

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

Lua runs in `lua/worker`, a separate process `guest.launch` starts under
Landlock and seccomp from the policy, with the preload, and talking to the
host only over a socket (`std.ipc` frames). The worker loads liblua and
then locks itself down with no grants before the script starts, so the
script reaches a file only when the host hands it a descriptor, and an
environment variable only through a host function. The host can cancel a
script at a deadline and kills a worker that stops answering. On macOS,
where nothing can contain the child, `guest.launch` refuses and starts
nothing. Build the worker once with
`ae build lua/worker/worker.ae -o lua-worker`; details under
[In a contained child](#in-a-contained-child) below.

## Stunt parity

Each of stunt's tests, as Lua comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects,
else the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | matches | differs: exec-bin-sh stricter, fork left out |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-etc-passwd not held, write-granted-file not held, env-cached left out | differs: env-cached left out | differs: open-granted-file stricter, syscall-fn-openat left out, syscall-openat left out, getenv-home stricter, env-cached left out |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | matches | differs: fork left out |
| `test_stunt_host_checked` | `-` | matches | matches | differs: half-frame left out |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | differs: dlopen-libc-by-name stricter, dlopen-libc-by-path stricter |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, open-etc-passwd not held | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, syscall-socket left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home not held, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, exec-bin-sh not held | differs: env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, fork left out, syscall-fn-openat left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | differs: write-granted-file stricter |

- Column 2: macOS 27.3 arm64, ae 0.934.0, Lua liblua.5.5.1.dylib
- Column 3: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, Lua liblua.so.5.5
- Column 4: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, Lua liblua.so.5.5
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies; the containment table
below has each row. The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it
say which host produced which column.

## Boundary tests

The shared tests in `tests/boundary.py`, run in this language's worker (`tests/test_boundary.sh`): one script per case, written in lua (`containment/boundary.tsv`), and what the host saw on the wire. `ok` means the value crossed as the same JSON value; `n/a` means the language has no way to write the case; anything else is what happened instead. Nothing in the tests lists what a worker can do.

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
| `int-max` | ok | ok |
| `int-min` | ok | ok |
| `float` | ok | ok |
| `bool-true` | ok | ok |
| `bool-false` | ok | ok |
| `null` | ok | ok |
| `array` | error | error |
| `map` | error | error |
| `nested` | error | error |
| a call inside a call's argument (`nest`) | ok | |
| an error answer (`FAIL`) to a call (`refuse`) | returned as a value | |
| a cancel while a call waits (`wait`) | heard at the call | |
| a cancel inside a guest loop (`loop`) | heard in a loop | |
| a 100,000-byte argument (`big-up`) | refused, worker killed | |
| a 100,000-byte answer (`big-down`) | refused: killed | |
<!-- boundary:end -->

## Boundary tests, in-process

The same cases (`tests/boundary.py run-inproc`) against this language's bridge in this process, through the API it really has (`containment/boundary_host.ae`, an Aether program written against it). There is no wire: "what the host saw" is what its typed readers made of the argument, so `differs` says what was lost. `ok` means the value came back as the same JSON value; `n/a` means the bridge has no call that could express the case; `refused` is a deliberate, documented refusal. Nothing in the tests lists what a bridge can do. For a table the host can build one but not read one, so a table read back in the host is a tag.

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
| `int-max` | ok | ok | same | same |
| `int-min` | ok | ok | same | same |
| `float` | differs: the host saw [0] | differs: the host saw ["1.5"] | same | same |
| `bool-true` | ok | ok | same | same |
| `bool-false` | ok | ok | same | same |
| `null` | ok | ok | same | same |
| `array` | differs: the host saw ["<a table>"] | n/a | same | same |
| `map` | differs: the host saw ["<a table>"] | differs: the host saw ["<a table>"] | same | same |
| `nested` | differs: the host saw ["<a table>"] | n/a | same | same |
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

In Lua: `export("name", fn)` (or `export("name")` for the global function of that name): Lua has no decorators, and a function is a value, so naming it at the call is the natural spelling. `export` is the worker's name, so a host cannot mount a function called `export`. A mounted function answers `nil, message` on a `FAIL`, so an export that wants the failure to be the host's `RAISE` says `error(err, 0)`.

```lua
export("total", function(items, rate)   -- a direct call: total({...}, 0.2)
  local sum = 0
  for _, i in ipairs(items) do sum = sum + i.price end
  return sum * (1 + rate)
end)

local seen = 0                           -- notify, then fetch: on_txn("123456")
export("on_txn", function(id)
  local d, err = txn_details(id)         -- a mounted host function, mid-invocation
  if not d then error(err, 0) end
  seen = seen + 1
  return string.format("%d %s %d", seen, d.kind, d.amount)
end)

for ev in next_event do handle(ev.name, ev.args) end   -- pull: no export at all
```

The names are fixed when the script ends; an exception in an export is `RAISE` with the language's own error text and the worker serves on; a `CANCEL` or deadline during an invocation ends the worker, as in a script. The grid below is generated from the tests.

## Host calls guest

The other direction (`tests/boundary.py run-invoke`, run by `tests/test_invoke.sh`): a script per case that exports functions, written in lua (the `x:` snippets in `containment/boundary.tsv`), run in this language's worker with a plan of invocations from the host in `tests/lib/invoke`. A type crosses as a `RETURN` value when the export returns it written in the language, and as an `INVOKE` argument when the export hands back what it was given. The second table holds the host-calls-guest safety rows by name: `held`, `stricter` (refused earlier than the row asks), `left out` (the language cannot express it), or `not held`, which fails the run. Nothing in the tests lists what a worker can do.

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
| `int-max` | ok | ok |
| `int-min` | ok | ok |
| `float` | ok | ok |
| `bool-true` | ok | ok |
| `bool-false` | ok | ok |
| `null` | ok | ok |
| `array` | error: raised: the value returned: a host function takes strings, numbers, ... | error: raised: the value returned: a host function takes strings, numbers, ... |
| `map` | error: raised: the value returned: a host function takes strings, numbers, ... | error: raised: the value returned: a host function takes strings, numbers, ... |
| `nested` | error: raised: the value returned: a host function takes strings, numbers, ... | error: raised: the value returned: a host function takes strings, numbers, ... |
| an export that calls a mounted host function (`relay`) | ok | |
| state between invocations (a counter, three calls) (`state`) | state carries | |
| an export that raises, then another call (`raise`) | RAISE: [string "local n = 0..."]:3: boom | |
| a cancel inside an export's loop (`loop`) | heard in a loop | |
| a cancel while an export waits on a host call (`wait`) | heard at the call | |
| a 100,000-byte `RETURN` (`big`) | refused, worker killed | |
| three arguments of different types (`args`) | ok | |
| a 100,000-byte `INVOKE` argument (`big-arg`) | refused: killed | |
| `INVOKE` of a name the guest did not export (`unexported`) | refused by the host, nothing sent; the guest serves on | |
| `END` (`end`) | DONE 0 | |
| a script that exports nothing (`none`) | DONE, as before exports | |
| notify, then fetch: two transactions, an unknown id (`txn`) | ok; an unknown id raises: no transaction 999999 | |

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

- **In-process transport.** a direct C call into the embedded Lua state (`dlopen`ed liblua); no transport.
- **Types.** in: str, int, strlist as globals; tables (array, map, nested) are built by a callback and pushed. out: str, int. The bridge also has bool, nil and double entry points that no spec exercises, so they are not claimed.
- **Strings carry their length: a NUL inside one crosses in-process, both ways.** Lua strings hold a NUL, and a C string cannot. The bridge used to read and write them as C strings, so a host that read a string with a NUL in it saw it cut at the NUL, and one it gave was cut the same way, silently (found by the shared boundary case `str-nul`, in-process). What the host reads (an argument, a result) is now returned as an Aether string made with `lua_pushlstring` and `lua_tolstring`, and what it gives arrives as an Aether string (`@aether string` in `module.ae`) and is pushed with its length, so the in-process grid above reads `ok` for it. A character beyond the BMP crosses too (`str-astral`). The contained child already carried both, as JSON.
- **Duplex.** host calls the guest (`vm_eval`); the guest calls the host while it runs (a registered callback); a callback is a plain call, so the guest waits. Nesting an `eval` from a callback is not claimed: no spec.
- **Callbacks.** registered by name, dotted names make the prefix table (`redis.call`); the host cannot receive a Lua function.
- **Errors and state.** `vm_eval` returns -1 and the error is the result (tag ERR); a callback's `raise_error` is a Lua error a script can catch; an instruction limit stops a loop; state is one Lua state per VM handle.
- **Contained child.** Contained child: `std.guest` over a `std.ipc` socketpair, 4-byte length-prefixed frames, JSON arguments and values, a granted file as a descriptor, guest-initiated calls answered one at a time, no nesting (the top-level README lists the common part). strings and integers cross as JSON (the stunt rows send no other kind); `print` is `SAY`; a `FAIL` raises a Lua error; `CANCEL` is looked for every 1000 instructions from a debug hook, and in every reply.
- **Tests.** `lua/test_host_lua_vm.ae`: "a host callback's int result and injected globals", "a typed string result", "a callback reads its string argument and the argument count", "a callback's raise_error is a Lua error the script can catch", "the instruction limit stops an endless loop", "array, map and nested tables from callbacks"; `lua/test_host_lua.ae`; the child: `tests/test_stunt_*.sh` rows `callback-greet`, `callback-add`, `binding-*` (`lua/containment/checks.lua`).

## Containment table

What a Lua script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Lua way
([containment/checks.lua](containment/checks.lua)), under the policies of
aether's `tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`).
"Per policy" means granted where the policy grants it and refused where it
does not, as stunt expects. The columns are the ways above: "in-process,
Linux (preload)" is way 3, "in-process, macOS" is way 2 where there is no
preload, and "contained child" is way 4. Every cell is a test outcome
(`../tests/run.sh`): checked on macOS arm64 (Lua 5.5.1 and 5.4.9) and on
CachyOS x86_64, kernel 7.3 with Landlock (Lua 5.5.1 and 5.4.9), and in CI.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`io.open`) | refused | **not held**: reads it | refused |
| `open-granted-file` (`io.open`) | per policy | **not held** | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`io.open(.., "w")`) | per policy | **not held** | refused under every policy |
| `getenv-home` (`os.getenv`) | per policy | **not held** | refused under every policy: the worker has no environment; `getenv` is a host function |
| `exec-bin-sh` (`os.execute`) | per policy | **not held** | refused under every policy: the worker's lockdown has no exec grant |
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
  On Linux the preload holds Lua's file and environment calls, and
  `os.execute` and `io.popen` too: they need the fork grant and exec of
  `/bin/sh` (aether #3580; glibc's `system()` starts `/bin/sh` with
  `posix_spawn`, whose `clone` and `execve` happen inside libc, and the
  preload holds that as well). On macOS nothing holds them at all. The host functions
  (`binding-*`), which check the grants themselves, hold everywhere.
- **The contained child holds every row it runs**, on Linux, and is
  stricter than the policy: the worker locks itself down with no grants
  before the script starts, so a script reaches a granted file only through
  the host (`read_file`, answered with a descriptor) and never gets a handle
  on libc. That includes process start: `os.execute` is refused under every
  policy, because the worker's own lockdown has no exec grant and the
  preload holds `system()` to it.
- The tests hold these cells, including the gaps: an **escapes** or **not
  held** row runs expecting the call to succeed, so the day the preload
  holds it that test fails and this table is brought up to date.

## Loading liblua

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

## In a contained child

`lua/worker/worker.ae` is a std.guest worker. It takes the liblua soname
as its one argument, since it is given no environment, and loads liblua
before it locks itself down. In the script, `print` is `SAY`, and each
function the host mounts is a Lua global of the same name whose call is a
`CALL` the host answers (a granted file comes back as
`{granted = true, text = ...}`).
[containment/child_host.ae](containment/child_host.ae) is a complete host.

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

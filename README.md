# aether-guest-languages

Other languages run from [Aether](https://github.com/aether-lang-dev/aether)
programs: the bridges that were `contrib/host/<lang>` in aether's tree, and
more, each held to the same named checks.

A language here can be run four ways, and the repository does not choose
for you. Every way stays supported; they differ in how much they contain
([lua/README.md](lua/README.md#four-ways-to-run-lua-from-aether),
[python/README.md](python/README.md#four-ways-to-run-python-from-aether),
[duktape/README.md](duktape/README.md#four-ways-to-run-javascript-from-aether),
[quickjs/README.md](quickjs/README.md#four-ways-to-run-javascript-from-aether),
[tcl/README.md](tcl/README.md#four-ways-to-run-tcl-from-aether),
[perl/README.md](perl/README.md#four-ways-to-run-perl-from-aether),
[ruby/README.md](ruby/README.md#four-ways-to-run-ruby-from-aether),
[tinygo/README.md](tinygo/README.md#four-ways-to-run-go-from-aether),
[go/README.md](go/README.md#four-ways-to-run-go-from-aether),
[java/README.md](java/README.md#four-ways-to-run-java-from-aether),
[factor/README.md](factor/README.md#four-ways-to-run-factor-from-aether),
[racket/README.md](racket/README.md#four-ways-to-run-racket-from-aether) and
[rexx/README.md](rexx/README.md#four-ways-to-run-rexx-from-aether)
each show all four, with examples. Go and Java as child processes are never
in your process, so their first three ways are a child process with less and
more around it. Three fourths do not work on Linux today, and each README
says why: a JVM, Factor's VM and Racket's Chez Scheme all map executable
memory, which the contained child's preload refuses):

1. **Embed.** The interpreter is loaded into the Aether program and called
   directly (`import lua`, then `lua.run(...)`, the script often a heredoc,
   or the two-way VM). The easy default: one binary, direct calls both
   ways, no containment.
2. **Embed inside a sandbox block.** The same, under a grant list
   (`lua.run_sandboxed(perms, ...)`): std's calls and the host functions
   you give the guest are checked against it. The interpreter's own libc
   calls are not.
3. **Embed under `LD_PRELOAD`.** Way 2 with `libaether_sandbox.so`
   preloaded (Linux), which holds the interpreter's own libc calls to the
   grants too. A library interposer, not a kernel boundary.
4. **Contained child.** The interpreter runs in a worker process that a
   host starts with `std.guest`'s `guest.launch` (`sandbox.spawn_socket`:
   Landlock and seccomp from the grant list, plus the preload). The worker
   locks itself down before the script's first step, and every effect the
   script has crosses a socket (`std.ipc` frames) the host answers. It
   needs Linux with Landlock; on macOS and Windows `guest.launch` starts
   nothing and says why.

Each language's README carries a **containment table**: for every row of
stunt's catalogue (aether `contrib/host/stunt`, the test double for a
hosted language), what that language's own way of doing it gets in each
mode: refused, allowed per the policy, not held, or left out because the
language cannot express it. The tests in `tests/` produce the table, so a
claim in it is a test outcome. That a language runs is not the question the
table answers; whether it is contained is.

Above it, a **Stunt parity** table gives one row per stunt test (the
`tests/test_stunt_*.sh` scripts, named after aether's) and one cell per way:
"matches", or the rows that differ and how. It is generated from a run
(`sh tests/run.sh --write-parity`), and every run checks it in the columns
its host can run, so a README that no longer says what the tests do fails
the tests.

Every language has a contained child except Racket (below), and Java and Factor,
which cannot start there today, and the two that are a child already. The contained child is the same conversation for all of them, because they share
`std.guest`; what follows is checked against `std/guest` and `std/ipc` in the
current release and against the workers' own runs:

- **Transport (`std.ipc`).** `guest.launch` starts the worker with
  `sandbox.spawn_socket`, which is `os.run_socket` plus the sandbox: an
  `AF_UNIX` socketpair at descriptor 3, both ways, descriptors passable. Frames
  are a 4-byte big-endian length and then that many bytes, at most 64 KiB
  (`MAX_FRAME`; the host's `g.max_frame` can be set, the worker reads at most
  64 KiB). A frame is a verb, a space and its text; arguments and values are
  JSON. A frame over the limit, a malformed one or one that stalls half way
  ends the guest (`KILLED frame refused`). A granted file travels as an open
  descriptor (`ipc.send_fd`), host to worker only, never a path; the host
  refuses a descriptor sent up.
- **Duplex.** While its script runs the guest starts every exchange. Up: `SAY`
  (what it prints), `LOG` and `AUDIT` with no answer, `CALL name [json array]`
  which the host must answer (`REPLY`, `FAIL` or `FILE`, through `answer`,
  `fail`, `grant_file`, `deny`), then `DONE` or `ERROR`. Down: the host speaks
  first (`MOUNT` for each host function, `PROBE`, `SCRIPT`) and afterwards only
  to answer a `CALL`, to send `CANCEL`, or `LOGLEVEL`; any other frame while the
  script runs breaks the protocol. One call is open at a time and a value is one
  frame (nothing streams).
- **The host calls the guest.** A script that exports functions by name ends
  with `READY ["name",...]` instead of `DONE` and then serves: only while it is
  idle may the host send `INVOKE name [json array]` (`guest.invoke`), which the
  guest answers with `RETURN value` or `RAISE why` (`guest.result` makes that
  Aether's `(value, err)`), calling mounted host functions on the way as a
  script does, and `END` (`guest.end`) finishes it with `DONE 0`; the two sides
  strictly alternate, nothing nests and there are no ids, and a worker that
  exports nothing behaves exactly as before. An invocation may carry its whole
  input as arguments, any JSON value up to the frame limit; a small
  notification the export answers by phoning home through a mounted function
  (`on_txn("123456")`, then `txn_details(id)`) is one suggested practice beside
  it, and the pull pattern, a guest looping on a mounted `next_event`, needs no
  verb at all (aether's `docs/design/guest-host-calls-guest.md` compares them).
- **Callbacks.** The host mounts functions by name with an arity
  (`guest.mount`); the worker makes each a name of its language, and a call's
  arguments and answer are JSON. An unmounted name is the language's own error.
  Nothing the guest makes (a closure, a handle) goes to the host: what the host
  can call later is only what the script exported, by name, fixed at `READY`.
  What a `FAIL` answer becomes in the script is the language's (below), and
  each language's README says how a script exports a function.

The table's in-process columns and its child columns both show what the shared
boundary tests found for each language, the same cases and the same words. In
process, `tests/boundary.py run-inproc` runs each case against the bridge in this
process through the API it really has (`<lang>/containment/boundary_host.ae`, an
Aether program written against that API; where a bridge can only run a script and
share a string map, the host uses that), and what the host saw is what its typed
readers made of the value, so "differs" says what was lost, "n/a" says the bridge
has no call that could express the case, and "refused" is a deliberate, documented
refusal. There is no wire:
the in-process transport is a direct call, and no cancel exists in-process (a
guest loop is stopped only by a limit the bridge counts itself, where it has one).
The results are Linux's; each language README's in-process grid also says where
macOS gave a different one. A table is something the host can build (where the
bridge has a builder) but not read, so a table the guest sends up reads as a tag.
The child columns: `tests/boundary.py`, run in each worker by `tests/test_boundary.sh`,
sends and receives every type, a callback with each, a call inside a call, an
error answer, a cancel in a loop and at a call, and frames past the limit, and
the language's README holds the full outcome grid. A cell is generated from that
grid, and a README that claims what the tests did not find fails the run.

A value counts as received when the script can hand the very value back to the host,
because the test cannot look inside the guest: "host to guest" in a grid is a value
the host answered with, which the script then sent up again. A worker that decodes a
table but cannot encode one reads "not array" there.

<!-- language-boundary:begin: the API column is hand-written; the in-process and child columns are generated from the shared boundary tests (tests/readme_tables.sh) -->
| Language | Way in | In-process: API (hand-written) | In-process: transport (generated) | In-process: types (generated) | In-process: duplex (generated) | In-process: callbacks (generated) | Child: transport (generated) | Child: types (generated) | Child: duplex (generated) | Child: callbacks (generated) | Child: host calls guest, types (generated) | Child: host calls guest (generated) | Detail |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Lua | `import lua` | `eval(str)` on a VM handle, or `run(str)`. In: str, int, strlist; tables (array, map, nested) come from callbacks. Out: str, int | direct call; macOS: the same | both ways: str, int, bool, null; not float, array, map, nested | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; a limit stops a loop | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, int, float, bool, null; not array, map, nested | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: returned as a value | both ways: str, int, float, bool, null; not array, map, nested | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error ([string "local n = 0..."]:3: boom), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: no transaction 999999; 9 of 9 host-calls-guest rows held | [lua](lua/README.md#what-crosses-the-boundary) |
| Python | `import python` | `eval(str)` on a VM handle, or `run(str)`. In: str, int, strlist. Out: str, int, float, bool, bytes (length-aware); a table result is a tag | direct call; macOS: the same | both ways: str, int, float, bool, null; not array, map, nested | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; a limit stops a loop | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, int, float, bool, null, array, map, nested | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: caught | both ways: str, int, float, bool, null, array, map, nested | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (ValueError: boom), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: RuntimeError: no transaction 999999; 9 of 9 host-calls-guest rows held | [python](python/README.md#what-crosses-the-boundary) |
| JavaScript, Duktape | `import duktape` | `eval(str)` on a VM handle, or `run(str)`. In: str, int, bool, nil, strlist. Out: str, int, bool | direct call; macOS: the same | both ways: str, int, bool, null; not float, array, map, nested | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; nothing stops a loop (no limit) | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, float, bool, null, array, map, nested, int (not 64-bit) | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard only at a call (a loop is killed after the grace period) | by name; argument and return types as in types; a `FAIL` answer: caught | both ways: str, float, bool, null, array, map, nested, int (not 64-bit) | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (Error: boom), and the guest serves on; cancel: not heard in an export's loop: killed after the grace period; notify then fetch: ok; an unknown id raises: Error: no transaction 999999; 9 of 9 host-calls-guest rows held | [duktape](duktape/README.md#what-crosses-the-boundary) |
| JavaScript, QuickJS | `import quickjs` | `eval(str)` on a VM handle, or `run(str)`. In: str, int, strlist, any JSON value. Out: str, int, float, bool, bytes (a `Uint8Array`, length-aware), nil; an array or object as a tag and as JSON; time limit and a cancel cell any thread may set; one runtime per handle | direct call; macOS: the same | both ways: str, float, bool, null, array, map, nested, int (not 64-bit) | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; a limit stops a loop | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, float, bool, null, array, map, nested, int (not 64-bit) | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: caught | both ways: str, float, bool, null, array, map, nested, int (not 64-bit) | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (Error: boom), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: Error: no transaction 999999; 9 of 9 host-calls-guest rows held | [quickjs](quickjs/README.md#what-crosses-the-boundary) |
| Tcl | `import tcl` | `eval(str)` on a VM handle, or `run(str)`; everything is a string underneath. In: str, int, strlist. Out: str, int, double, bool | direct call; macOS: the same | guest to host: str (not empty); not int, float, bool, null, array, map, nested<br>host to guest, passed back: int, float, null, str (not empty); not bool, array, map, nested | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; a limit stops a loop | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, int, float; not bool, null, array, map, nested | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: caught | both ways: str, int, float; not bool, null, array, map, nested | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (boom), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: no transaction 999999; 9 of 9 host-calls-guest rows held | [tcl](tcl/README.md#what-crosses-the-boundary) |
| Perl | `import perl` | `run(str)` only, 0 or -1: no value comes back. str and int cross both ways through callbacks and the shared map | direct call; macOS: the same | both ways: str; not int, float, bool, null, array, map, nested | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; nothing stops a loop (no limit) | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | guest to host: str, int, float, null, array, map, nested; not bool<br>host to guest, passed back: str, int, float, bool, null, array, map, nested | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: caught | as a `RETURN` value: str, int, float, null, array, map, nested; not bool<br>as an `INVOKE` argument, returned: str, int, float, bool, null, array, map, nested | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (boom), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: no transaction 999999; 9 of 9 host-calls-guest rows held | [perl](perl/README.md#what-crosses-the-boundary) |
| Ruby | `import ruby` | `eval(str)`, the result kept as its `to_s`; or `run(str)`. In and out: str only, as text | direct call; macOS: the same | both ways: str, int, float, bool, null; not array, map, nested | the host calls the guest and the guest calls back mid-eval, one thread; calls nest in an expression; no cancel in-process; a limit stops a loop | by name; argument and return types as in types; an error raised by the host function: caught | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, int, float, bool, null, array, map, nested | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: caught | both ways: str, int, float, bool, null, array, map, nested | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (ArgumentError: boom), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: RuntimeError: no transaction 999999; 9 of 9 host-calls-guest rows held | [ruby](ruby/README.md#what-crosses-the-boundary) |
| Go, as a shared library | `import tinygo` | `load(path)` then `call_<ret>_<args>(symbol, ...)`: int, long, double, str, ptr, and owned-string results; any other signature through libffi when built with it. No tables, no structures | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | a `std.guest` worker that loads a Go library; the script is lines `call SYMBOL a<TAB>b` | str only: two strings in, one string out; no JSON | the script's lines run in order; the host cancels (checked by the `spin` row) | none: the worker mounts no host function | none: the worker mounts no host function and its script is call lines, with nothing to export | none: the worker mounts no host function and its script is call lines, with nothing to export | [tinygo](tinygo/README.md#what-crosses-the-boundary) |
| Go, as a child process | `import go` | `run(binary)`, `run_script(path)`: starts a program; out: its exit status. Env and the shared map (`..._with_map`) are the only values | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | the worker is a Go program: its script is `call NAME [JSON]` lines, values are the JSON text; a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | both ways: str, int, float, bool, null, array, map, nested | guest starts every exchange, host answers or cancels; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: reported as text | none: the worker's script is `call NAME [JSON]` lines, with nothing to export (go/README.md) | none: the worker's script is `call NAME [JSON]` lines, with nothing to export (go/README.md) | [go](go/README.md#what-crosses-the-boundary) |
| Java, as a JVM child | `import java` | `run(classpath, main)`: starts a JVM; out: its exit status; the shared map through `AETHER_MAP_SHM` | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | does not start: the preload refuses the JVM's executable memory | does not start: the preload refuses the JVM's executable memory | does not start: the preload refuses the JVM's executable memory | does not start: the preload refuses the JVM's executable memory | does not start: the preload refuses the JVM's executable memory | does not start: the preload refuses the JVM's executable memory | [java](java/README.md#what-crosses-the-boundary) |
| Factor (experimental fork) | `import factor` | `eval(str)` returns what the script printed; `run(str)`; `set` and `get` a string key in the VM's global namespace; shared map by FFI words | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | does not start: the preload refuses Factor's executable memory | does not start: the preload refuses Factor's executable memory | does not start: the preload refuses Factor's executable memory | does not start: the preload refuses Factor's executable memory | does not start: the preload refuses Factor's executable memory | does not start: the preload refuses Factor's executable memory | [factor](factor/README.md#what-crosses-the-boundary) |
| Racket CS (and Rhombus) | `import racket`, `import rhombus` | `evaluate(str)` returns what was printed; `run(str)`; `set` and `get` string keys. Rhombus: the same calls on the same VM, module body in | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | not run: no in-process boundary host yet | does not start: Chez Scheme cannot run in the child | does not start: Chez Scheme cannot run in the child | does not start: Chez Scheme cannot run in the child | does not start: Chez Scheme cannot run in the child | does not start: Chez Scheme cannot run in the child | does not start: Chez Scheme cannot run in the child | [racket](racket/README.md#what-crosses-the-boundary) |
| Rexx, RexxAE (an engine written in Aether, from its own repository) | `import rexx`, over RexxAE's binary library (`import rexxae`) | `run(str)` returns 0, the `EXIT` code or -1; a fresh context per script with a 5 s limit. On RexxAE's API, values cross as JSON both ways: num, str, bool, null, arrays, objects | direct call; macOS: the same | both ways: str (not NUL); not int, float, bool, null, array, map, nested | no host function: the script and the host exchange strings through the shared map; no cancel in-process; a limit stops a loop | none: the host registers no function | a 60 KB str crosses; a 100 KB argument: refused, worker killed; a 100 KB answer: refused: killed | guest to host: float, str (not NUL), int (not 64-bit); not bool, null, array, map, nested<br>host to guest, passed back: str, float, bool, null, array, map, nested, int (not 64-bit) | guest starts every exchange while its script runs, host answers or cancels; once the script has ended with exports, the host invokes them one at a time; calls nest in an expression; cancel heard in a loop and at a call | by name; argument and return types as in types; a `FAIL` answer: ends the script | as a `RETURN` value: float, str (not NUL), int (not 64-bit); not bool, null, array, map, nested<br>as an `INVOKE` argument, returned: str, float, bool, null, array, map, nested, int (not 64-bit) | exports by name; an export calls mounted host functions; an error: `RAISE` with the language's error (division by zero), and the guest serves on; cancel: heard in an export's loop; notify then fetch: ok; an unknown id raises: no transaction 999999; 8 of 8 host-calls-guest rows held, 1 left out | [rexx](rexx/README.md#what-crosses-the-boundary) |
<!-- language-boundary:end -->

A guest that is written in Aether and lives in another repository (its engine
is linked into the host) takes a row in each table like the rest: how it runs is
"Aether engine linked into the host", its runtime comes from that repository at
a pinned commit, and a way of running that does not apply is written "does not
apply" with the reason. The generated columns find a row by the link in its
first cell, so adding one needs no change to the tests.

<!-- language-facts:begin: columns marked (generated) are written by sh tests/run.sh --write-parity (tests/readme_tables.sh) from .github/workflows/ci.yml and the parity tables; the rest is hand-written -->
| Language | How it runs | Runtime comes from | CI today (generated) | Checked by hand | Status | Resource limits | Containment (generated) |
|---|---|---|---|---|---|---|---|
| [Lua](lua/README.md#four-ways-to-run-lua-from-aether) | embedded in the host process | system liblua, found at run time | Ubuntu 24.04: Lua 5.4 + child<br>macOS 15: Lua 5.5, 5.4 | 5.3, 5.5 (README, undated) | stable | instruction limit | macOS: 8 of 10 tests have a row not held; Linux preload: every row held; child: every row held |
| [CPython](python/README.md#four-ways-to-run-python-from-aether) | embedded in the host process | system libpython, found at run time (free-threaded refused) | Ubuntu 24.04: Python 3.12 + child<br>macOS 15: Python 3.13 | 3.10 to 3.14 (README, undated); cachyos 3.14, 2026-11-21 | stable | instruction limit | macOS: 9 of 10 tests have a row not held; Linux preload: 1 of 10 tests have a row not held; child: every row held |
| [Duktape](duktape/README.md#four-ways-to-run-javascript-from-aether) | embedded in the host process | system libduktape, found at run time | Ubuntu 24.04: Duktape 2.7 + child<br>macOS 15: Duktape 2.7 | none beyond CI | stable | none (reported unavailable) | macOS: every row held; Linux preload: every row held; child: every row held |
| [QuickJS](quickjs/README.md#four-ways-to-run-javascript-from-aether) | engine compiled into your program | Aether's `contrib.quickjs`: quickjs-ng 0.18.0 as the ae release ships it (Array.find fix patched in) | Ubuntu 24.04: quickjs-ng 0.18 + child<br>macOS 15: quickjs-ng 0.18 | none beyond CI | stable | 5 s deadline per script (in-process); none inside the worker (the host kills it) | macOS: every row held; Linux preload: every row held; child: every row held |
| [Tcl](tcl/README.md#four-ways-to-run-tcl-from-aether) | embedded in the host process | system libtcl, found at run time; one build serves 8.5, 8.6, 9.0 and 9.1 | Ubuntu 24.04: Tcl 8.6 + child<br>macOS 15: Tcl 8.6 (Homebrew tcl-tk@8)<br>macOS 15: Tcl 9.1 (Homebrew tcl-tk) | 8.5, 8.6 (README, undated); 9.0.4 with zipfs (the default) and without, private builds, in-process and as a child, cachyos, 2026-11-21; 9.1.0 (Homebrew, library in files), macOS CI, 2026-11-21 | stable | command limit, time limit | macOS: 9 of 10 tests have a row not held; Linux preload: 1 of 10 tests have a row not held; child: every row held |
| [Perl](perl/README.md#four-ways-to-run-perl-from-aether) | embedded in the host process | system libperl (threaded), found at run time | Ubuntu 24.04: Perl 5.38 + child<br>macOS 15: Perl 5.34 (the system's) | 5.34 to 5.44 (README, undated) | stable | none | macOS: 9 of 10 tests have a row not held; Linux preload: every row held; child: every row held |
| [CRuby](ruby/README.md#four-ways-to-run-ruby-from-aether) | embedded in the host process | system libruby, found at run time | Ubuntu 24.04: Ruby 3.2 + child<br>macOS 15: Ruby 4.0 (Homebrew) | 2.6 to 3.4 (README, undated); 4.0.10, a private build on cachyos, 2026-11-21 (CI runs Homebrew's 4.0.9 on macOS) | stable | line and call budget | macOS: 9 of 10 tests have a row not held; Linux preload: every row held; child: every row held |
| [Go, shared library](tinygo/README.md#four-ways-to-run-go-from-aether) | compiled shared library loaded by the host | your Go library (`go build -buildmode=c-shared`); the bridge needs no Go | Ubuntu 24.04: Go (the runner's) + child<br>macOS 15: Go (the runner's) | Go 1.22, 1.27 (README, undated) | stable; the grant checks are the preload's and the child's only | none | macOS: 9 of 10 tests have a row not held; Linux preload: 9 of 10 tests have a row not held; child: 1 of 9 tests have a row not held |
| [Go, child process](go/README.md#four-ways-to-run-go-from-aether) | separate runtime process | the `go` command or a built binary | Ubuntu 24.04: Go 1.22 + child<br>macOS 15: Go (the runner's) | 1.22, 1.27 (README, undated) | stable | the child's deadline and kill (contained child) | macOS: 9 of 10 tests have a row not held; Linux preload: 9 of 10 tests have a row not held; child: 1 of 9 tests have a row not held |
| [Java, JVM child](java/README.md#four-ways-to-run-java-from-aether) | separate runtime process | the `java` command, JDK 21 or later | Ubuntu 24.04: OpenJDK 21 + child<br>macOS 15: JDK 21 (the runner's) | 21, 25 (README, undated) | stable; the JVM does not start in the preload on Linux | the child's deadline and kill | macOS: 9 of 10 tests have a row not held; Linux preload: runtime does not start; child: runtime does not start |
| [Factor](factor/README.md#four-ways-to-run-factor-from-aether) | embedded in the host process | a fork of Factor with an embedding API, built from `aether-lang-dev/factor-language` at a pinned commit | Ubuntu 24.04: Factor embed-api fork + child<br>macOS 15: none (compile check only) | none beyond CI | experimental: a fork, and its child cannot start | none | macOS: not run; Linux preload: 1 of 10 tests have a row not held; child: runtime does not start |
| [Racket CS, Rhombus](racket/README.md#four-ways-to-run-racket-from-aether) | embedded in the host process | a Racket CS installation (Linux: `libracketcs.so` made from the distribution's `.a`) | Ubuntu 24.04 arm64: Racket 8.10 (apt)<br>macOS 15: Racket 9.3 (cask), with Rhombus | 8.10, 9.3 (README, undated) | stable in-process; no contained child | none | macOS: 9 of 10 tests have a row not held; Linux preload: every row held; child: not run |
| [Rexx, RexxAE](rexx/README.md#four-ways-to-run-rexx-from-aether) | Aether engine linked into the host (RexxAE's binary library); in the child, RexxAE's worker | RexxAE (`aether-lang-dev/RexxAE`, its own repository, no release tags), built from source at the commit `REXXAE_REF` pins | Ubuntu 24.04: RexxAE at REXXAE_REF + child<br>macOS 15: RexxAE at REXXAE_REF | none beyond CI | a working slice of Rexx (RexxAE: 71 RexxJS builtins, 8 extensions); no I/O of its own, by design | 5 s per script (in-process); instruction budget per step; a builtin's result capped at 1 MiB; in the child, `CANCEL` every 1000 instructions | macOS: every row held; Linux preload: every row held; child: every row held |
<!-- language-facts:end -->

## Layout

```
aether.toml            the package: exports every language directory (modules = "lua, python, duktape, tcl, perl, ruby, quickjs, tinygo, go, java, factor, racket, rhombus, rexx")
ci/versions.env        toolchain pins (AE_PIN, AEB_REF, AETHER_STUNT_REF),
                       and the commits of outside engines (FACTOR_FORK_REF, REXXAE_REF)
lua/
  module.ae            import lua: the bridge's API
  aether_host_lua.c    the bridge, compiled into the importing program (@source)
  lua_abi.h            the Lua types the bridge uses (no Lua dev package needed)
  test_host_lua*.ae    the bridge's specs
  worker/              lua-worker: Lua in a contained child (std.guest)
  containment/         the containment hosts and checks.lua, stunt's rows in Lua;
                       lang.sh, what the harness needs to know about Lua
  PROVENANCE.md        where the bridge came from, and what the move changed
python/
  module.ae            import python: the bridge's API
  aether_host_python.c the bridge, compiled into the importing program (@source)
  python_abi.h         the CPython types the bridge uses (no Python dev package needed)
  test_host_python*.ae the bridge's specs (test_support.c: their C helpers)
  worker/              python-worker: CPython in a contained child (std.guest)
  containment/         the containment hosts and checks.py, stunt's rows in Python;
                       lang.sh; test_imports.sh, importing under a sandbox
  PROVENANCE.md        where the bridge came from, and what the move changed
duktape/
  module.ae            import duktape: the bridge's API
  aether_host_duktape.c the bridge, compiled into the importing program (@source)
  duktape_abi.h        the Duktape types the bridge uses (no Duktape dev package needed)
  test_host_duktape*.ae the bridge's specs
  examples/            the programs in README.md's four ways
  worker/              duktape-worker: Duktape in a contained child (std.guest)
  containment/         the containment hosts and checks.js, stunt's rows in JavaScript;
                       lang.sh; most rows are left out, with why: Duktape has no I/O
  PROVENANCE.md        where the bridge came from, and what the move changed
quickjs/
  module.ae            import quickjs: the bridge's API
  aether_host_quickjs.c the bridge, compiled into the importing program (@source);
                       the engine is aether's contrib.quickjs, not a copy
  test_host_quickjs.ae, test_quickjs_bridge.ae  the bridge's specs
  examples/            the programs in README.md's four ways
  worker/              quickjs-worker: QuickJS in a contained child (std.guest)
  containment/         the containment hosts and checks.js, stunt's rows in JavaScript;
                       lang.sh; most rows are left out, with why: no I/O of its own
  PROVENANCE.md        where the bridge came from, and what the move changed
tcl/
  module.ae            import tcl: the bridge's API
  aether_host_tcl.c    the bridge, compiled into the importing program (@source)
  tcl_abi.h            the Tcl types the bridge uses (no Tcl dev package needed)
  test_host_tcl*.ae    the bridge's specs (test_support.c: their C helpers)
  worker/              tcl-worker: Tcl in a contained child (std.guest)
  containment/         the containment hosts and checks.tcl, stunt's rows in Tcl;
                       lang.sh, what the harness needs to know about Tcl
perl/
  module.ae            import perl: the bridge's API
  aether_host_perl.c   the bridge, compiled into the importing program (@source)
  perl_abi.h           the Perl types the bridge uses (no perl dev headers needed)
  test_host_perl*.ae   the bridge's specs (test_support.c: their C helpers)
  worker/              perl-worker: Perl in a contained child (std.guest)
  containment/         the containment hosts and checks.pl, stunt's rows in Perl;
                       lang.sh; test_imports.sh, loading modules under a sandbox
ruby/
  module.ae            import ruby: the bridge's API
  aether_host_ruby.c   the bridge, compiled into the importing program (@source)
  ruby_abi.h           the Ruby types the bridge uses (no Ruby dev package needed)
  test_host_ruby.ae    the bridge's spec (test_support.c: its C helpers)
  worker/              ruby-worker: CRuby in a contained child (std.guest)
  containment/         the containment hosts and checks.rb, stunt's rows in Ruby;
                       lang.sh; test_requires.sh, requiring under a sandbox
  PROVENANCE.md        where the bridge came from, and what the move changed
tinygo/
  module.ae            import tinygo: the bridge's API (the name is kept from aether's
                       contrib.host.tinygo; the libraries come from standard go)
  aether_host_tinygo.c the bridge, compiled into the importing program (@source)
  test_host_tinygo*.ae the bridge's specs (test_support/: a C stand-in for a Go library)
  examples/            the Go libraries and the programs in README.md's four ways
  worker/              tinygo-worker: a Go c-shared library in a contained child (std.guest)
  containment/         the containment hosts and checks.go, stunt's rows in Go, built
                       into a c-shared library; lang.sh; test_threads.sh, goroutines and
                       thread-local grants; test_examples.sh
go/
  module.ae            import go: the bridge's API (starts a Go program as a child)
  aether_host_go.c     the bridge, compiled into the importing program (@source)
  test_host_go.ae      the bridge's spec
  go.mod               one Go module: rows, guestchild, worker, containment, examples
  rows/                stunt's rows in Go (cgo for the libc ones), shared by the programs below
  guestchild/          std.guest's worker protocol, in Go
  worker/              go-worker: Go in a contained child (std.guest)
  containment/         the containment hosts and checks.go, stunt's rows in Go;
                       lang.sh; test_ways.sh, what the four ways are for a child process
  examples/            the programs in README.md's four ways
  PROVENANCE.md        where the bridge came from, and what the move changed
java/
  module.ae            import java: the bridge's API (grant_jvm_runtime, and java.run* to start a JVM)
  aether_host_java.c   the bridge, compiled into the importing program (@source): go's, for java
  build.sh             javac and jar: the agent jar and the classes below (JDK 21 or later)
  src/                 the agent (-javaagent): grant checker, hooks, shared map, Native (libc through FFM)
  rows/                stunt's rows in Java, shared by the programs below
  guestchild/          std.guest's worker protocol, in Java
  worker/              java-worker: Java in a contained child (std.guest)
  containment/         the containment hosts and Checks.java; lang.sh; test_ways.sh, the four ways
                       and what libc sees (libghltrace.c)
  examples/            the programs in README.md's four ways
factor/
  module.ae            import factor: the bridge's API
  aether_host_factor.c the bridge, compiled into the importing program (@source)
  factor_abi.h         the function-pointer types the bridge calls libfactor through
  test_host_factor.ae  the bridge's spec (test_support.c: its C helper)
  examples/            the programs in README.md's four ways
  worker/              factor-worker: Factor in a contained child (std.guest)
  containment/         the containment hosts and checks.factor, stunt's rows in Factor;
                       lang.sh; preload.factor, the vocabularies loaded before the lockdown
  PROVENANCE.md        where the bridge came from, and what the move changed
racket/
  module.ae            import racket: the bridge's API
  aether_host_racket.c the bridge, compiled into the importing program (@source)
  racket_abi.h         the Chez Scheme / Racket CS types and macros the bridge uses (no Racket headers needed)
  test_host_racket.ae  the bridge's spec
  examples/            the programs in README.md's four ways (way 4 compiles, and cannot run)
  worker/              racket-worker: written, and cannot run (Chez needs anonymous executable memory)
  containment/         the containment hosts and checks.rkt, stunt's rows in Racket;
                       lang.sh; spin and env-cached are left out, with why
  PROVENANCE.md        where the bridge came from, and what the move changed
rhombus/
  module.ae            import rhombus: Rhombus on racket's VM (the bridge's rhombus_* points)
  test_host_rhombus.ae the spec (only where the Racket has the rhombus collection)
rexx/
  module.ae            import rexx: run, run_sandboxed, run_sandboxed_with_map, over RexxAE's
                       binary library (import rexxae); RexxAE itself is another repository,
                       taken at REXXAE_REF and never copied (rexx/README.md, PROVENANCE.md)
  test_host_rexx.ae    the bridge's spec, and the boundary on RexxAE's API
  containment/         the containment hosts and checks.rexx, stunt's rows in Rexx;
                       lang.sh; test_surface.sh and surface.ae, RexxAE's whole surface
                       held to reaching nothing; the worker is RexxAE's own (no worker/ here)
  PROVENANCE.md        where each file came from
tests/
  run.sh               everything, every language (GUEST_LANG=<lang> for one)
  stunt_lib.sh         holds a language to stunt's catalogue, by name
  test_stunt_*.sh      one per policy: aether's contrib_host_stunt lists
  test_catalogue.sh    every catalogue row is named here or left out
  parity.sh            renders and checks a README's Stunt parity table
  readme_tables.sh     checks (or writes) the generated columns of the language-facts table: CI today, from ci.yml, and Containment, from the parity tables
  factor_env.sh        find the embed-api fork's libfactor and image (the fork's checkout)
  <lang>_env.sh        find the language's runtime library on this machine (lua, python,
  <lang>_env.sh        find the language's runtime library on this machine (racket's also makes
                       build/libracketcs.so from libracketcs.a on Linux) (lua, python,
                       duktape, tcl, perl, ruby; quickjs: checks ae ships its engine)
                       duktape, tcl, perl, ruby), or for tinygo the go toolchain, which
                       builds the c-shared libraries the tests load
                       duktape, tcl, perl, ruby), or for go the toolchain and cgo;
                       java_env.sh finds a JDK 21 or later (a JVM is started, not loaded);
                       rexx_env.sh fetches RexxAE at REXXAE_REF and builds its library and worker
```

## Using a language from this repository

The repository is an Aether package. In your project:

```sh
ae add github.com/aether-lang-dev/aether-guest-languages
```

fetches the default branch and records it in `aether.toml` with the hash of
the tree it installed (there are no tagged releases yet):

```toml
[dependencies]
"github.com/aether-lang-dev/aether-guest-languages" = "latest"

[dependency-hashes]
"github.com/aether-lang-dev/aether-guest-languages" = "sha256-..."
```

Then `import lua` resolves to `lua/module.ae`, and likewise `import python`,
`import duktape`, `import quickjs`, `import tcl`, `import perl` and
`import ruby` to their directories' `module.ae`:
`import duktape`, `import tcl`, `import perl`,
`import ruby` and `import tinygo` to their directories' `module.ae`:
`import duktape`, `import tcl`, `import perl` and
`import ruby` and `import go` to their directories' `module.ae` (and `import java`):
`ae build`, `ae run` and `ae lib-path` put the package on the module search
path, and the bridge's C is compiled into your program from the `@source`
line in `module.ae`, with nothing to build beforehand.

Against a local checkout instead (developing a bridge, or CI proving a
branch):

```toml
[patch]
"github.com/aether-lang-dev/aether-guest-languages" = "../aether-guest-languages"
```

`ae build` then prints `Overriding ... -> ../aether-guest-languages`. For a
one-off without a manifest, `ae build app.ae --lib <checkout>` does the
same, since the checkout's root holds each language's `<lang>/module.ae`.

`import rexx` needs one more thing, because Rexx's engine is another
repository: RexxAE's binary library, built from a RexxAE checkout (`make
aether-lib`) and passed with `--lib <its build/aether>` (rexx/README.md,
"Where RexxAE comes from").

At run time the bridge `dlopen`s the language's library (all but QuickJS,
whose engine is compiled into the program: nothing to find, nothing to set). Each bridge tries
the usual sonames for its platform first (its README lists them), and an
environment variable names an exact soname or path when that is not enough:
`AETHER_LUA_SONAME` (for example `liblua5.4.so.0`, or
`/opt/homebrew/lib/liblua.dylib`), `AETHER_PYTHON_SONAME` (for example
`libpython3.12.so.1.0`), `AETHER_DUKTAPE_SONAME` (`libduktape.so.207`, or
Homebrew's `/opt/homebrew/lib/libduktape.so`), `AETHER_TCL_SONAME`
(`libtcl8.6.so.0`), `AETHER_PERL_SONAME` (for example
`/usr/lib/x86_64-linux-gnu/libperl.so.5.38`) and `AETHER_RUBY_SONAME`
(for example `/usr/lib/libruby.so.3.4.12`). Factor takes two: `AETHER_FACTOR_SONAME`
names the embed-api fork's libfactor (Factor's makefile calls the shared
engine `libfactor.so`, or on Linux `libfactor.a`, an ELF shared object that
`dlopen` takes by path) and `AETHER_FACTOR_IMAGE` the `factor.image`
bootstrapped for it, which the VM needs to start; a distribution's Factor has
no `factor_embed_eval` and is refused at load (factor/README.md). Racket
takes `AETHER_RACKET_SONAME`
(`Racket.framework/Versions/9.3_CS/Racket` on macOS; on Linux a
`libracketcs.so` you make from the distribution's `libracketcs.a`, which
Racket CS does not ship as a shared library) with `AETHER_RACKET_BOOT_DIR`,
`AETHER_RACKET_COLLECTS` and `AETHER_RACKET_CONFIG` for the boot images and
the module library, all found beside the library when not given
(racket/README.md, "Finding Racket"). `tests/<lang>_env.sh` is one way
to find each. No language's dev package is needed to build. QuickJS is
compiled in from aether's `contrib.quickjs`, so there is nothing to find.
The two Go bridges are the exceptions to the pattern: `tinygo.load` takes
the path of a library you built with `go build -buildmode=c-shared` (the
bridge needs no Go toolchain, only the library's author does), and `go`
loads no library at all: `GO_BIN` names the `go` command for
`go.run_script`, and a built Go binary needs nothing. `java` loads none either:
`JAVA_BIN` names the `java` command, `AETHER_JAVA_FLAGS` its flags
(`--enable-native-access=ALL-UNNAMED`, and `--enable-preview` on JDK 21) and
`AETHER_JAVA_AGENT` the agent jar that sandboxed runs add; `JAVA_HOME` is only
how the tests find a JDK.

## Toolchain

`ae` (and `aeb`, which nothing here needs yet) from aeb's `get.sh`, pinned
as `ci/versions.env` says. One line installs both into `~/.local` (no sudo;
`PREFIX=` to change it):

```sh
curl -fsSL https://raw.githubusercontent.com/aether-lang-dev/aeb/v0.340/get.sh \
  | AE_PIN=0.951.0 AEB_REF=v0.340 sh
```

That installs everything the bridges, the in-process tests and the contained
child need (the child is `std.guest`, part of that release). `AE_PIN` is the
current Aether release, and the only one this repository is tested on.

## Tests

```sh
sh tests/run.sh                    # every language: specs, containment rows, parity check
sh tests/run.sh containment        # the rows only
GUEST_LANG=python sh tests/run.sh  # one language
GUEST_LANG=duktape sh tests/run.sh
GUEST_LANG=tcl TCL_VERSION=8.6 sh tests/run.sh
RUBY=/opt/homebrew/opt/ruby/bin/ruby GUEST_LANG=ruby sh tests/run.sh
LUA_VERSION=5.4 GUEST_LANG=lua sh tests/run.sh
PYTHON=/usr/bin/python3.12 GUEST_LANG=python sh tests/run.sh
GUEST_LANG=perl sh tests/run.sh    # PERL=/path/to/perl to pick the perl
GUEST_LANG=tinygo sh tests/run.sh  # GO=/path/to/go to pick the go
GUEST_LANG=go sh tests/run.sh      # GO=/path/to/go to pick the go command
GUEST_LANG=java sh tests/run.sh    # JAVA_HOME=/path/to/jdk to pick the JDK (21 or later)
GUEST_LANG=factor sh tests/run.sh  # FACTOR_HOME=<the fork's checkout>; skips without it
GUEST_LANG=racket sh tests/run.sh  # RACKET_HOME=/path/to/racket to pick the installation
GUEST_LANG=rexx sh tests/run.sh    # REXXAE_SRC=/path/to/RexxAE instead of the pinned commit
sh tests/run.sh --write-parity     # rewrite the parity tables' columns this host ran
```

`tests/run.sh` also runs the shared boundary tests in each contained child
(`tests/test_boundary.sh`, `tests/boundary.py`; each language's snippets are
`<lang>/containment/boundary.tsv`) and checks the boundary grid in its README.

The containment rows build stunt from aether's source at
`AETHER_STUNT_REF` (three files, fetched once into `build/`; or set
`AETHER_SRC` to an aether checkout) to read its catalogue: release archives
ship `contrib/host/stunt` without `stunt.c` and `stunt_main.c`. On Linux
the in-process rows run under the release's `libaether_sandbox.so`; the
child rows run where the kernel has Landlock
(`GHL_REQUIRE_CHILD=1` makes a missing child mode a failure, as CI does).

CI (`.github/workflows/ci.yml`), two jobs per language, both on `AE_PIN`: Linux
(Ubuntu 24.04: in-process and the contained child, required; Racket's child is
probed and skipped) and macOS (in-process). Linux installs only each language's
runtime package, never a dev package: liblua5.4-dev (Lua), libpython3.12,
libduktape207, tcl8.6, perl and libperl5.38t64, ruby and libruby3.2, golang-go
(Go 1.22; gcc is the runner's), QuickJS needs nothing installed. macOS uses
Homebrew's lua (5.5) and lua@5.4, python@3.13, duktape, tcl-tk and ruby, the
system perl (5.34), and the runner's Go (`brew install go` if it has none).
Java needs only a JDK, openjdk-21-jdk-headless on Ubuntu 24.04 and a JDK 21
from the runner or `brew install openjdk@21` on macOS; its Linux job passes
with the JVM blocked by the preload, which the tests check (java/README.md).

Factor is the exception to "install the runtime package": a distribution's
Factor has no embedding API, so its Linux jobs build the embed-api fork at
`FACTOR_FORK_REF` and bootstrap its image (cached under the commit; the slow
part is the bootstrap), and its macOS job only compiles the bridge, hosts and
worker and checks that the specs and rows skip cleanly. Its Linux job runs the
contained child with `GHL_REQUIRE_CHILD=1` like the others.

Racket: Ubuntu's `racket` package (8.10, which
brings `libracketcs.a`; the jobs make `libracketcs.so` from it) and, on macOS,
`brew install --cask racket` (9.3) with `raco pkg install rhombus`; the
contained child is never required for it, since it cannot run.
Rexx needs nothing installed: `tests/rexx_env.sh` fetches RexxAE at
`REXXAE_REF` and builds it with the job's ae. It has two jobs, Linux
(in-process and the contained child) and macOS, both on `AE_PIN`.

## Moving from `contrib.host.<lang>`

The Lua bridge was copied from aether at `d3732a0ed` (lua/PROVENANCE.md).
Its functions and names are unchanged, so moving is the import line and the
dependency:

```diff
-import contrib.host.lua
+import lua
```

plus the dependency (`ae add`, above). What changes for the better: no
`make contrib MODULES=lua` / `make install-contrib` step and no Lua dev
package at build time (a release install of ae could not link
`contrib.host.lua` without them), and Lua 5.5 works (aether's copy refuses
to load it: it looks for `luaL_openlibs`, which 5.5 does not export).

The Python bridge was copied from aether at `9673641e6`
(python/PROVENANCE.md), and moves the same way, the import line and the
dependency:

```diff
-import contrib.host.python
+import python
```

What changes: no `make contrib MODULES=python` step and no python3-dev at
build time; the `_aether_map` module no longer depends on the build host's
Python.h; a free-threaded CPython is refused with a message rather than
loaded.

The Duktape bridge was copied from aether at `2609ec841`
(duktape/PROVENANCE.md), and moves
the same way:

```diff
-import contrib.host.duktape
+import duktape
```

(aether's `contrib.host.js`, a thin alias of it, is not carried over: `import js` does not exist here.) What changes: no
`make contrib MODULES=duktape` step and no libduktape-dev at build time, and
libduktape is found under its usual names without `AETHER_DUKTAPE_SONAME`.
Duktape has no I/O of its own, so what it can do in a sandbox is what the
bridge's `env`, `readFile`, `writeFile` and `exec` are allowed: they hold on
macOS as on Linux, with no preload (duktape/README.md).
The QuickJS bridge was copied from aether at `2609ec841`
(quickjs/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.quickjs
+import quickjs
```

What changes: no `make contrib` step; and a script now has `print` and
`console.log` (aether's could only compute). The engine is not copied:
the bridge imports aether's `contrib.quickjs`, which every release ships with
its pinned quickjs-ng 0.18, so there is still one copy of QuickJS and one
pin. QuickJS has no I/O of its own (`quickjs-libc` is not compiled in), so
most of stunt's rows are left out and nothing in it needs the preload
(quickjs/README.md).

The Tcl bridge was copied from aether at `2609ec841`
(tcl/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.tcl
+import tcl
```

What changes: no `make contrib MODULES=tcl` step and no tcl-dev at build
time; one build runs against Tcl 8.5, 8.6 and 9.0 whatever `tcl.h` the build
host had; libtcl is found by name on Debian, Homebrew and macOS without
`AETHER_TCL_SONAME`; and `tcl.run_sandboxed_with_map` is exported.
The Perl bridge was copied from aether at `2609ec841`
(perl/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.perl
+import perl
```

What changes: no `make contrib MODULES=perl` step and no perl dev headers
at build time, and fixes aether's copy needed to be usable: it ignored a
failed `perl_parse` (macOS's perl crashed on the first sub), could not load
XS modules, wrote a shared-map output the host never saw, spliced the map's
values into Perl source and unset the host's own environment variables for
good; host functions (`perl.register`) and `perl.last_error()` are new.
The Ruby bridge was copied from aether at `2609ec841`
(ruby/PROVENANCE.md), and moves the same way, the import line and the
dependency:

```diff
-import contrib.host.ruby
+import ruby
```

What changes: no `make contrib MODULES=ruby` step and no Ruby dev package at
build time; `ruby.finalize_host()` is terminal and says so (a later
`init_host` returns -1 where aether's returned 0 and crashed on the next
eval); a sandboxed run puts the process's environment back when it ends
(aether's deleted the ungranted variables for good); `aether_map_put` in
`run_sandboxed_with_map` writes to the host's map; `puts` output is no longer
lost at exit; and Ruby 3.3 and later get their built-in methods (aether's
copy raised `NoMethodError` for `Integer#times` and the like). RubyGems is not
loaded by default (ruby/README.md, "Lifecycle").

The Go bridge was copied from aether at `2609ec841`
(tinygo/PROVENANCE.md), and moves the same way, the import line and the
dependency (the module keeps the name `tinygo`: the libraries it loads are
standard Go's, since TinyGo's c-shared is wasm-only):

```diff
-import contrib.host.tinygo
+import tinygo
```

What changes: no `make contrib MODULES=tinygo` step. (`call_dynamic` is the
libffi-less stub, as aether's is when libffi was not found.) What does not
change, and is the thing to know: the bridge checks no grant itself. A Go
library makes its system calls without libc, so the preload holds only what
it does through cgo, and aether's `sandbox.enforce` levels are per thread
while Go has threads of its own; only the contained child's kernel layer
holds a Go library (tinygo/README.md, "Four ways").

The Go child-process bridge was copied from aether at `2609ec841`
(go/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.go
+import go
```

What changes: no `make contrib MODULES=go` step; an empty grant list is a
grant list (aether's copy returned -1 for it and started nothing); and the
README says what the preload does for a Go child, which is less than aether's
said: Go makes its own system calls, so on Linux only what a program reaches
through cgo is held, and a static binary is not held at all. `go.run_sandboxed`
still starts nothing on macOS. The containment that holds Go's own calls is
the contained child (go/README.md, "4"), new here.

The Java bridge was copied from aether at `2609ec841`
(java/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.java
+import java
```

`java.grant_jvm_runtime()` is unchanged. What changes: it builds on JDK 21
(aether's agent used JDK 22's `allocateFrom`; the Foreign Function calls are
one class now, `Native`), the agent's grant checker follows the matcher the
rest of Aether uses (ports in `tcp` grants), and the bridge starts the JVM
(`java.run`, `java.run_sandboxed`, `java.run_sandboxed_with_map`), where
aether left that to the caller's `spawn_sandboxed`. What does not change, and is
the thing to know: a JVM does not start under `libaether_sandbox.so`, which
refuses it executable memory, so on Linux neither `java.run_sandboxed` nor the
contained child can run one today (java/README.md, "The wall that stops a
JVM"). The agent and the bridge's other calls are unaffected.

The Factor bridge was copied from aether at `2609ec841`
(factor/PROVENANCE.md), and moves the same way, the import line and the
dependency (it was never in `CONTRIB_HOST_LANGS`: it needed the fork, and
still does):

```diff
-import contrib.host.factor
+import factor
```

What changes: no `make contrib MODULES=factor` step, and no archive to build
and place by hand; `run_sandboxed` and `run_sandboxed_with_map` now push the
grants (aether's accepted `perms` and ignored them), after starting the VM, so
under the preload they bind Factor's own libc calls; and there is a contained
child (factor/README.md says why it cannot start yet).

The Racket and Rhombus bridges were copied from aether at `origin/main` of
2026-11-05 (racket/PROVENANCE.md), and move the same way, the import line and
the dependency:

```diff
-import contrib.host.racket
+import racket
-import contrib.host.rhombus
+import rhombus
```

What changes: no `make contrib` step, and no Racket build tree. Aether's copy
needed the headers and `libracketcs.a` of a built Racket CS at build time and
linked it statically (so it could not be imported from a release install);
this one `dlopen`s Racket's library (`Racket.framework` on macOS; on Linux a
`libracketcs.so` made from the distribution's `libracketcs.a`, README
"Finding Racket"), finds its boot images and collects beside it, and needs
none of the headers (`racket_abi.h`). `run` now prints and returns -1 on an
error (aether's discarded the output and returned 0), `run_sandboxed` pushes
its grants as a level (aether's ignored them), and there are host functions
(`racket.register`). What does not change, and is the thing to know: the
contained child does not work for Racket (Chez Scheme maps anonymous
executable memory, which the preload refuses a contained child), and nothing
stops a run that does not end (racket/README.md, "Stopping a run").

Aether's copies are removed by aether PR #3607 (2026-11-06; Lua's and
Python's went earlier), with no transition period: the bridges here are the
ones, and fixes land here. Until that lands in a release, do not import both
copies of a language in one program: they define the same C symbols.

## License

MIT, as aether: see [LICENSE](LICENSE).

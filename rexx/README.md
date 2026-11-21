# rexx: Rexx from Aether, on RexxAE

`import rexx` lets an Aether program run Rexx on
[RexxAE](https://github.com/aether-lang-dev/RexxAE), an embeddable Rexx
engine **written in Aether**, with RexxJS as its dialect reference. It is the
odd one out among the languages here, in three ways that this README keeps
in view rather than smoothing over:

- **There is no foreign runtime.** The others put an interpreter written in
  C (or a JVM, Go, Chez Scheme) inside or beside your program. RexxAE is
  Aether code: in process it is a binary library your program links
  (`import rexxae`), and in a contained child it is compiled into the worker.
  There is nothing to `dlopen` and no C interpreter whose libc calls need
  holding.
- **It lives in its own repository**, with its own pins, tests and
  containment checks (RexxAE's `make containment`,
  `tools/check_containment.py`). This repository takes it at a pinned commit
  and does not copy it ([PROVENANCE.md](PROVENANCE.md)).
- **It is contained by construction.** RexxAE's builtins compute; it has no
  file, stream, environment, process, network or library function, and
  `ADDRESS` is a request to the host, never a shell. So most of stunt's rows
  are not "the sandbox refused it" but "the language cannot express it", and
  the tests prove that over the engine's whole surface
  ([containment/test_surface.sh](containment/test_surface.sh)), not one
  spelling at a time.

## Where RexxAE comes from

RexxAE has **no release tags** (checked 2026-11-21: `git tag` is empty), so
`../ci/versions.env` pins a commit, `REXXAE_REF`, beside `REXXAE_REPO`. It is
not a floor: it tracks RexxAE's `main` the way the family tracks Aether's
latest release, and moves in a commit of its own that says what it brings.

`../tests/rexx_env.sh` fetches that commit into `build/` (or uses
`REXXAE_SRC`, a checkout of your own) and builds, with the `ae` under test,
into `build/rexxae-lib/`:

- `librexxae.so` (`.dylib` on macOS), RexxAE's Aether binary library, the
  shared-runtime variant (`make aether-lib` in RexxAE), which `import rexxae`
  and so `import rexx` link against;
- `rexxae-worker`, RexxAE's own worker (`worker/worker.ae`), the contained
  child.

To use it from your own program, the same two steps: build the library from a
RexxAE checkout (`make aether-lib`) and put its directory on the module search
path with `--lib` (`ae build app.ae --lib path/to/RexxAE/build/aether`),
beside this package (`ae add`, top-level README). `--lib`, not
`AETHER_LIB_DIR`: ae 0.934.0 finds a binary library under the latter only
when its file name matches its source's
([../asks/ae-lib-dir-binary-library-by-file-name.md](../asks/ae-lib-dir-binary-library-by-file-name.md)).

Why this shape, and not the alternatives:

- **Not `ae add` of RexxAE.** RexxAE is not laid out as an Aether package
  (no `aether.toml`; its engine is `src/engine.ae`, imported as `src.engine`,
  a name that would collide in a consumer), and its documented Aether
  interface is the binary library, which a package import would not build.
- **Not a copy.** A copy of the engine here would be a second place to fix
  it and a second containment review. RexxAE's own tests and
  `tools/check_containment.py` hold the engine; this repository holds what a
  host and a guest do with it, against stunt's catalogue.
- **A pinned commit, as Factor's fork is** (`FACTOR_FORK_REF`): the
  repository's existing way of taking an outside engine, recorded in
  `ci/versions.env`, with this README and [PROVENANCE.md](PROVENANCE.md) saying
  where it comes from. `rexx/module.ae` is the thin layer over RexxAE's
  binary library that makes it `import rexx` like the other languages.

## Four ways to run Rexx from Aether

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Rexx runs | your process (RexxAE's binary library) | your process | your process | its own process (RexxAE's worker) |
| What a script can reach | `SAY`; the host's functions and `ADDRESS` environments, as requests | the same | the same | `SAY` and `LOG`; the functions the host mounted; every `ADDRESS` is refused by the worker |
| Your host functions | not checked | checked against the grants | checked against the grants | checked against the grants; each call is a message the host answers |
| Rexx's own libc calls | none: RexxAE has none | none | none, so the preload holds nothing more | none |
| Time limit | 5 s per run (`rexx.TIME_LIMIT_MS`) | the same | the same | the host's deadline: the worker answers `CANCEL` every 1000 instructions |
| Platforms | everywhere RexxAE builds | same | Linux | Linux with Landlock; macOS refuses to start it |

**What "in-process" means for Rexx.** For the others it is a foreign
interpreter embedded in the host. Here it is Aether code linked into the host,
with no grants of its own to ask for: RexxAE's public entry points are
`@pure`, it imports no module that reaches the system and declares no
`extern` (RexxAE's `tools/check_containment.py`; `test_surface.sh` here
checks the import list and the externs again in the pinned source). So:

- **Way 2 matters for the host's code, not the guest's.** A sandbox block
  holds the host functions a script calls; RexxAE itself makes no
  permission check at all (`test_host_rexx.ae`, "a sandboxed run makes no
  permission check of its own", counted with `std.audit`).
- **Way 3 applies, and adds nothing for Rexx.** The preload holds an
  interpreter's own libc calls; RexxAE makes none a script can cause. The
  rows are run under it anyway (column 3 below) and come out as column 2,
  and `test_surface.sh` runs the whole surface under it with denials logged:
  none, beside a control that shows the log would catch one. That is the
  finding, not a run invented to fill a column; the preload remains a second
  line under the host's own functions.
- **Way 4 is RexxAE's own worker**, which compiles the engine in from
  source, needs no grants, and locks itself down (`std.guest.child`'s
  `child.run`) before the script's first instruction.

### 1. Embed

```aether
import rexx

main() {
    rc = rexx.run("LET n = 6 * 7\nSAY 'n is' n")
    println("rc ${rc}")
}
```

Build with `ae build app.ae --lib <the directory with librexxae>`. It prints
`n is 42` and `rc 0`. `rexx.run` makes a fresh context per call, steps it
10000 instructions at a time, prints each `SAY`, and stops it at 5 s. It
returns the script's `EXIT` code (0 when it just ends), or -1 for an error or
the time limit, with the reason on stderr as `[rexx] ...`. An `ADDRESS`
command is answered RC 1 with `ERRORTEXT` "ADDRESS <env> is not mounted": the
bridge mounts no environment. A call to a function nobody registered ends the
script (`unknown or unavailable function`); RexxAE has no `SIGNAL ON` to catch
it.

For host functions, typed values or state between runs, use RexxAE's own API
directly (`import rexxae`: `rexxae_create`, `rexxae_register_function`,
`rexxae_step`, `rexxae_reply_value`, `rexxae_cancel`; RexxAE's README, "Embed
through the C interface", has the model). That is still way 1.
[containment/host.ae](containment/host.ae) is a complete host.

### 2. Embed inside a sandbox block

```aether
import rexx
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    rexx.run_sandboxed(reader, "SAY 1 + 1")
}
```

`run_sandboxed` runs the script inside `sandbox.enforce(perms)` and pops the
level after, also when the script fails. `run_sandboxed_with_map(perms,
code, token)` adds `AETHER_MAP_GET(key)` and `AETHER_MAP_PUT(key, value)`
over the shared map, with the host's inputs frozen first (a put to one
answers 0). The grants hold the host functions a script reaches; the engine
asks for nothing.

### 3. The same, under `LD_PRELOAD`

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./app
```

The rows come out as in way 2 (the parity table's columns 2 and 3 agree),
because there is nothing in RexxAE for the preload to hold.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.sandbox

main() {
    perms = sandbox.new("rexx") {}
    g, err = guest.launch(perms, "build/rexxae-lib/rexxae-worker", null, "rexx", "")
    if err != "" { println("not started: ${err}") return }   // macOS: refused
    guest.mount(&g, "GREET", 1, 1)
    guest.start(&g, "SAY GREET('rexx')")
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

It prints `hello`. The worker is RexxAE's, built by `../tests/rexx_env.sh`
(or `make worker` in RexxAE). [containment/child_host.ae](containment/child_host.ae)
is a complete host.

## Boundary tests

The shared tests in `tests/boundary.py`, run in this language's worker (`tests/test_boundary.sh`): one script per case, written in rexx (`containment/boundary.tsv`), and what the host saw on the wire. `ok` means the value crossed as the same JSON value; `n/a` means the language has no way to write the case; anything else is what happened instead. Nothing in the tests lists what a worker can do.

<!-- boundary:begin: generated by sh tests/run.sh --write-parity, from build/boundary-*.tsv -->
| case | guest to host | host to guest, passed back |
|---|---|---|
| `str` | ok | ok |
| `str-empty` | ok | ok |
| `str-nonascii` | ok | ok |
| `str-nul` | n/a | ok |
| `str-astral` | ok | ok |
| `str-60k` | ok | ok |
| `int` | ok | ok |
| `int-neg` | ok | ok |
| `int-max` | differs: the host saw [9.223372036854776e+18] | differs: the host saw [9.223372036854776e+18] |
| `int-min` | differs: the host saw [-9.223372036854776e+18] | differs: the host saw [-9.223372036854776e+18] |
| `float` | ok | ok |
| `bool-true` | n/a | ok |
| `bool-false` | n/a | ok |
| `null` | n/a | ok |
| `array` | n/a | ok |
| `map` | n/a | ok |
| `nested` | n/a | ok |
| a call inside a call's argument (`nest`) | ok | |
| an error answer (`FAIL`) to a call (`refuse`) | ends the script | |
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
| `str-nul` | n/a | differs: the host saw ["a"] | same | same |
| `str-astral` | ok | ok | same | same |
| `str-60k` | ok | ok | same | same |
| `int` | differs: the host saw ["42"] | differs: the host saw ["42"] | same | same |
| `int-neg` | differs: the host saw ["-7"] | differs: the host saw ["-7"] | same | same |
| `int-max` | differs: the host saw ["9.2233720368547758e+18"] | differs: the host saw ["9223372036854775807"] | same | same |
| `int-min` | differs: the host saw ["-9.2233720368547758e+18"] | differs: the host saw ["-9223372036854775808"] | same | same |
| `float` | differs: the host saw ["1.5"] | differs: the host saw ["1.5"] | same | same |
| `bool-true` | n/a | n/a | same | same |
| `bool-false` | n/a | n/a | same | same |
| `null` | n/a | n/a | same | same |
| `array` | n/a | n/a | same | same |
| `map` | n/a | n/a | same | same |
| `nested` | n/a | n/a | same | same |
| a call inside a call's argument (`nest`) | n/a | | same | |
| an error raised by the host function (`refuse`) | n/a | | same | |
| a cancel while a call waits (`wait`) | n/a | | same | |
| a limit on a guest loop (`loop`) | stopped by the limit | | same | |
| a 100,000-byte argument (`big-up`) | crossed | | same | |
| a 100,000-byte answer (`big-down`) | crossed | | same | |
<!-- boundary-inproc:end -->

## What crosses the boundary

The top-level README's first table says it in a line; this is the detail, each
claim with the spec that backs it.

- **In-process transport.** none: RexxAE is Aether code in a binary library
  your program links (`import rexxae`); a call is a direct call.
- **Types.** `rexx.run*` return an int (0, the `EXIT` code, or -1). On
  RexxAE's API, values cross as JSON both ways: numbers, strings, booleans,
  null, arrays and objects, in the arguments a script passes and the answers
  a host gives (`test_host_rexx.ae`: "typed values cross both ways as JSON").
  Non-finite numbers stay inside the engine (RexxAE's README).
- **Duplex.** the guest calls the host mid-evaluation: the engine suspends on
  each call, inside an expression, and resumes with the answer; nothing is
  re-run (`test_host_rexx.ae`: "a script suspends on each call
  mid-expression and resumes with the answer"). The host never calls into the
  guest except to step it.
- **Callbacks.** host functions by name with an arity
  (`rexxae_register_function`); `ADDRESS` commands are a second kind of
  request, answered with RC, RESULT and ERRORTEXT (`test_host_rexx.ae`:
  "ADDRESS is answered RC 1, not mounted, and runs nothing"; the
  `exec-bin-sh` row). Nothing the guest makes is handed to the host.
  `REQUIRE` asks a resolver the host mounts (`test_surface.sh`).
- **Errors and state.** a host's error answer ends the script (Rexx cannot
  catch it; `callback-unregistered` is judged by the host from that end); a
  syntax error is found before anything runs (`test_host_rexx.ae`: "an
  error in the script is -1, a syntax error too"). The time limit in
  `rexx.run*`; on the API, an instruction budget per step and
  `rexxae_cancel`, safe from any thread (`test_host_rexx.ae`: "a cancel
  while a call waits ends the script, and the late answer is refused"). A
  fresh context per run.
- **Contained child.** the common `std.guest` conversation (top-level README).
  What differs: the worker is RexxAE's own; a `FAIL` ends the script, so the
  hosts here answer `READ_FILE` and `GETENV` with `{"granted": ...}` objects
  as `grant_file` and `deny` do; the worker answers every `ADDRESS` itself
  (RC 1, not mounted, and an `AUDIT` line), so no command reaches the host;
  `CANCEL` is heard every 1000 instructions and while a call waits, so a loop
  ends `ERROR cancelled by host` instead of being killed (the `spin` row).
- **Tests.** `rexx/test_host_rexx.ae`; the rows in
  [containment/checks.rexx](containment/checks.rexx); the surface in
  [containment/test_surface.sh](containment/test_surface.sh).

### The contained child, in the first table's four terms

What crosses between a host and RexxAE's worker, each with the test that
shows it. "RexxAE's worker-test" is RexxAE's own `make worker-test`
(`tests/test_worker.ae` at `REXXAE_REF`; 12 passing on cachyos, ae 0.934.0,
2026-11-21), not run by this repository's CI.

- **Transport.** `std.guest` over a `std.ipc` socketpair made by
  `sandbox.spawn_socket` (Landlock, seccomp, the preload; refused on macOS:
  checked by every run there). 4-byte length-prefixed frames, at most 64 KiB:
  a 1 MiB `SAY` is refused and the worker killed (the `oversize` row). A
  granted file is an open descriptor, never a path
  (`binding-read-granted-file`; an ungranted one is a `deny`,
  `binding-read-etc-passwd`).
- **Types.** arguments go up as the engine's JSON (`rexxae_arguments_json`),
  answers come down as JSON the engine decodes into Rexx values: strings and
  integers both ways (`callback-greet`, `callback-add`), objects in answers
  (`READ_FILE` and `GETENV` answer `{"granted": ..., "text"|"why": ...}`,
  read as `r.granted`, `r.text`: the `binding-*` rows), booleans in answers
  (`map-overwrite-input` reads `AETHER_MAP_PUT`'s `false`). Arrays, null and
  floats in the child are untested here; in process they cross
  (`test_host_rexx.ae`, "typed values cross both ways as JSON"), and the
  worker passes the same JSON through unchanged.
- **Duplex.** the guest starts every exchange: `SAY` and `LOG` up with no
  answer, `CALL name [json]` up and the host's answer down before the script
  goes on; the engine suspends inside the expression and resumes with the
  answer (in process: "a script suspends on each call mid-expression"). One
  call open at a time. The host speaks unasked only to cancel: `CANCEL` is
  heard every 1000 instructions and while a call waits, so a loop ends
  `ERROR cancelled by host` (the `spin` row) and a script waiting on a reply
  is cancelled too (RexxAE's worker-test). `ADDRESS` never reaches the host:
  the worker answers RC 1 itself and sends an `AUDIT` line (`exec-bin-sh`,
  stricter).
- **Callbacks.** the host mounts functions by name with an arity
  (`guest.mount`); the script calls them as Rexx functions. A name not
  mounted ends the script, "unknown or unavailable function"
  (`callback-unregistered`); a `FAIL` answer ends it too, since Rexx cannot
  catch an error (no `SIGNAL ON`), which is why the hosts here answer
  refusals as `{"granted": false}` values. Nothing the guest makes is handed
  to the host.

## Stunt parity

Each of stunt's tests, as Rexx comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd left out | differs: open-etc-passwd left out | differs: exec-bin-sh stricter, fork left out, open-etc-passwd left out |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-granted-file left out, open-etc-passwd left out, write-granted-file left out, getenv-home left out, env-cached left out | differs: open-granted-file left out, open-etc-passwd left out, write-granted-file left out, getenv-home left out, env-cached left out | differs: open-granted-file left out, open-etc-passwd left out, syscall-fn-openat left out, syscall-openat left out, write-granted-file left out, getenv-home left out, env-cached left out |
| `test_stunt_fork` | `fork` | differs: open-etc-passwd left out | differs: open-etc-passwd left out | differs: fork left out, open-etc-passwd left out |
| `test_stunt_host_checked` | `-` | matches | matches | differs: silent left out, half-frame left out |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd left out | differs: open-etc-passwd left out | differs: dlopen-libc-by-name left out, dlopen-libc-by-path left out, open-etc-passwd left out |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd left out, open-granted-file left out | differs: open-etc-passwd left out, open-granted-file left out | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, open-etc-passwd left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, open-etc-passwd left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out, syscall-socket left out, open-etc-passwd left out |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out | differs: connect-192.0.2.1 left out, connect-ipv4-mapped left out |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd left out, open-granted-file left out, write-granted-file left out, getenv-home left out, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, dlopen-libm left out | differs: open-etc-passwd left out, open-granted-file left out, write-granted-file left out, getenv-home left out, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, dlopen-libm left out | differs: open-etc-passwd left out, open-granted-file left out, write-granted-file left out, getenv-home left out, env-cached left out, connect-192.0.2.1 left out, connect-ipv4-mapped left out, fork left out, dlopen-libc-by-name left out, dlopen-libc-by-path left out, dlopen-libm left out, syscall-fn-openat left out, syscall-openat left out, syscall-socket left out |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: write-granted-file left out, open-granted-file left out | differs: write-granted-file left out, open-granted-file left out | differs: write-granted-file left out, open-granted-file left out |

- Column 2: macOS 27.3 arm64, ae 0.934.0, Rexx RexxAE 5c8862c
- Column 3: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, Rexx RexxAE 5c8862c
- Column 4: CachyOS, kernel 7.3.5 x86_64, ae 0.934.0, Rexx RexxAE 5c8862c
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt` at
`AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies. The table is generated from
a run (`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it say
which host produced which column.

Almost everything that differs is **left out**, because RexxAE cannot express
the row; `exec-bin-sh` is **stricter** in the child, because RexxAE's worker
mounts no `ADDRESS` environment. Nothing is "not held" and nothing
"escapes".

## Containment table

What a Rexx script gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Rexx way
([containment/checks.rexx](containment/checks.rexx)), under the policies of
aether's `tests/integration/contrib_host_stunt` (`../tests/test_stunt_*.sh`).
Every cell is a test outcome (`../tests/run.sh`), checked on macOS arm64 and
on CachyOS x86_64 with Landlock. The last column says which kind of outcome
it is:

- **cannot express**: no builtin, extension, `ADDRESS` environment or
  library function RexxAE ships reaches it; `test_surface.sh` is the
  evidence, over every name;
- **RexxAE refuses**: the engine's or its worker's own check (an unknown
  function; an `ADDRESS` environment the worker does not mount);
- **the sandbox refuses**: the grants say no, through the host's check
  (`aether_sandbox_check_slow`, held to every level in force);
- **the host grants**: a host request the host chose to answer, under a
  grant, and refused without one; each such row is run under both kinds of
  policy, so none is vacuous.

| Row | In-process (macOS; Linux under the preload) | Contained child, Linux | Kind |
|---|---|---|---|
| `binding-read-granted-file`, `binding-read-etc-passwd` (`READ_FILE`) | per policy: the host reads the file when `fs_read` grants it, and answers `refused:` otherwise | per policy; a granted file goes down as a descriptor | the host grants; the sandbox refuses |
| `binding-getenv-home` (`GETENV`) | per policy | per policy | the host grants; the sandbox refuses |
| `exec-bin-sh` (`ADDRESS SYSTEM "exit 0"`) | per policy: the host's SYSTEM environment runs `/bin/sh -c` when `exec` grants `/bin/sh`, and answers `refused:` otherwise | refused, whatever the grants: the worker mounts no `ADDRESS` environment (**stricter**) | in process, the host grants or the sandbox refuses; in the child, RexxAE refuses |
| `callback-greet`, `callback-add` | work | work | the host grants |
| `callback-unregistered` (`NOPE()`) | refused: the engine ends the script, "unknown or unavailable function: NOPE" | same | RexxAE refuses |
| `map-get`, `map-set` | work (the bridge's `AETHER_MAP_*`) | work (the host's map) | the host grants |
| `map-overwrite-input` | refused: `AETHER_MAP_PUT` answers 0, the input is unchanged | same | the host refuses (the map's frozen input) |
| `spin` (`DO FOREVER`) | stopped at the bridge's 5 s time limit | `ERROR cancelled by host`: the worker answers the deadline's `CANCEL` | host-checked |
| `oversize` (`SAY COPIES('x', 1048576)`) | (child-only row) | killed: the frame is refused | host-checked |
| `open-etc-passwd`, `open-granted-file`, `write-granted-file` | left out: no `STREAM`, `LINEIN`, `LINEOUT`, `CHARIN`, `CHAROUT`, no RexxJS `FILE_*` | | cannot express |
| `getenv-home`, `env-cached` | left out: no `VALUE(...,'ENVIRONMENT')`, no `GETENV` of its own; the engine never reads the environment | | cannot express |
| `connect-192.0.2.1`, `connect-ipv4-mapped` | left out: no sockets, no RexxJS `HTTP_*` | | cannot express |
| `fork` | left out: no process API; `ADDRESS` is a request | | cannot express |
| `syscall-fn-openat`, `syscall-openat`, `syscall-socket` | left out: no `syscall()`, no way to call C | | cannot express |
| `dlopen-libm`, `dlopen-libc-by-name`, `dlopen-libc-by-path` | left out: no FFI, no `extern` in the engine; `REQUIRE` goes to a resolver the host mounts | | cannot express |
| `silent` | (child-only row) | left out: a script cannot keep the worker from answering `CANCEL`: it polls every 1000 instructions, and no single builtin runs long (`test_surface.sh` times each on 1 MiB strings; four did, below, until RexxAE fixed them) | cannot express |
| `half-frame` | (child-only row) | left out: the worker frames what a script says | cannot express |

What the table says:

- **Containment is by construction, and the tests show it over the whole
  surface.** `test_surface.sh` takes the names from RexxAE's own inventories
  at the pinned commit and checks: the engine's function lists in
  `src/*.ae` are exactly `tests/guest_functions.json` (79 names: 71 RexxJS
  builtins and 8 RexxAE extensions, `CRON_*` and `LOG_*`); the engine
  imports only string, map, JSON, math and memory modules (and `std.host`
  for its quota) and declares no `extern`; each of the 79 is called with
  arguments that name a file, a variable, a program, an address and a cron
  expression (553 calls) inside a no-grant sandbox block, and none asks the
  host for anything (the `LOG_*` extensions excepted, which send the host a
  log event) or makes a permission check; every other name RexxJS has (315,
  its 79 host-effect functions and 20 date and time functions among them)
  and classic Rexx's I/O and clock names are unknown functions; `ADDRESS`
  under six environment names is a request each time; and every builtin and
  extension is timed on 1 MiB strings (474 calls), each within 3 s. On
  Linux the sweep runs again under the preload, logging denials: none. The cron extension does
  not read a clock: `CRON_NEXT` takes the instant it starts from as an
  argument.
- **What a host registers is what a guest gets, and no more.** Every row
  that succeeds goes through a host function or environment, and each one
  that succeeds under a grant is refused under a policy without it, in the
  same run.
- **The child is stricter by one row**, `exec-bin-sh`: RexxAE's worker
  answers `ADDRESS` itself. A host that wants commands from a contained Rexx
  script mounts a function instead.
- **No effect escaped RexxAE; one bound did not hold, and is fixed there.**
  Nothing in its surface reached the file system, the environment, the
  process table, the network or a clock without a host request. But the
  sweep's timing pass found four builtins that ran for seconds in one call
  on 1 MiB strings: `TRANSLATE`, `VERIFY` and `STRIP` looked each character
  up in their whole table (14 s), and the cron parser behind `CRON_VALID`
  and `CRON_NEXT` built its text a byte at a time (6 s). One call is one
  instruction to the step budget and a `CANCEL` is heard between
  instructions, so such a call held the worker past any deadline (it could
  have been the `silent` row) and an in-process run past its time limit.
  RexxAE made them linear (its commits `0545b5f` and `5c8862c`, with a spec
  seen failing first), and `REXXAE_REF` moved past them; the sweep now
  fails if any builtin takes over 3 s.

## Tests

- [`test_host_rexx.ae`](test_host_rexx.ae): the bridge (`run`, the `EXIT`
  code, errors, `ADDRESS` not mounted, the time limit, no permission check of
  the engine's own, the shared map and its frozen inputs, the grant stack
  left as found) and the boundary on RexxAE's API (typed values, suspension
  mid-expression, a cancel while a call waits).
- [`containment/test_surface.sh`](containment/test_surface.sh), with
  [`containment/surface.ae`](containment/surface.ae): the engine's whole
  surface, above.
- `../tests/test_stunt_*.sh` and `../tests/test_catalogue.sh`: the tables
  above. `GUEST_LANG=rexx sh tests/run.sh` runs all of it.

Containment can only be exercised on Linux (the child needs Landlock, the
preload is Linux's); on macOS the child is checked to refuse.

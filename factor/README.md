# factor: embedded Factor

`import factor` lets an Aether program run [Factor](https://factorcode.org)
(a dynamically-typed, image-based **concatenative** language in the Forth and
Joy tradition, with first-class quotations and a CLOS-style object system)
in its process, in a live, persistent VM. It is the bridge that was aether's
`contrib.host.factor`, moved here ([PROVENANCE.md](PROVENANCE.md)): the same
functions under the same names, `factor.eval`, `factor.run`, `factor.set`,
`factor.get`, `factor.run_sandboxed`. It loads libfactor at run time and needs
no Factor dev package to build.

> **Status: experimental, and it needs a forked Factor.** This bridge is
> unlike the others here. Read the two caveats below, and "Four ways", where
> the fourth (a contained child) cannot start yet.

## Caveat 1: it needs the embed-api fork of Factor

The modern upstream VM has no re-entrant C embedding entry point: it once
shipped `start_embedded_factor`, since deleted. The surviving public path
(`start_standalone_factor`) takes the process over and `exit()`s, which is
useless for a host that needs the result back. So this bridge cannot bind a
distribution's `libfactor` (Arch's `factor` package, for one, has none of
the entry points below).

It depends on a small fork that restores embedded eval:
[`aether-lang-dev/factor-language`](https://github.com/aether-lang-dev/factor-language),
`vm/embed_api.cpp`, on top of upstream `master`:

```c
/* generic, NOT Aether-specific */
char* factor_embed_eval(const char* image_path, const char* src);
void  factor_embed_eval_free(char* result);
/* host key-value hooks, for the shared map */
void  factor_embed_map_set_hooks(const char* (*get)(const char*), void (*put)(const char*, const char*));
```

It reuses Factor's own `OBJ_EVAL_CALLBACK` / `eval>string` machinery and a
**stock `factor.image`** baked by the normal bootstrap (no custom
`save-image`). The map interop is layered in the bridge on top of the
generic eval, so the fork's API stays Aether-free and upstream-proposable.
The tests were run against the fork at `c467127bc2` (`FACTOR_FORK_REF` in
`../ci/versions.env`), built on CachyOS x86_64.

## Caveat 2: what contains Factor, and what does not

aether's copy of the bridge said the LD_PRELOAD gate "does not cleanly
contain" Factor (its VM does its own GC, JIT, signals and threads), and
accepted `perms` in `run_sandboxed` and ignored it. Measured here, the gate
does hold Factor's libc calls **once the VM is up**: `io.files`' `open`, the
FFI's `connect` and `getenv`, `dlopen`, `run-process`. The bridge now pushes
the grants like the other bridges, after starting the VM (starting it reads
the image and maps the code heap executable). What the gate does not hold is
Factor's `os-envs`, which reads the process's `environ` array without asking
`getenv`, the same as Python's `os.environ`.

The stronger answer, a contained child, **cannot start Factor on ae 0.889.0**:
see "4. A contained child".

## Four ways to run Factor from Aether

From least to most contained, as for every language here. The first three run
Factor inside your program; the fourth runs it in a separate, locked-down
process, and for Factor that one is blocked by the preload (below).

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Factor runs | your process | your process | your process | its own process (`factor/worker`) |
| Your host functions | none: the shared map is the only channel | the map, checked against the grants | the map, checked against the grants | each call is a message the host answers (the tests' convention, below) |
| Factor's own file, socket, process and FFI calls | not checked | **not checked** | checked against the grants (Linux), except `os-envs` | refused beyond what the host hands over |
| Loading a vocabulary not loaded yet | yes | yes | only with a read grant on Factor's tree | only what the worker loaded before its lockdown |
| Platforms | everywhere the fork's libfactor loads | same | Linux | **does not start** on ae 0.889.0 (Linux with Landlock otherwise) |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0; the exec rows from 0.884.0 | ae 0.878.0 (`std.guest`) |

### 1. Embed: Factor in your process, no containment

```aether
import factor

main() {
    r = factor.eval(<<FACTOR
        USING: math kernel prettyprint ;
        IN: scratchpad
        : fib ( n -- m ) dup 2 < [ ] [ dup 1 - fib swap 2 - fib + ] if ;
        10 fib pprint
    FACTOR
    )
    println("fib(10) = ${r}")                  // fib(10) = 55

    factor.set("who", "Aether")                // the VM is persistent
    _ = factor.eval(<<FACTOR
        USING: namespaces kernel sequences ;
        "who" get "hello, " prepend "greeting" set-global
    FACTOR
    )
    println(factor.get("greeting"))            // hello, Aether
}
```

([examples/way1_embed.ae](examples/way1_embed.ae)). A heredoc carries
Factor's own quotes verbatim, with no interpolation. `eval` returns what the
snippet **printed** (under `eval>string`, effect `( -- )`): leave a clean
stack and print what you want back. A snippet that errors (a stack-effect
mismatch, an undefined word) comes back as Factor's own error text, not as
`""`, so check for it if you treat a non-empty result as success.

### 2. Embed inside a sandbox block

```aether
import factor
import std.sandbox

main() {
    // Load the vocabularies the script names with no level in force.
    _ = factor.eval("USING: io.files io.encodings.utf8 continuations kernel sequences math.parser namespaces ;")

    reader = sandbox.new("reader") { grant_fs_read("/etc/hostname") }
    script = <<FACTOR
        USING: io.files io.encodings.utf8 continuations kernel sequences math.parser namespaces ;
        IN: scratchpad
        : try-read ( path key -- )
            [ [ utf8 file-contents length number>string " bytes" append ] [ 2drop "refused" ] recover ] dip
            set-global ;
        "/etc/hostname" "hostname" try-read
        "/etc/passwd" "passwd" try-read
    FACTOR
    if factor.run_sandboxed(reader, script) != 0 { println("factor failed") }
    println("/etc/hostname: ${factor.get("hostname")}")
    println("/etc/passwd:   ${factor.get("passwd")}")
}
```

([examples/way2_sandbox.ae](examples/way2_sandbox.ae)).
`run_sandboxed` starts the VM if it is not up, then pushes the block's
grants as a level for the length of the call. Aether's own std calls and the
shared map are checked against them on every platform; Factor's `io.files`,
FFI, `dlopen` and `run-process` go straight to libc, so on their own they are
**not** checked. Without the preload this prints `13 bytes` and `2111 bytes`.

### 3. The same, under `LD_PRELOAD`

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./way2_sandbox
```

The same program prints `13 bytes` and `refused`. The preload sits between
Factor and libc and holds Factor's own calls to the grants (Linux). It is a
library interposer, not a kernel boundary: a raw system call made without
libc is not seen. `checks.factor`'s `syscall-*` rows make one, with a few
bytes of machine code mapped from a memfd (`alien-indirect`); only the
contained child's kernel layer would hold those. The containment table below
has every row.

### 4. A contained child, over `std.guest`

```aether
import std.guest
// ... a policy with the grants the worker needs to start (below), then:
g, err = guest.launch(perms, "build/factor-worker", args, "factor", "")
guest.mount(&g, "greet", 1, 1)
guest.start(&g, script)       // then guest.next(&g) for SAY, CALL, DONE ...
```

([examples/way4_child.ae](examples/way4_child.ae), complete).
`factor/worker/worker.ae` is a std.guest worker: it loads libfactor, starts
the VM from the image and loads the script's vocabularies before it locks
itself down, then runs the script with the bridge's `eval`. Build it with
`ae build factor/worker/worker.ae -o build/factor-worker`.

**On ae 0.889.0 it does not start.** `sandbox.spawn_socket` puts
`libaether_sandbox.so` in every child, and the preload refuses any anonymous
executable `mmap` and any `mprotect` to `PROT_EXEC`, whatever the grants
("Allow the dynamic linker and JIT runtimes to map their own code" says its
comment, but only an `fd`-backed mapping is let through). Factor's VM maps
its code heap and its callback heap that way as it starts:

```
fatal_error: Out of memory in mmap: 0x42000
Native backtrace (up to 32 frames):
.../libfactor.a(_ZN6factor7segmentC1Embm+0x7a)
.../libfactor.a(_ZN6factor13callback_heapC1EmPNS_9factor_vmE+0x31)
```

(`segment` is the VM's executable allocation; the `EACCES` is the preload's
`mmap_exec_anonymous`.) So in this repository's tests the child mode finds
this exact failure with its probe, reports it, and `GHL_REQUIRE_CHILD=1` does
not turn it into a failure (`lang_child_blocked` in `containment/lang.sh`);
any other reason the child does not start still fails. The day the preload
lets a JIT map its code, the probe passes and the child rows run. "In a
contained child" below says what runs once it is let through, which was
measured with a copy of the preload with those two checks removed.

## Stunt parity

Each of stunt's tests, as Factor comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how. Column 2 (macOS, no preload) has no host:
there is no Factor to run on macOS. Column 4 is not run: the worker cannot
start with the shipped preload.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | not run: no Factor on macOS | matches | not run: the worker cannot start |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | not run: no Factor on macOS | matches | not run: the worker cannot start |
| `test_stunt_fork` | `fork` | not run: no Factor on macOS | from ae 0.884.0: matches; before: differs: exec-bin-sh escapes | not run: the worker cannot start |
| `test_stunt_host_checked` | `-` | not run: no Factor on macOS | differs: spin left out | not run: the worker cannot start |
| `test_stunt_native` | `native=*` | not run: no Factor on macOS | matches | not run: the worker cannot start |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | not run: no Factor on macOS | matches | not run: the worker cannot start |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | not run: no Factor on macOS | matches | not run: the worker cannot start |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | not run: no Factor on macOS | matches | not run: the worker cannot start |
| `test_stunt_no_grants` | `-` | not run: no Factor on macOS | from ae 0.884.0: differs: env-cached escapes; before: differs: env-cached escapes, exec-bin-sh escapes | not run: the worker cannot start |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | not run: no Factor on macOS | matches | not run: the worker cannot start |

- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Factor the embed-api fork at c467127bc2
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Factor the embed-api fork at c467127bc2
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies (the contained child's with
the grants it needs to start added, below). The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run.

## Containment table

What a Factor program gets when it tries each row of stunt's catalogue
(aether `contrib/host/stunt`, at the ref in `../ci/versions.env`), done the
Factor way ([containment/checks.factor](containment/checks.factor)), under
the policies of aether's `tests/integration/contrib_host_stunt`
(`../tests/test_stunt_*.sh`). "Per policy" means granted where the policy
grants it and refused where it does not, as stunt expects. The columns are the
ways above: "in-process, Linux (preload)" is way 3, and "contained child" is
way 4, **measured with a copy of `libaether_sandbox.so` whose `mmap` and
`mprotect` executable checks were removed**, since the shipped one does not
let Factor start. The in-process column is a plain test outcome on CachyOS
x86_64 with Landlock (ae 0.889.0 and 0.878.0), and in CI; the child column is
not checked by any test or job that runs on a shipped ae. Nothing of it is
checked on macOS, which has no Factor here.

| Row | In-process, Linux (preload) | Contained child, Linux (preload with the executable-memory checks removed) |
|---|---|---|
| `open-etc-passwd` (`utf8 file-contents`) | refused | refused |
| `open-granted-file` (`utf8 file-contents`) | per policy | refused under every policy: the file comes through `read_file` |
| `write-granted-file` (`utf8 set-file-contents`) | per policy | refused under every policy |
| `getenv-home` (libc's `getenv`, through the FFI) | per policy | refused under every policy: the worker has no environment; `getenv` is a host function |
| `env-cached` (`os-envs`, the `environ` array) | **escapes**: no `getenv` is asked | refused under every policy: the worker is started with no environment |
| `connect-192.0.2.1`, `connect-ipv4-mapped` (FFI `socket`, `connect`) | per policy | refused under every policy: the worker's lockdown has no tcp grant |
| `exec-bin-sh` (libc's `system()` and `run-process`) | per policy, from ae 0.884.0; before it **escapes** (`system()`) | refused under every policy, from ae 0.884.0 |
| `fork` (FFI `fork`) | (child-only row) | refused under every policy: no fork grant |
| `dlopen-libm` (`dlopen`) | loads | loads |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`dlopen`) | (child-only row) | refused under every policy, `native=*` included |
| `syscall-fn-openat` (libc's `syscall()`, through the FFI) | (child-only row) | refused |
| `syscall-openat`, `syscall-socket` (the syscall instruction, from machine code in a memfd mapping, `alien-indirect`) | (child-only row) | per policy: Landlock refuses the open, seccomp lets `socket(AF_INET)` through |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy; a granted file goes down as a descriptor |
| `callback-greet`, `callback-add` | work | work |
| `callback-unregistered` | refused: a FAIL that names the function | same |
| `map-get`, `map-set` | work | work (the host's map) |
| `map-overwrite-input` | refused (read back: still the host's value) | same |
| `spin` | **left out**: see below | **left out** |
| `silent` (a loop nothing interrupts) | (child-only row) | killed: no answer to CANCEL within the grace period |
| `oversize` (one 1 MiB `print`) | (child-only row) | killed: the frame is refused |
| `half-frame` (`c-write` of a frame's header and part of its payload to the worker's own socket, `AETHER_IPC_FD`) | (child-only row) | killed: the host times out inside the frame |

What the table says:

- **`spin` is the one row left out**, because Factor cannot express what it
  tests: being stopped while it runs a word. The bridge has no instruction
  limit (Lua's and Python's have), and the worker has no hook to look for a
  CANCEL from (their `debug.sethook` and trace functions): Factor's green
  threads are cooperative, so nothing runs while a word loops. A loop is ended
  only by the host's kill, which is the `silent` row; so a Factor script
  cannot be *cancelled* at a deadline, only killed after the grace period.
- **`env-cached` escapes the preload**, as Python's `os.environ` does:
  `os-envs` walks libc's `environ` array in memory. `os-env`, the word, is
  `getenv` and is held. The test runs the row expecting it to succeed, so the
  day the preload holds it the test fails and this table is brought up to
  date.
- **In-process is not containment on its own**, and the bridge's host
  functions are only the shared map: `binding-*` and `callback-*` ride on it
  as a convention of the tests' hosts (below).
- **The contained child holds every row it runs**, and is stricter than the
  policy: the worker locks itself down with no grants before the script
  starts, so a script reaches a granted file only through the host and never
  gets a handle on libc, a socket, a fork or a shell.

### Host functions

The bridge has no host-function API: the only channel to the host is the
fork's key-value hook pair, which the bridge uses for the shared map
(`aether-map-get`, `aether-map-put`). The tests' hosts give Factor host
functions over the same hook with a convention of their own: a get with the
key `^A name ^A json-args` asks the host for the function `name` and is
answered `KIND ^A text`, KIND being `VALUE` (text is the JSON value), `FAIL`
(the message) or `CANCEL`. In-process `containment/host_support.c` answers in
C (`read_file` and `getenv` check the grants themselves, as stunt's host
does); in the child the worker turns each into a `CALL`. This is the tests',
not a feature of the bridge; `checks.factor` is a short example of using it.

## What a sandboxed script can load

Factor loads vocabularies from its image; a `USING:` of one that is in the
image still reads the disk the first time it is parsed (a `platforms.txt` or
`architectures.txt` beside the vocabulary: `unix` has one), and one that is
not in the image is loaded from `basis/` or `extra/`. Under a level with no
read grant on Factor's tree, either fails ("Permission denied" on the open,
as a Factor error), and in the contained child, where the process has no
grant at all, it can crash the VM (below).

So **load what the script names before the level goes in force**:
[containment/preload.factor](containment/preload.factor) is a `USING:` of
every vocabulary `checks.factor` uses, evaluated by the host
(`containment/host.ae`) before it pushes any grants, and handed to the worker
as its third argument. Once parsed, a vocabulary reads nothing.
`run_sandboxed` and `run_sandboxed_with_map` start the VM first for the same
reason (it reads the image).

## Loading libfactor and the image

The bridge's C (`aether_host_factor.c`) is compiled into the program that
imports `factor` (`@source` in `module.ae`); there is no archive to build
first. libfactor is not linked: the bridge `dlopen`s it at the first call, and
every entry point is looked up with `dlsym` (the types are in
[factor_abi.h](factor_abi.h)). Where it looks, in order:

1. `${AETHER_FACTOR_SONAME}`: an exact path or soname of the fork's libfactor
   (`/opt/factor/libfactor.so`; Factor's own makefile names the engine
   `libfactor.a` on Linux, though it is an ELF shared object, which `dlopen`
   takes by path);
2. `libfactor.so` (unversioned, if installed).

Factor also needs its bootstrapped image: set `${AETHER_FACTOR_IMAGE}` to the
`factor.image` path. If the library cannot be loaded, or lacks
`factor_embed_eval` (a distribution's), `factor.*` returns the error shape
(`-1` or `""`) with a message. `../tests/factor_env.sh` finds a checkout's
(`FACTOR_HOME`, `~/scm/factor-language`, `~/factor-language`):

```sh
. tests/factor_env.sh && factor_soname   # exports AETHER_FACTOR_SONAME, _IMAGE, FACTOR_HOME
```

## Building the fork

```sh
git clone https://github.com/aether-lang-dev/factor-language && cd factor-language
make linux-x86-64                                  # factor, libfactor and its test library
curl -fsSLO https://downloads.factorcode.org/images/master/boot.unix-x86.64.image
./factor -i=boot.unix-x86.64.image                 # bootstraps factor.image
```

The CI jobs do this at `FACTOR_FORK_REF` and cache the result (the recipe
there is the one step not run on a runner; the tests were run against a build
of the same commit made by hand).

## In a contained child

`factor/worker/worker.ae` takes the libfactor path and the image path as its
first two arguments (it is given no environment) and, optionally, a
`USING:` form to load as its third. It starts the VM, loads the vocabularies
and hooks the host-function channel, then locks itself down and runs the
script with `factor.eval`: what the script prints is one `SAY` when it ends
(the bridge's eval gives output, not a stream), a host function is a `CALL`
the host answers, and the end is `DONE 0`. There is no CANCEL hook (see
`spin` above).

What the worker's process must be granted to start Factor (measured; no row
reads any of it), which
[containment/child_host.ae](containment/child_host.ae) adds to every policy:

- a read of **libfactor and the image** (not in a system directory);
- a read of **`/dev/random` and `/dev/urandom`**: Factor opens both as it
  starts, and with them refused it fails while printing the error;
- **`./.cache/factor` of the working directory**, made by the host first,
  with read and write: with no `HOME` Factor lists and cleans that directory
  as it starts, and fails while printing the error if it cannot;
- a read of Factor's own tree (`FACTOR_HOME/*`, and its
  `basis/unix/platforms.txt`) to load the vocabularies before the lockdown;
- a deadline longer than the VM's start, which takes a second or two with a
  154 MB image: the host's `deadline_ms` starts with the script, and
  `child_host.ae` sets 20 s (8 s for the host-checked rows, where the `silent`
  row is then ended by the deadline and not by the start).

All of it is read before the lockdown. A Factor error that happens before
the VM has set up its output ("Error in print-error!", then `die`) crashes
instead of printing, so a missing grant looks like a crash, not a refusal.

## API

| Call | Effect |
|------|--------|
| `factor.eval(code) -> string` | evaluate `code` in the persistent VM; returns its **printed output** (`""` if the library cannot load; Factor's error text if the snippet errors) |
| `factor.run(code) -> int` | fire-and-forget; 0 on success (also when the snippet printed an error) |
| `factor.set(key, value)` | store a string value under `key` in the VM's global namespace |
| `factor.get(key) -> string` | read `key` back as raw text (`10`, not `"10"`); `""` if absent |
| `factor.run_sandboxed(perms, code)` | `run` with `perms` pushed as a level for the call, after starting the VM |
| `factor.run_sandboxed_with_map(perms, code, map_token)` | the same, with live access to an Aether-owned shared map |
| `factor.init()`, `factor.finalize()` | load libfactor; `finalize` does nothing (the VM lives to process exit) |

The k-v map is **string-only**; numbers round-trip as decimal text. Evaluated
snippets run in a **bare vocabulary** (no default imports): each names what it
uses (`USING: math ; ...`). Defining a word inline needs an `IN:` vocabulary
declaration first (`IN: scratchpad`).

### Shared map

`run_sandboxed_with_map` gives the script a live view of an Aether-owned
shared map through two words the bridge injects:

```factor
"key" aether-map-get          ! -> value string, or f if absent (inputs are frozen)
"key" "value" aether-map-put  ! write or overwrite an output key
```

Aether reads the whole map back afterwards, including keys the script
discovered. Example, a word-frequency map-reduce (`test_host_factor.ae`):

```factor
USING: splitting sequences math math.parser namespaces kernel ;
IN: scratchpad
"text" aether-map-get " " split
[ dup aether-map-get [ string>number ] [ 0 ] if* 1 + number>string aether-map-put ] each
```

Hand in `text = "the cat sat on the mat the cat"` and the map comes back with
`the=3 cat=2 sat=1 on=1 mat=1`. The words are defined in the vocabulary
`aether.host.map`, by a prelude (`LIBRARY: factor`, `FUNCTION:` on the fork's
`factor_embed_map_get` and `factor_embed_map_put`) the bridge puts in front of
the code.

## Performance

Measured by aether on linux-x86-64 (yours will vary): the first call (VM init,
boot, callback install) about 175 ms, once; a warm eval about 0.03 ms; a
`set`/`get` pair about 0.06 ms. A heavy one-time start (the image is about
150 MB on disk, with a comparable resident footprint), then effectively free
steady state; embedding is the wrong tool for a program that evals once and
exits. Factor's generational GC can add pause-time jitter under allocation
heavy loops.

## Implementation notes

- All libfactor access goes through a `dlsym` table: `aether_host_factor.c`
  has no unresolved Factor symbols, and needs none of Factor's headers.
- The VM is one per process, single-threaded, and lives on the thread that
  started it: the bridge does not serialize calls.
- `run` returns 0 whenever the VM answered, including when the snippet printed
  an error; `eval` is how to see it.

## Tests

- [`test_host_factor.ae`](test_host_factor.ae): the fib set-piece, the
  stack-effect error contract, a populated k-v map across calls, a
  read-mutate-write round trip, a string round trip, `run`, and the shared
  map's word-frequency case. It skips when libfactor or the image cannot be
  found (on GitHub Actions a skip is a failure).
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh`: the containment
  table and the parity table above.

`../tests/run.sh` runs everything: `GUEST_LANG=factor sh tests/run.sh`.

# racket: embedded Racket CS from Aether (and Rhombus on the same VM)

`import racket` runs [Racket](https://racket-lang.org) inside an Aether
program: one persistent Racket CS VM (Chez Scheme underneath), booted at the
first call and kept for the life of the process. `import rhombus` runs
[Rhombus](https://rhombus-lang.org) on that same VM ([../rhombus/](../rhombus/README.md)).
The bridge was `contrib.host.racket` in aether's tree
([PROVENANCE.md](PROVENANCE.md)); its functions keep their names.

```aether
import racket

main() {
    fib = <<RKT
        (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
        (display (fib 10))
    RKT
    r = racket.evaluate(fib)                     // r = "55", what the code printed
    racket.set("answer", "42")                   // a string k-v map, shared with the code
    v = racket.get("answer")                     // v = "42"
}
```

**Read this first: Racket CS only embeds as a library on macOS.** Racket's
embedding API (`racket_boot`, `racket_eval`, ...) is a shared library only in
the macOS distribution (`Racket.framework`); upstream's `configure` says
building Racket CS as a shared library "is not currently supported". The
Linux packages ship the static `libracketcs.a` and no `.so`. The bridge
`dlopen`s a library like every other language here, so on Linux that library
has to be made, from the archive that is already in Debian's and Ubuntu's
`racket` package, with the C compiler and no Racket dev package
([Finding Racket](#finding-racket)). And the fourth way, the contained
child, does not work for Racket at all ([4](#4-a-contained-child-does-not-work)).

## What was run, and where

Every claim below is a test outcome from one of these, and nothing else was
run:

| Where | Racket | ae | Ran |
|---|---|---|---|
| macOS 27.2 arm64 | 9.3 CS, `brew install --cask racket` (Racket.framework); `raco pkg install rhombus` for Rhombus | 0.889.0 | the specs (racket and rhombus), the containment rows in-process (column 2), the examples |
| Ubuntu 24.04 arm64, in a podman container (Linux 7.1, Landlock present, gcc 13) | 8.10 CS, `apt install racket`; `libracketcs.so` made from `/usr/lib/libracketcs.a` | 0.889.0 | the specs (racket; no rhombus: 8.10 has none), the containment rows in-process under the preload (column 3), the examples, the contained child (it starts, boots Racket, and dies: [4](#4-a-contained-child-does-not-work)) |

Not run: any ae older than 0.889.0 (the pins in `../ci/versions.env` are the
repository's; Racket is verified against none but 0.889.0), FreeBSD, Windows,
Racket BC, and any Racket other than the two above. **x86_64 Linux cannot use
the distribution's archive:** Ubuntu 24.04's x86_64 `libracketcs.a` is not
position-independent (`ld: relocation R_X86_64_PC32 against symbol stderr can
not be used when making a shared object; recompile with -fPIC`, from
`prim5.o`), so no shared library can be made from it, and `tests/racket_env.sh`
says so and the specs skip. The arm64 archive links (its code is
position-independent as a matter of the architecture's code model). On x86_64
a Racket CS built from source with `CFLAGS=-fPIC` is the way; nothing here has
tried it. CI runs the Linux Racket jobs on arm64 runners for this reason.

## Finding Racket

At the first call the bridge needs four things, and finds each in this order.

**The library** (`dlopen`):

1. `AETHER_RACKET_SONAME`, a name or a path: `Racket.framework/Versions/<v>/Racket`
   on macOS, a `libracketcs.so` elsewhere.
2. `AETHER_RACKET_HOME`, the root of an installation: its
   `lib/Racket.framework/Versions/*/Racket`, `lib/libracketcs.so`,
   `lib/libracketcs.dylib`.
3. The usual places: `/Applications/Racket v*/lib/Racket.framework/Versions/*/Racket`,
   Homebrew's `lib/Racket.framework`, then `libracketcs.so` and
   `libracketcs.dylib` on the loader's path.

**The boot images**, which Racket CS needs besides the library: `petite.boot`,
`scheme.boot` and `racket.boot` (`racket.so` in some builds), in
`AETHER_RACKET_BOOT_DIR`, else `<home>/lib/Racket.framework/Versions/*/boot`,
`<home>/lib/racket`, `<home>/lib`, beside the library (`boot/`, `.`, `racket/`),
`/usr/lib/racket`, `/usr/local/lib/racket`. They are `Racket.framework/Versions/<v>/boot`
on macOS and `/usr/lib/racket` on Debian and Ubuntu.

**The collects directory**, the module library Racket reads as it boots and
for every `require`: `AETHER_RACKET_COLLECTS`, else `<home>/collects`,
`<home>/share/racket/collects`, four directories above the framework
(`<root>/collects`), `/usr/share/racket/collects`, and the Homebrew and
`/usr/local` equivalents.

**The config directory**, `etc/` of the installation (its `config.rktd` names
the package directories, which is how `rhombus` is found when it was
installed with `raco pkg`): `AETHER_RACKET_CONFIG`, else `<collects>/../etc`,
`/etc/racket`, `/usr/local/etc/racket`.

`../tests/racket_env.sh` is the probe the tests use; it exports all four
(`RACKET_HOME=/path` names an installation). It also makes the library on
Linux:

```sh
# libracketcs.a is in Debian's and Ubuntu's `racket` package. -Bsymbolic binds its
# symbols locally, which is what lets position-dependent objects into a shared library.
cc -shared -Wl,-Bsymbolic -o libracketcs.so \
   -Wl,--whole-archive /usr/lib/libracketcs.a -Wl,--no-whole-archive \
   -lm -ldl -lpthread -l:libz.so.1 -l:liblz4.so.1 -l:libtinfo.so.6
export AETHER_RACKET_SONAME=$PWD/libracketcs.so
```

(Built this way on Ubuntu 24.04 arm64 with Racket 8.10: it boots, evaluates,
and the specs and rows pass. Without `-Bsymbolic` the link fails with
"relocation R_AARCH64_ADR_PREL_PG_HI21 against symbol `S_lookup_c_entry`
which may bind externally can not be used when making a shared object". On
x86_64 the same command fails for good, the archive not being
position-independent: see "Not run" above.)
Racket's official Linux installers ship no `libracketcs.a` at all (their boot
images are inside the executable), so on Linux this means the distribution's
Racket.

Two ABI facts the bridge carries in `racket_abi.h`, because there is no
Racket header at build time:

- **Chez's object layout** (`Snil`, `Scons`, `Spairp`, `Sbytevector_data`,
  ...): 64-bit little endian, the same in Chez 9.5 (Racket 8.x) and Chez 10
  (Racket 9.x). A 32-bit Racket is not supported.
- **`racket_boot_arguments_t`** gained a `bootN_data` field after each boot
  path in Racket 8.14. Both layouts are in the header; the bridge picks the
  new one when the library exports `Sregister_boot_file_bytes` (which came
  with it), and `AETHER_RACKET_BOOT_ABI=old|new` overrides. Checked: 8.10
  (old) and 9.3 (new).

The library is Racket CS only. Racket BC was not looked at.

## API

| Call | Effect |
|---|---|
| `racket.evaluate(code) -> string` | evaluate `code` (every s-expression in it) in the VM's namespace; returns **what it printed**, not the last value. On an error: what it printed, then `error: <message>` |
| `racket.run(code) -> int` | evaluate for effect, output to stdout (flushed after each run); 0, or -1 on an error (printed to stderr as `[racket] ...`, kept for `last_error()`) |
| `racket.last_error() -> string` | the message of the last failed `run`/`evaluate` |
| `racket.set(key, value)` / `racket.get(key) -> string` | a string-only k-v map over a hash in the VM; the code sees it as `aether-host-kv`. `""` for an absent key |
| `racket.run_sandboxed(perms, code)` | `run` under the grant list (way 2 and 3) |
| `racket.run_sandboxed_with_map(perms, code, map_token)` | the same, with the shared map: the code calls `(aether-map-get "k")` (`""` if absent) and `(aether-map-put "k" "v")`; Aether enumerates it afterwards with `map.keys`. Inputs are frozen |
| `racket.register(name, fn)` and the `arg_*` / `push_*` calls | host functions, below |
| `racket.init() -> int` | boot now (otherwise at the first call); 0 on success |
| `racket.finalize()` | a no-op: the embedding API cannot tear a VM down and boot another, so the VM lives until the process exits |

The result-returning call is `evaluate`, not `eval`: aether's copy of the
bridge was statically linked with `libracketcs.a`, which exports its own
`racket_eval`. The name is kept. Every call is made from one thread: Racket
CS runs on the thread that booted it, so the main thread, not actors.

Definitions persist: the VM has one top-level namespace (the `racket`
language plus `ffi/unsafe`), and each call evaluates the forms of its code in
it with `eval`.

**Host functions** (new here; aether's copy had none): register an Aether
`@c_callback` as a Racket procedure.

```aether
@c_callback
add(vm: ptr) -> int {
    racket.push_int(vm, racket.arg_int(vm, 1) + racket.arg_int(vm, 2))
    return 1                                       // how many values it pushed
}

racket.register("add", add)
racket.evaluate("(display (add 2 40))")            // "42"
```

Arguments are strings, integers and booleans (`arg_type` says which:
`RACKET_TNIL`, `TBOOL`, `TINT`, `TSTR`), results up to 16 values pushed with
`push_int`, `push_str`, `push_bool` or `push_nil` and returned as multiple
values; `return racket.raise_error(vm, "refused: ...")` makes the call raise
an exception the code can catch. There is one frame, so a host function must
not call back into the VM. The mechanism is `ffi/unsafe` over C functions in
the bridge, so it needs nothing from Racket beyond the FFI.

## Stopping a run

**Nothing here can stop a run that does not end.** aether's other bridges have
an instruction limit or a hook; Racket CS's embedding API has neither. The
obvious tool is a Racket thread that kills the evaluation's thread after a
time, and it was built and measured: on Racket 8.10 on Linux, with the
bridge as well as with `libracketcs.a` linked statically (a plain C
program calling `racket_eval` with `(void)`, then `(sleep 0.5)`, with a
thread spinning), the main thread never woke from a timed wait while another
thread was runnable, in a pattern that depended on how many `racket_eval`
calls had come before and not on the code. (`racket -e` itself is fine, and
a single `racket_eval` that creates a thread and waits on it with
`sync/timeout` also works. The embedded scheduler is the suspect; the cause
was not found.) A time limit that holds on some call counts is worse than
none, so the bridge has none, stunt's `spin` row is left out below, and the
contained child's worker has no cancel hook (a CANCEL is seen only in a
host function's reply).

## Four ways to run Racket from Aether

From least to most contained. All four stay supported; the fourth does not
work for Racket.

| | 1. Embed | 2. Embed in a sandbox block | 3. Embed + `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| Where Racket runs | your process | your process | your process | its own process (`racket/worker`) |
| Your host functions | not checked | checked against the grants | checked against the grants | checked; each call a message |
| Racket's own file, socket, process, `getenv` and FFI calls | not checked | **not checked** | checked against the grants (Linux), all of them | **does not work**: Chez Scheme cannot run under the child's preload |
| `(require ...)` of a module not required yet | yes | yes | only with a read grant on the collects directory | n/a |
| Platforms | everywhere the library loads | same | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0 | ae 0.878.0 (`std.guest`) |

(The ae versions are the repository's pins; Racket was run on 0.889.0 only.)

### 1. Embed: Racket in your process, no containment

```aether
import racket

main() {
    script = <<RKT
        (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
        (printf "fib(10) = ~a\n" (fib 10))
        (displayln (list "it's" #t 'sym))
    RKT
    if racket.run(script) != 0 { println("racket failed: ${racket.last_error()}") }

    // evaluate returns what the code printed; the VM keeps its definitions.
    out = racket.evaluate("(display (* 6 7))")
    println("evaluate gave ${out}")

    racket.set("who", "Racket")
    println(racket.evaluate("(display (string-append \"hello, \" (hash-ref aether-host-kv \"who\")))"))
}
```

(`examples/way1_embed.ae`; run on both hosts above.) It prints

```
fib(10) = 55
(it's #t sym)
evaluate gave 42
hello, Racket
```

A heredoc (`<<RKT ... RKT`) carries the code verbatim: no `${}` and no
escapes, so Racket's quotes and `~a` need none, and the indentation the lines
share is stripped. Booting costs about half a second (the `racket` language is
required at the first call), and the VM commits about 130 MB (resident, on
Ubuntu with Racket 8.10): a
long-lived host amortizes it, a fire-once tool does not.

### 2. Embed inside a sandbox block

```aether
import racket
import std.sandbox

main() {
    reader = sandbox.new("reader") { grant_env("HOME") }
    script = <<RKT
        (define (try label thunk)
          (printf "~a: ~a\n" label
                  (with-handlers ([exn:fail? (lambda (e) "refused")]) (thunk))))
        (try "HOME   " (lambda () (getenv "HOME")))
        (try "USER   " (lambda () (getenv "USER")))
        (try "passwd " (lambda () (if (call-with-input-file "/etc/passwd" read-line) "read it" "?")))
    RKT
    if racket.run_sandboxed(reader, script) != 0 { println("racket failed: ${racket.last_error()}") }
}
```

(`examples/way2_sandbox.ae`.) `run_sandboxed` pushes the block's grants as a
level for the length of the call: Aether's own std calls and every host
function the code reaches are checked against them, on every platform.
Racket's `open-input-file`, `tcp-connect`, `subprocess` and FFI go straight
to libc, so on their own they are **not** checked: on macOS this prints
`/Users/paul`, `paul` and `read it`, and the code could connect anywhere or
run a shell just as well.

### 3. The same, under `LD_PRELOAD`

```sh
LD_PRELOAD="$(ae cflags --libs | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)/libaether_sandbox.so" ./myprog
```

The program from way 2, with `USER=paul` in the environment, on Ubuntu 24.04
arm64 (Racket 8.10, ae 0.889.0), first without the preload and then with it:

| Racket does | without | with `LD_PRELOAD` |
|---|---|---|
| `(getenv "HOME")` | `/root` | `/root` (granted) |
| `(getenv "USER")` | `paul` | `#f`: held |
| `(call-with-input-file "/etc/passwd" read-line)` | reads it | refused (`EACCES`) |

Racket's `getenv` is libc's `getenv` called each time, not a copy taken at
start, so unlike CPython's `os.environ` the preload holds it. Every in-process
row is held (the [containment table](#containment-table)). It is a library
interposer, not a kernel boundary: a raw system call without libc is not
seen, and macOS has no equivalent.

Racket needs one thing the preload would otherwise take away: **anonymous
executable memory.** The preload refuses it only for a process started with
the sandbox's environment (`sandbox_active`), not for a level pushed with
`run_sandboxed` in a process you started yourself, which is why this works
and way 4 does not. `ffi-lib` by path reads Racket's `system.rktd` the first
time, so a program that loads libraries under grants with no read on the
installation should load one first (`containment/host.ae` does).

### 4. A contained child does not work

`racket/worker/worker.ae` and `containment/child_host.ae` exist, are written
like the other languages' (std.guest, mounts and CALLs as JSON, `print`
becoming SAY), and `examples/way4_child.ae` is the host side. It compiles.
What happens when it runs, on Ubuntu 24.04 arm64 with Landlock:

```
allocation failed for code; maybe write+execute permission is not allowed
KILLED frame refused: ipc.read_frame: end of stream
```

The worker gets as far as booting Racket (given a read grant on the
library's, the boot images', the collects' and the config's directories, as
python's worker needs on its standard library: `GHL_RACKET_READ`), and dies
at the first evaluation after its lockdown. Chez Scheme maps anonymous
executable memory for the code it generates; the
sandbox preload that `sandbox.spawn_socket` gives every contained child
refuses exactly that (`mmap_exec_anonymous`, `mprotect_exec`, in
aether's `libaether_sandbox_preload.c`). Nothing in the bridge or the worker
can change that and still be contained, so the fourth way is not offered for
Racket. A JIT runtime that maps its code from a memfd would run; Chez does
not. On macOS `guest.launch` refuses to start a child at all.

`../tests/stunt_lib.sh` probes the child on every run (it runs
`callback-greet` in it) and skips child mode with the worker's own words;
CI does not set `GHL_REQUIRE_CHILD` for Racket. If a later preload or Chez
makes it work, the probe passes and the child rows start to run (and
`lang.sh`'s `child_stricter` is where their differences go). Column 4 of the
parity table below is therefore written by hand.

## Stunt parity

Each of stunt's tests, as Racket comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects,
else the rows that did not and how.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | matches | does not run (way 4) |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: open-etc-passwd not held, write-granted-file not held, env-cached left out | differs: env-cached left out | does not run (way 4) |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | matches | does not run (way 4) |
| `test_stunt_host_checked` | `-` | differs: spin left out | differs: spin left out | does not run (way 4) |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | matches | does not run (way 4) |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, open-granted-file not held | matches | does not run (way 4) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | matches | does not run (way 4) |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | matches | does not run (way 4) |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home not held, env-cached left out, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held | differs: env-cached left out | does not run (way 4) |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held | matches | does not run (way 4) |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Racket 9.3
- Column 3, ae from 0.884.0: Ubuntu 24.04.5 LTS, kernel 7.1.10 aarch64, ae 0.889.0, Racket 8.10
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies; the containment table
below has each row. The table is generated from a run
(`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it say
which host produced which column. Column 4 is not generated: there is no
contained child to run (way 4).

## Containment table

What a Racket program gets when it tries each row of stunt's catalogue
(aether `contrib/host/stunt`, at the ref in `../ci/versions.env`), done the
Racket way ([containment/checks.rkt](containment/checks.rkt)), under the
policies of aether's `tests/integration/contrib_host_stunt`
(`../tests/test_stunt_*.sh`). "Per policy" means granted where the policy
grants it and refused where it does not, as stunt expects. The columns are
the ways above: "in-process, Linux (preload)" is way 3, "in-process, macOS"
is way 2 where there is no preload, and the contained child is way 4, which
does not run. Every cell is a test outcome (`../tests/run.sh`): macOS arm64
(Racket 9.3) and Ubuntu 24.04 arm64 (Racket 8.10), ae 0.889.0.

| Row | In-process, Linux (preload) | In-process, macOS | Contained child, Linux |
|---|---|---|---|
| `open-etc-passwd` (`call-with-input-file`) | refused (`errno=13`) | **not held**: reads it | does not run |
| `open-granted-file` (`call-with-input-file`) | per policy | **not held** | does not run |
| `write-granted-file` (`call-with-output-file`) | per policy | **not held** | does not run |
| `getenv-home` (libc's `getenv`, through the FFI) | per policy | **not held** | does not run |
| `env-cached` | **left out**: Racket keeps no copy of the environment, `getenv` is libc's (`getenv-home`) | left out | left out |
| `connect-192.0.2.1`, `connect-ipv4-mapped` (`tcp-connect`) | per policy (`errno=13` when refused; let through is a connect still in progress after 2 s) | **not held** | does not run |
| `exec-bin-sh` (`subprocess` of `/bin/sh`) | per policy: rktio forks, and the child's exec is refused (`errno=1`, exit 1) | **not held** | does not run |
| `fork` (`fork` through the FFI) | (child-only row) | (child-only row) | does not run |
| `dlopen-libm` (`ffi-lib`) | loads | loads | does not run |
| `dlopen-libc-by-name`, `dlopen-libc-by-path` (`ffi-lib`) | (child-only row) | (child-only row) | does not run |
| `syscall-fn-openat` (libc's `syscall()`, through the FFI) | (child-only row) | (child-only row) | does not run |
| `syscall-openat`, `syscall-socket` (the syscall instruction, from machine code in a memfd mapping, through `function-ptr`) | (child-only row) | (child-only row) | does not run |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home` (host functions) | per policy | per policy | does not run |
| `callback-greet`, `callback-add` | work | work | does not run |
| `callback-unregistered` | refused: an "undefined" exception the code can catch | same | does not run |
| `map-get`, `map-set` | work | work | does not run |
| `map-overwrite-input` | refused (read back: still the host's value) | same | does not run |
| `spin` | **left out**: nothing stops an endless evaluation ([Stopping a run](#stopping-a-run)) | left out | left out |
| `silent`, `oversize`, `half-frame` | (child-only row) | (child-only row) | does not run |

The rows marked "does not run" are written (`checks.rkt` has all of them; the
worker, `child_host.ae` and the host-checked rows for `silent`, `oversize` and
`half-frame` are in place) and have never been run: Racket does not start in
the child. Their claims are therefore none. What the table does say:

- **On Linux the preload holds all of Racket's own calls that stunt tries
  in-process**, `getenv` included, and `subprocess` is held at the fork's
  exec, in every ae version (Racket forks and execs itself; there is no
  `system()` escape as in Lua and Python before ae 0.884.0). It does so with
  Chez's code allocator unaffected, because the preload refuses anonymous
  executable memory only in a process started under the sandbox.
- **On macOS nothing holds Racket's own calls**; the host functions hold.
- **`connect-*` under a grant** only shows the call was let through; nobody
  answers on TEST-NET-1. Racket's `tcp-connect` blocks, so `checks.rkt` runs
  it in a thread and gives it 2 s.
- **The checks need their libraries required first** (`ffi/unsafe`,
  `racket/tcp`, `racket/system`, `racket/port`, `json`; `containment/host.ae`
  does it before any sandbox). Racket reads a module from disk the first time
  it is required, so under grants with no read of the collects directory a
  module not required yet cannot be.

## Rhombus

[../rhombus/](../rhombus/README.md) names the bridge's `rhombus_*` entry
points (the source wrapped as a `#lang rhombus` module body). One VM, one k-v
map: a key set with `racket.set` is read by `rhombus.get`.

## Tests

```sh
GUEST_LANG=racket sh tests/run.sh              # specs, containment rows, parity check
GUEST_LANG=racket sh tests/run.sh spec         # racket/test_host_racket.ae (+ rhombus's, if the Racket has rhombus)
RACKET_HOME="/Applications/Racket v9.3" GUEST_LANG=racket sh tests/run.sh
AETHER_RACKET_SONAME=/path/libracketcs.so AETHER_RACKET_BOOT_DIR=... AETHER_RACKET_COLLECTS=... GUEST_LANG=racket sh tests/run.sh
```

Without a Racket the specs skip and the containment rows skip with "no
Racket CS library found"; on GitHub Actions a skip is a failure.

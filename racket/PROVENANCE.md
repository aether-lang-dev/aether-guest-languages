# Provenance

The Racket and Rhombus bridges were copied from aether's `contrib/host/racket/`
and `contrib/host/rhombus/` at `origin/main` as of 2026-11-05 (aether-lang-dev/aether;
aether is MIT licensed, copyright "Aether Programming Language Contributors";
so is this repository, `../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_racket.c` | `contrib/host/racket/aether_host_racket.c` | rewritten, see below |
| `aether_host_racket.h` | `contrib/host/racket/aether_host_racket.h` | the Racket and Rhombus entry points, and the new ones |
| `module.ae` | `contrib/host/racket/module.ae` | `@source("aether_host_racket.c")`; the new calls; header comment |
| `racket_abi.h` | (new) | the Chez Scheme and Racket CS types and macros the bridge uses |
| `README.md` | `contrib/host/racket/README.md` | rewritten |
| `../rhombus/module.ae` | `contrib/host/rhombus/module.ae` | `import racket` (for the `@source`); header comment |
| `../rhombus/README.md` | `contrib/host/rhombus/README.md` | rewritten |
| `test_host_racket.ae` | `tests/integration/host_racket/uses_racket.ae` | a `std.spec` spec; the fib set-piece, the error contract and the k-v map kept, more cases |
| `../rhombus/test_host_rhombus.ae` | `tests/integration/host_rhombus/uses_rhombus.ae` | the same, as a spec |
| `containment/checks.rkt`, `containment/host.ae`, `containment/child_host.ae`, `worker/` | (new) | stunt's rows in Racket, after `../python/containment/` and `../lua/containment/` |

## What the move changed in `aether_host_racket.c`

In aether the bridge was compiled by `make contrib` (when `$AETHER_RACKET_INCLUDE`
held Racket's headers) into `libaether_host_racket.a`, and the importing
program linked `libracketcs.a` statically, with `-rdynamic`, from
`$AETHER_RACKET_LIB`. It included `chezscheme.h` and `racketcs.h`. Here it is
compiled into each program that imports it (`@source`), and:

- **It `dlopen`s the library.** There is no `libracketcs.a` on the link line
  and no Racket header at build time: `racket_abi.h` carries the Chez
  object macros and both layouts of `racket_boot_arguments_t` (it changed in
  Racket 8.14), and every function is found with `dlsym`. On macOS the
  library is `Racket.framework`; on Linux it is a `libracketcs.so` made from
  the distribution's static archive (README, "Finding Racket"). A machine
  that runs a program built with it needs Racket's runtime files, not a
  build tree.
- **It finds the boot images, collects and config itself** beside the
  library when the environment does not name them, and does not require
  `AETHER_RACKET_BOOT_DIR` (it was mandatory).
- **It reads a prelude instead of building every datum by hand.** aether's
  copy assembled the eval expression as a cons tree in C on every call; this
  one has a small s-expression reader and a Racket prelude it evaluates once
  (`aether-run`, the k-v procedures), after which a call is `(aether-run
  "code" lang capture?)`. A bug that disappears with it: aether's `set`
  held one Racket string across a call into Scheme while building the next,
  where a collection could move it.
- **`run` prints and reports errors.** aether's `run` captured the output
  and discarded it, and returned 0 on a Racket error. Here `run` writes to
  stdout (flushed after each call) and returns -1 on an error, printing it to
  stderr and keeping it for the new `racket.last_error()`.
- **`run_sandboxed` pushes its grants** (`aether_sandbox_push`) around the
  run, as the other bridges do, so `sandbox.enforce` levels and the preload
  apply. aether's accepted `perms` and ignored them ("rely on the process-level
  sandbox"); the README explains what that does and does not give.
- **Host functions are new** (`racket.register` and the `arg_*`/`push_*`
  calls), by `ffi/unsafe` over C functions in the bridge.
- `#include "../../../runtime/aether_shared_map.h"` became
  `#include "aether_shared_map.h"` (and `aether_sandbox.h`, `aether_string.h`):
  the installed runtime's headers, on the include path `ae build` gives every
  C file it compiles. `AETHER_HAS_SANDBOX` is defined in the file, as the
  other bridges do. The `AETHER_HAS_RACKET` stub for a build without Racket is
  gone: the bridge always builds.

There is no time limit: a thread-based one was built and found not to hold on
Racket 8.10 on Linux (README, "Stopping a run"), so it was not shipped.

Later changes made here are in this repository's history (`git log -- racket/`).

aether keeps its copy for a transition period; see the top-level README. Do
not import both copies in one program: they define the same C symbols.

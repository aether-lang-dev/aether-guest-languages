# Provenance

The Go bridge was copied from aether's `contrib/host/go/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, 2026-11-05; v0.889.0's merge),
with what aether has of its tests: nothing of its own (its README said the
closest coverage was the tinygo host's, a different bridge). aether is MIT
licensed, copyright "Aether Programming Language Contributors"; so is this
repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_go.c` | `contrib/host/go/aether_host_go.c` | see below |
| `aether_host_go.h` | `contrib/host/go/aether_host_go.h` | unchanged |
| `module.ae` | `contrib/host/go/module.ae` | `@source("aether_host_go.c")`; header comment; `import go` in the usage |
| `test_host_go.ae` | (new) | the bridge's spec |
| `rows/`, `guestchild/`, `worker/`, `containment/`, `examples/` | (new) | Go, and Aether hosts for it, modelled on `../python/containment` and `../python/worker`; `containment/lang.sh` is `../python/containment/lang.sh`'s counterpart |
| `../tests/go_env.sh` | (new) | finds the go toolchain and checks cgo |

Not brought over: `README.md` (rewritten for this repository; the "Testing"
section's pointer to `tests/integration/host_tinygo` is aether's, and the
tinygo bridge is not this one).

## What the move changed in `aether_host_go.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_go.a`, with relative includes into `runtime/`. Here it is
compiled into each program that imports it (`@source` in `module.ae`), with
the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/aether_sandbox.h"`, `aether_shared_map.h`,
  `utils/aether_spawn.h` and `sandbox/aether_sandbox_preload_path.h` became
  `#include "aether_sandbox.h"`, `"aether_shared_map.h"`,
  `"utils/aether_spawn.h"` and `"sandbox/aether_sandbox_preload_path.h"`: the
  installed runtime's headers (`include/aether/runtime/`), already on that
  include path. The runtime functions the file calls (`aether_fork`,
  `aether_spawn_env`, `aether_pipe_cloexec`, the shared map's) are in
  `libaether.a`, which every program links.

## What was changed beyond the move

- **An empty grant list is a grant list.** `serialize_grants_to_shm`
  returned NULL for a list with no entries (`n <= 0`), and
  `spawn_go_sandboxed` then failed with "cannot serialize grants to shm", so
  `go.run_sandboxed(sandbox.new("x") {}, binary)` returned -1 and started
  nothing instead of running the child with everything refused. It now writes
  the empty segment (one NUL byte), which the preload reads as zero grants.
  `test_host_go.ae` and `test_stunt_no_grants` cover it. aether's
  `contrib/host/aether/aether_host_aether.c` has the same function and the
  same check; it is not fixed there from here.

Nothing else in the file changed in the move. Later changes made here are in
this repository's history (`git log -- go/`).

## Found, not changed

- **The README's claim about macOS.** aether's README said that on macOS the
  Go subprocess runs without the preload and the grants are advisory. The
  code looks for `libaether_sandbox.so` whenever grants are given, finds none
  on macOS and returns -1 with a message, starting nothing. That is the safer
  behaviour and is kept (and documented, `README.md` "2."); the tests hold it.
- **The preload and Go.** The README said grants "apply to libc calls the Go
  binary makes ... exactly the same way they apply to any other process".
  True, and nearly empty: Go's `os`, `net` and `os/exec` make system calls
  themselves, and a static binary ignores `LD_PRELOAD` (`README.md`, the
  containment table).

aether keeps its copy for a transition period; see the top-level README.

# Provenance

The Factor bridge was copied from aether's `contrib/host/factor/` at commit
`2609ec841` (aether-lang-dev/aether, `origin/main`, 2026-11-05). aether is
MIT licensed, copyright "Aether Programming Language Contributors"; so is
this repository (`../LICENSE`). The bridge needs the embed-api fork of
Factor (`aether-lang-dev/factor-language`, `vm/embed_api.cpp`), which is a
separate project under Factor's BSD license and is not copied here.

| Here | From aether | Changed |
|---|---|---|
| `aether_host_factor.c` | `contrib/host/factor/aether_host_factor.c` | see below |
| `aether_host_factor.h` | `contrib/host/factor/aether_host_factor.h` | the include of `aether_string.h` (see below) |
| `module.ae` | `contrib/host/factor/module.ae` | `@source("aether_host_factor.c")`; header comment |
| `test_host_factor.ae` | `contrib/host/factor/test_host_factor.ae` | `import factor` instead of `import contrib.host.factor`; the `describe` name; the header comment; one case added (below) |
| `test_support.c` | (new) | the shared map by token, which an Aether spec cannot make (as `../ruby/test_support.c`) |
| `factor_abi.h` | (new) | the function-pointer types the bridge calls libfactor through |
| `containment/`, `worker/` | (new) | modelled on `../lua/containment` and `../lua/worker` |

Not brought over: aether's `tests/sandbox/test_shared_map_all.sh` case for
Factor (a word-frequency map-reduce through `run_sandboxed_with_map`): it is
the `test_host_factor.ae` case "reads and writes the shared map" here, with
the same text and the same expected counts.

## What the move changed in `aether_host_factor.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_factor.a`, with `-DAETHER_HAS_FACTOR -DAETHER_HAS_SANDBOX` on
the command line, and is left out of the default set because it needs the
fork. Here it is compiled into each program that imports it (`@source` in
`module.ae`), with the cflags `ae build` uses for any program. So:

- `#include "../../../std/string/aether_string.h"` and
  `"../../../runtime/aether_shared_map.h"` became `"aether_string.h"` and
  `"aether_shared_map.h"`, and `"aether_sandbox.h"` was added: the installed
  runtime's headers, already on that include path.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_FACTOR` are defined in the file when
  the build does not define them.
- The two host-hook function-pointer types and the three `dlsym` signatures
  moved to `factor_abi.h`. The bridge never linked against libfactor and has
  no Factor header to include; nothing else of Factor's is used.

And one change of behaviour, which the move is the occasion for:

- **`factor_run_sandboxed` and `factor_run_sandboxed_with_map` push the
  grants** as a level for the length of the call, as the lua, python and
  ruby bridges do. aether's copy accepted `perms` and ignored it ("for
  signature parity"), on the ground that the preload does not cleanly
  contain Factor's VM. Measured here (README.md, "Four ways"), it holds the
  libc calls Factor makes once the VM is up, so the bridge now asks it to.
  Because the VM's start reads the image and maps its code heap executable,
  both functions start the VM first, with no level in force, and push the
  level after. Code that runs under a level can load no vocabulary from the
  disk the first time a `USING:` names it (README.md, "What a sandboxed
  script can load").

`test_host_factor.ae` gained one case, aether's cross-host shared-map test for
Factor (above). That the grants now bind is held by the containment rows
(`../tests/test_stunt_*.sh`, run under the preload), not by a spec: without
`LD_PRELOAD` nothing enforces a level on Factor's own libc calls.

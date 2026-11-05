# Provenance

The Duktape bridge was copied from aether's `contrib/host/duktape/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, the v0.889.0 release merge,
2026-11-05; the directory last changed at `3b8d8fb87`), with the `js` alias
from `contrib/host/js/` and the parts of aether's tests that exercise the
bridge. aether is MIT licensed, copyright "Aether Programming Language
Contributors"; so is this repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_duktape.c` | `contrib/host/duktape/aether_host_duktape.c` | see below |
| `aether_host_duktape.h` | `contrib/host/duktape/aether_host_duktape.h` | no |
| `module.ae` | `contrib/host/duktape/module.ae` | `@source("aether_host_duktape.c")`; header comment |
| `test_host_duktape.ae` | `contrib/host/duktape/test_host_duktape.ae` | `import duktape` instead of `import contrib.host.duktape`; the `describe` name; the first comment lines |
| `test_host_duktape_vm.ae` | `tests/integration/host_duktape_bidirectional/test_host_duktape_bidirectional.sh` | the C harness as an Aether spec, with `@c_callback` host functions: the same cases (dotted registration, persistent state, non-ASCII round trip, bool, nil, string list, caught and uncaught errors, the limit message) are in it and in `test_host_duktape.ae`, which already had them; this adds the callback-side readers, `push_double`, and the bindings `run_sandboxed` gives a script |
| `duktape_abi.h` | (new) | the Duktape types and constants the bridge uses |
| `../js/module.ae` | `contrib/host/js/module.ae` | wrappers over `import duktape` instead of `@extern("duktape_...")` bindings to the same C (see below) |
| `../js/README.md` | `contrib/host/js/README.md` | rewritten for the alias here |
| `../js/test_js_alias.ae` | (new) | the alias reaches the bridge |
| `containment/`, `worker/` | (new) | modelled on `../lua/containment` and `../lua/worker` |
| `examples/` | (new) | the programs in `README.md`'s four ways |

Not brought over: the shared-map case of `tests/sandbox/test_shared_map_all.sh`
(JS reads `name` and `count`, writes `doubled`, tries to tamper with `name`),
which is the containment rows `map-get`, `map-set` and `map-overwrite-input`
here (`containment/checks.js`), run in every mode that applies; and the
`make contrib` smoke test's `duktape` entry, which compiled the bridge archive
against the dev headers: the bridge is compiled into the importing program
now.

## What the move changed in `aether_host_duktape.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_duktape.a`, with the Duktape dev package's headers found by
pkg-config and `-DAETHER_HAS_DUKTAPE -DAETHER_HAS_SANDBOX` on the command
line. Here it is compiled into each program that imports it (`@source` in
`module.ae`), with the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/aether_sandbox.h"` (and `aether_shared_map.h`)
  became `#include "aether_sandbox.h"`: the installed runtime's headers,
  already on that include path.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_DUKTAPE` are defined in the file when
  the build does not define them (the `#ifdef` on the latter became `#if`).
- `<duktape.h>` was replaced by `duktape_abi.h`, which declares the Duktape
  types and constants the bridge uses. The bridge never linked against
  libduktape (it uses `dlopen` and `dlsym`), so the Duktape dev package is no
  longer needed to build a program; the Duktape runtime is still needed to
  run one.
- libduktape is found by `AETHER_DUKTAPE_SONAME`, then by the names
  `libduktape.so.207`, `libduktape.so.2`, `libduktape.so`, and the
  `.dylib` and `.so` that Homebrew installs under `/opt/homebrew/lib` and
  `/usr/local/lib`. In aether it was `AETHER_DUKTAPE_SONAME` and then
  `libduktape.so` only ("the orchestrator owns the probe"); the list is
  there so the usual installs work without one, and `../tests/duktape_env.sh`
  still makes the probe for the tests.
- `dlerror()` is read once when the library cannot be loaded. The old message
  called it twice in one expression, and the second call gives NULL, which
  the format printed with `%s`. (The same fix is in `../lua` and `../python`.)
- the "not available" message no longer says `contrib.host.duktape`.

Nothing else in the file changed in the move.

## The `js` alias

aether's `contrib/host/js/module.ae` bound `js_*` names to the C symbols
`duktape_*` with `@extern`, relying on `ae build`'s import-driven auto-link to
find `libaether_host_duktape.a` for `js`. There is no archive here and no
auto-link: `js/module.ae` imports `duktape` and each `js_*` function calls
the `duktape_*` function of the same name. Both names are one bridge; use
either, not a mix for the same VM's state.

aether keeps its copy for a transition period; see the top-level README.

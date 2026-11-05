# Provenance

The QuickJS bridge was copied from aether's `contrib/host/quickjs/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, the v0.889.0 release merge,
2026-11-05; the directory last changed at `7b4525cc9`), with the parts of
aether's tests that exercise it. aether is MIT licensed, copyright "Aether
Programming Language Contributors"; so is this repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_quickjs.c` | `contrib/host/quickjs/aether_host_quickjs.c` | see below |
| `aether_host_quickjs.h` | `contrib/host/quickjs/aether_host_quickjs.h` | no |
| `module.ae` | `contrib/host/quickjs/module.ae` | `@source("aether_host_quickjs.c")`; header comment |
| `test_host_quickjs.ae` | `contrib/host/quickjs/test_host_quickjs.ae` | `import quickjs` instead of `import contrib.host.quickjs`; the `describe` name; the first comment lines; `/etc/hosts` for `/etc/hostname`, which macOS has not |
| `test_quickjs_bridge.ae` | (new) | `print`, `console.log`, no `quickjs-libc` names, ES2023 |
| `containment/`, `worker/` | (new) | modelled on `../duktape/containment` and `../duktape/worker`, with aether's engine module in the worker instead of a dlopen'd library |
| `examples/` | (new) | the programs in `README.md`'s four ways |

Not brought over: aether's README for the bridge (replaced by `README.md`
here), and the shared-map case of `tests/sandbox/test_shared_map_all.sh`
(JS reads `name` and `count`, writes `doubled`, tries to tamper with `name`),
which is the containment rows `map-get`, `map-set` and `map-overwrite-input`
here (`containment/checks.js`), run in every mode that applies.

## The engine: aether's, not a copy

The bridge needs QuickJS, and aether has it already: the engine module
`contrib/quickjs/` (`module.ae`, `aether_quickjs.c`, the pinned amalgamation
`amalgamation.lock`: quickjs-ng 0.17.0, SHA-256
`a0955463c74809173a253ff87365095e6972e8b171cfb6aa8a1e781adf35cfeb`). It stays
in aether (sae and others use it), and every release archive ships it,
amalgamation included, under `share/aether/contrib/quickjs/` (present in
every release installed here from 0.778.0 to 0.889.0). So `module.ae` keeps
what aether's did, `import contrib.quickjs as qjs_engine`: importing the
engine is what compiles its C into the program, and the bridge's C calls it.

This was chosen over vendoring the amalgamation into this repository
because that would put a second copy of QuickJS, and a second pin to keep
current, beside aether's, for no gain: a release install provides the engine
to a `@source` bridge, and the specs and the containment tests run against
it. The pin that applies is therefore whatever the `ae` in use ships (0.17.0
in 0.889.0); it is not repeated in `ci/versions.env`.

Keeping in step with aether: the bridge's C declares the engine's `qjs_*`
functions with `extern` (there is no header shared with the engine), and
the closure passed to `qjs_function` has the compiler's `{fn, env}` layout.
If a release changes those signatures, the specs fail to build or run;
update the declarations in `aether_host_quickjs.c` and `worker/worker.ae`
(which uses the engine's Aether API) in the commit that raises `AE_PIN`. A
bump of the engine's version alone needs nothing here, but the README's
mention of 0.17.0 should follow it.

## What the move changed in `aether_host_quickjs.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_quickjs.a`, with `-DAETHER_HAS_QUICKJS` on the command line.
Here it is compiled into each program that imports it (`@source` in
`module.ae`), with the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/aether_sandbox.h"` (and `aether_shared_map.h`)
  became `#include "aether_sandbox.h"`: the installed runtime's headers,
  already on that include path.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_QUICKJS` are defined in the file when
  the build does not define them (the `#ifdef` on the latter became `#if`).
- **`print` and `console.log` are new.** aether's scripts had no way to say
  anything: they could compute and write to the shared map. Every run now
  installs a `print` (and `console.log`) that writes its space-joined
  arguments and a newline to stdout, so a script can be seen to run (the
  README's examples) and the worker's `SAY` has a counterpart in-process.
  They check nothing and reach nothing but stdout. This is the only change of
  behaviour; the three entry points, their return codes, the 32 MB heap cap,
  the 5000 ms limit per run and the grants handling are as in aether.

Nothing else in the file changed in the move.

aether keeps its copy for a transition period; see the top-level README.

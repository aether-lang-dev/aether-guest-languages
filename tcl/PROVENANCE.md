# Provenance

The Tcl bridge was copied from aether's `contrib/host/tcl/` as it stands on
`main` at commit `2609ec841` (aether-lang-dev/aether, the v0.889.0 release
merge, 2026-11-05; the directory last changed at `3b8d8fb87`), with the
parts of aether's tests that exercise it. aether is MIT licensed, copyright
"Aether Programming Language Contributors"; so is this repository
(`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_tcl.c` | `contrib/host/tcl/aether_host_tcl.c` | see below |
| `aether_host_tcl.h` | `contrib/host/tcl/aether_host_tcl.h` | no |
| `module.ae` | `contrib/host/tcl/module.ae` | `@source("aether_host_tcl.c")`; header comment; `run_sandboxed_with_map` is exported |
| `test_host_tcl.ae` | `contrib/host/tcl/test_host_tcl.ae` | `import tcl` instead of `import contrib.host.tcl`; the `describe` name; the first comment lines |
| `test_host_tcl_vm.ae` | `tests/integration/host_tcl_bidirectional/test_host_tcl_bidirectional.sh` | the C harness as an Aether spec, with `@c_callback` host functions: the same cases and assertions |
| `test_support.c` | the same harness | the C that harness did inline that an Aether spec cannot: a VM used from a second thread, the resident size, the int64 limits |
| `tcl_abi.h` | (new) | the Tcl types and constants the bridge uses |
| `containment/`, `worker/` | (new) | modelled on `../lua/containment` and `../python/containment`, `../lua/worker` |

Not brought over: aether's test of the bridge through the `contrib/host`
build (`tests/integration/contrib_host_*`, which compile the bridge against
the system's `tcl.h`); the examples and trim tests that use
`contrib.host.tcl` as a fixture stay in aether, which keeps its copy for a
transition period.

## What the move changed in `aether_host_tcl.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_tcl.a`, with the Tcl dev package's `tcl.h` found by
pkg-config and `-DAETHER_HAS_TCL -DAETHER_HAS_SANDBOX` on the command line.
Here it is compiled into each program that imports it (`@source` in
`module.ae`), with the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/aether_sandbox.h"` (and
  `aether_shared_map.h`) became `#include "aether_sandbox.h"`: the
  installed runtime's headers, already on that include path.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_TCL` are defined in the file when
  the build does not define them.
- `<tcl.h>` was replaced by `tcl_abi.h`, which declares the few Tcl types
  and constants the bridge needs. The bridge never linked against libtcl
  (it uses `dlopen` and `dlsym`), so the Tcl dev package is no longer needed
  to build a program; the Tcl runtime library is still needed to run one.
  The `#undef Tcl_GetStringResult / Tcl_GetString / Tcl_Eval` workaround for
  Tcl 9's macros went with `<tcl.h>`: the helpers over the three exports
  that exist in every Tcl stay.
- The widths. With the real header, `Tcl_Size` was `int` (Tcl 8) or
  `ptrdiff_t` (Tcl 9) according to the header the bridge was compiled
  against, which is the build host's, not the one dlopen finds. Now the
  prototypes of the dlsym'd functions declare `ptrdiff_t` always, which an
  8.x callee reads as the int it takes (`tcl_abi.h` says why that holds on
  LP64 and ILP32), and the two command procedures take their `objc` as a
  `ptrdiff_t` and keep its low 32 bits. One build serves 8.5, 8.6 and 9.0.
- `Tcl_Obj` is the four words `tcl_abi.h` declares, replacing the
  `_Static_assert` on the real struct's `typePtr` offset.
- `Tcl_GetVersion` is looked up at load, and the library is refused if it is
  older than 8.5 or newer than 9.x.
- Loading. aether looked at `$AETHER_TCL_SONAME`, then `libtcl.so` (only the
  dev package has that name). Here, when the variable is unset, the bridge
  tries `libtcl8.6.so.0`, `libtcl8.6.so`, `libtcl9.0.so.0`, `libtcl9.0.so`,
  `libtcl.so`, Homebrew's tcl-tk libraries and `/usr/lib/libtcl.dylib`;
  `dlerror()` is read once.
- The message of the stub built with `AETHER_HAS_TCL=0` names this module.

`module.ae` exports `tcl_run_sandboxed_with_map`, which the C file and
`aether_host_tcl.h` always had and aether's `module.ae` did not (the Lua and
Python modules do): the containment host uses it for the shared-map rows.

Nothing else in the file changed in the move. Later changes made here are
in this repository's history (`git log -- tcl/`).

aether keeps its copy for a transition period; see the top-level README.

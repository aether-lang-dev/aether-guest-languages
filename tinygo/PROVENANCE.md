# Provenance

The Go bridge was copied from aether's `contrib/host/tinygo/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, 2026-11-05; the directory's last
change before it was `0a41ce009`, 2026-10-21), with the part of aether's
tests that exercises it. aether is MIT licensed, copyright "Aether
Programming Language Contributors"; so is this repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_tinygo.c` | `contrib/host/tinygo/aether_host_tinygo.c` | the three `#include "../../../..."` lines became the installed runtime's headers; the file's header comment |
| `aether_host_tinygo.h` | `contrib/host/tinygo/aether_host_tinygo.h` | the first comment line only |
| `module.ae` | `contrib/host/tinygo/module.ae` | `@source("aether_host_tinygo.c")`; the header comment (the new name, the toolchain note, what the bridge does not check) |
| `examples/greet.go` | `contrib/host/tinygo/examples/greet.go` | the comment's build line and import |
| `test_host_tinygo.ae` | `tests/integration/host_tinygo/uses_tinygo.ae`, `test_host_tinygo.sh` | `import tinygo`; the library's path from `$TINYGO_LIB`; the same four lines checked by the program itself rather than by `grep`; an unresolved symbol and a missing library added |
| `test_host_tinygo_owned.ae`, `test_support/fake_cshared.c` | `tests/integration/host_tinygo/uses_owned.ae`, `fake_cshared.c`, `test_host_tinygo_owned.sh` | `import tinygo`; the library's path from `$TINYGO_FAKE_LIB`, which `../tests/tinygo_env.sh` builds with the C compiler |
| `examples/probe.go`, `examples/way*.ae` | (new) | the programs in README.md's four ways |
| `containment/`, `worker/` | (new) | modelled on `../python/containment` and `../python/worker` |
| `../tests/tinygo_env.sh` | (new) | finds the Go toolchain and builds the c-shared libraries the tests load |

Not brought over: `README.md` (rewritten for this repository, keeping aether's
toolchain note, the calling-convention table, "Memory ownership" and the
limitations), and `tests/integration/host_tinygo/test_host_tinygo.sh` and
`test_host_tinygo_owned.sh` as shell scripts (the build and the checks are
`../tests/tinygo_env.sh` and the two `.ae` programs).

## What the move changed in `aether_host_tinygo.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_tinygo.a` (with `-DAETHER_HAS_LIBFFI` when libffi is found).
Here it is compiled into each program that imports it (`@source` in
`module.ae`), with the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/utils/aether_compiler.h"`,
  `"../../../std/dl/aether_dl.h"` and `"../../../std/string/aether_string.h"`
  became `#include "aether_compiler.h"` and so on: the installed runtime's
  headers, already on that include path.
- `call_dynamic` is the stub: nothing here defines `AETHER_HAS_LIBFFI`, so it
  returns 0 and `last_error()` says libffi is unavailable, as aether's does
  when libffi was not found. (Linking libffi into every importing program
  would be a dependency this repository does not otherwise have.)

Nothing else in the file changed in the move, and there are no behaviour
changes beyond the above: the bridge still checks no grant itself and has no
sandboxed entry point (README.md, "Four ways"). Later changes made here are
in this repository's history (`git log -- tinygo/`).

## What is new

Everything under `containment/` and `worker/`:

- `containment/checks.go`: stunt's rows in Go, built into a c-shared
  library, each an exported `check_<row>`; the rows the bridge cannot carry
  (host functions, the shared map) are left out in `containment/lang.sh`.
- `worker/worker.ae`: a std.guest worker that loads the library before its
  lockdown and calls its exported functions, on a thread of its own so that a
  `CANCEL` can be heard.
- `tests/stunt_lib.sh` gained two hooks for a language the bridge of which has
  no host functions: `LANG_PROBE` (the row that shows a contained child can
  run) and `child_escapes` (a row the child lets through where the policy
  refuses it, run expecting success).

# Provenance

The Python bridge was copied from aether's `contrib/host/python/` at commit
`9673641e6` (aether-lang-dev/aether, `main`, the v0.884.0 release merge,
2026-11-05), with the parts of aether's tests that exercise it. aether is
MIT licensed, copyright "Aether Programming Language Contributors"; so is
this repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_python.c` | `contrib/host/python/aether_host_python.c` | see below |
| `aether_host_python.h` | `contrib/host/python/aether_host_python.h` | no |
| `module.ae` | `contrib/host/python/module.ae` | `@source("aether_host_python.c")`; header comment |
| `test_host_python.ae` | `contrib/host/python/test_host_python.ae` | `import python` instead of `import contrib.host.python`; the `describe` name; the first comment line |
| `test_host_python_vm.ae` | `tests/integration/host_python_bidirectional/test_host_python_bidirectional.sh` and `tests/integration/host_python_map_context/test_host_python_map_context.sh` | the two C harnesses as one Aether spec, with `@c_callback` host functions: the same cases and assertions |
| `test_support.c` | the same two harnesses | the C those harnesses did inline that an Aether spec cannot: maps by token, two threads, the resident size, the bridge's own address for the nested map case |
| `python_abi.h` | (new) | the CPython types and constants the bridge uses |
| `containment/`, `worker/` | (new) | modelled on `../lua/containment` and `../lua/worker` |

Not brought over: `tests/integration/namespace_python/` (it tests `ae build
--namespace`, which generates a Python SDK for an Aether library: the other
direction, and the compiler's, not this bridge's) and
`tests/sandbox/test_dlopen_python.py` (a script that tries ctypes on libc
under the preload, run by aether's sandbox suite: its cases are the
containment rows `getenv-home`, `dlopen-libc-by-name` and
`dlopen-libc-by-path` here, `containment/checks.py`). The two harnesses'
`libpython.supp` (LeakSanitizer suppressions for CPython's own allocations)
are not needed: nothing here runs under ASan.

## What the move changed in `aether_host_python.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_python.a`, with the headers of the Python dev package found
by python3-config and `-DAETHER_HAS_PYTHON -DAETHER_HAS_SANDBOX` on the
command line. Here it is compiled into each program that imports it
(`@source` in `module.ae`), with the cflags `ae build` uses for any program.
So:

- `#include "../../../runtime/aether_sandbox.h"` (and
  `aether_shared_map.h`, `std/string/aether_string.h`,
  `std/bytes/aether_bytes.h`) became `#include "aether_sandbox.h"` and so
  on: the installed runtime's headers, already on that include path.
  `#include "../aep_dl.h"` (contrib/host's dlopen shim, which maps the
  POSIX names onto Win32) became `#include <dlfcn.h>`, as the Lua bridge
  has it: neither bridge builds for Windows here.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_PYTHON` are defined in the file when
  the build does not define them.
- `<Python.h>` was replaced by `python_abi.h`, which declares the few
  CPython types the bridge needs, and says why each is the same from 3.10
  to 3.14. The bridge never linked against libpython (it uses `dlopen` and
  `dlsym`), so the Python dev package is no longer needed to build a
  program; the Python runtime is still needed to run one.
- The `_aether_map` module (`aether_map_get`, `aether_map_put`) is made with
  `PyModule_New` and a `PyCFunction_NewEx` per function instead of
  `PyModule_Create2` from a `PyModuleDef`. A `PyModuleDef` begins with
  `PyObject_HEAD`, whose size and initial values depended on the Python.h
  the bridge was compiled with (immortal refcounts from 3.12, another
  header in the free-threaded build), while it is read by the libpython it
  runs against; `PYTHON_API_VERSION` came from the build host's headers
  too. `PyModule_Create2` is no longer looked up.
- A free-threaded libpython (3.13t, 3.14t) is refused at load with a
  message: it exports `_Py_DecRefShared`, which the default build does not
  (`python_abi.h` says why it is refused).
- The message of the stub built with `AETHER_HAS_PYTHON=0` names this
  module.

Nothing else in the file changed in the move. Later changes made here are
in this repository's history (`git log -- python/`).

aether keeps its copy for a transition period; see the top-level README.

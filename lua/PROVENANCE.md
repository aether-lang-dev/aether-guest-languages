# Provenance

The Lua bridge was copied from aether's `contrib/host/lua/` at commit
`d3732a0ed` (aether-lang-dev/aether, `main`, the v0.877.0 release merge,
2026-11-04). aether is MIT licensed, copyright "Aether Programming
Language Contributors"; so is this repository (`../LICENSE`).

| Here | From aether `contrib/host/lua/` | Changed |
|---|---|---|
| `aether_host_lua.c` | `aether_host_lua.c` | see below |
| `aether_host_lua.h` | `aether_host_lua.h` | no |
| `module.ae` | `module.ae` | `@source("aether_host_lua.c")`; header comment |
| `test_host_lua.ae` | `test_host_lua.ae` | `import lua` instead of `import contrib.host.lua` |
| `lua_abi.h` | (new) | the Lua types and constants the bridge uses |

## What the move changed in `aether_host_lua.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_lua.a`, with the Lua dev package's headers found by
pkg-config and `-DAETHER_HAS_LUA -DAETHER_HAS_SANDBOX` on the command line.
Here it is compiled into each program that imports it (`@source` in
`module.ae`), with the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/aether_sandbox.h"` (and `aether_shared_map.h`)
  became `#include "aether_sandbox.h"`: the installed runtime's headers,
  already on that include path.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_LUA` are defined in the file when
  the build does not define them.
- `<lua.h>`, `<lualib.h>` and `<lauxlib.h>` were replaced by `lua_abi.h`,
  which declares the few Lua types the bridge needs. The bridge never
  linked against liblua (it uses `dlopen` and `dlsym`), so the Lua dev
  package is no longer needed to build a program; the Lua runtime is still
  needed to run one. The `#undef luaL_openlibs` workaround went with
  `<lualib.h>`.

Nothing else in the file changed in the move. Later changes made here are
in this repository's history (`git log -- lua/`), starting with Lua 5.5
support.

aether keeps its copy for a transition period; see the top-level README.

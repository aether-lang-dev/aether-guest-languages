# aether-guest-languages

Other languages run from [Aether](https://github.com/aether-lang-dev/aether)
programs: the bridges that were `contrib/host/<lang>` in aether's tree, and
more, each held to the same named checks.

A language here can be run four ways, and the repository does not choose
for you. Every way stays supported; they differ in how much they contain
([lua/README.md](lua/README.md#four-ways-to-run-lua-from-aether) shows all
four for Lua, with examples):

1. **Embed.** The interpreter is loaded into the Aether program and called
   directly (`import lua`, then `lua.run(...)`, the script often a heredoc,
   or the two-way VM). The easy default: one binary, direct calls both
   ways, no containment.
2. **Embed inside a sandbox block.** The same, under a grant list
   (`lua.run_sandboxed(perms, ...)`): std's calls and the host functions
   you give the guest are checked against it. The interpreter's own libc
   calls are not.
3. **Embed under `LD_PRELOAD`.** Way 2 with `libaether_sandbox.so`
   preloaded (Linux), which holds the interpreter's own libc calls to the
   grants too. A library interposer, not a kernel boundary.
4. **Contained child.** The interpreter runs in a worker process that a
   host starts with `std.guest`'s `guest.launch` (`sandbox.spawn_socket`:
   Landlock and seccomp from the grant list, plus the preload). The worker
   locks itself down before the script's first step, and every effect the
   script has crosses a socket (`std.ipc` frames) the host answers. It
   needs Linux with Landlock; on macOS and Windows `guest.launch` starts
   nothing and says why.

Each language's README carries a **containment table**: for every row of
stunt's catalogue (aether `contrib/host/stunt`, the test double for a
hosted language), what that language's own way of doing it gets in each
mode: refused, allowed per the policy, not held, or left out because the
language cannot express it. The tests in `tests/` produce the table, so a
claim in it is a test outcome. That a language runs is not the question the
table answers; whether it is contained is.

| Language | In-process | Contained child | Table |
|---|---|---|---|
| Lua 5.3, 5.4, 5.5 | `import lua` | `lua/worker` (std.guest) | [lua/README.md](lua/README.md#containment-table) |

## Layout

```
aether.toml            the package: exports lua/ (modules = "lua")
ci/versions.env        toolchain pins (AE_PIN, AE_CHILD_PIN, AEB_REF, AETHER_STUNT_REF)
lua/
  module.ae            import lua: the bridge's API
  aether_host_lua.c    the bridge, compiled into the importing program (@source)
  lua_abi.h            the Lua types the bridge uses (no Lua dev package needed)
  test_host_lua*.ae    the bridge's specs
  worker/              lua-worker: Lua in a contained child (std.guest)
  containment/         the containment hosts and checks.lua, stunt's rows in Lua
  PROVENANCE.md        where the bridge came from, and what the move changed
tests/
  run.sh               everything
  stunt_lib.sh         holds a language to stunt's catalogue, by name
  test_stunt_*.sh      one per policy: aether's contrib_host_stunt lists
  test_catalogue.sh    every catalogue row is named here or left out
```

## Using a language from this repository

The repository is an Aether package. In your project:

```sh
ae add github.com/aether-lang-dev/aether-guest-languages
```

fetches the default branch and records it in `aether.toml` with the hash of
the tree it installed (there are no tagged releases yet):

```toml
[dependencies]
"github.com/aether-lang-dev/aether-guest-languages" = "latest"

[dependency-hashes]
"github.com/aether-lang-dev/aether-guest-languages" = "sha256-..."
```

Then `import lua` resolves to `lua/module.ae`:
`ae build`, `ae run` and `ae lib-path` put the package on the module search
path, and the bridge's C is compiled into your program from the `@source`
line in `module.ae`, with nothing to build beforehand.

Against a local checkout instead (developing a bridge, or CI proving a
branch):

```toml
[patch]
"github.com/aether-lang-dev/aether-guest-languages" = "../aether-guest-languages"
```

`ae build` then prints `Overriding ... -> ../aether-guest-languages`. For a
one-off without a manifest, `ae build app.ae --lib <checkout>` does the
same, since the checkout's root holds `lua/module.ae`.

At run time the bridge `dlopen`s the language's library: for Lua, set
`AETHER_LUA_SONAME` (for example `liblua5.4.so.0`, or
`/opt/homebrew/lib/liblua.dylib`) unless the system has an unversioned
`liblua.so`. The Lua dev package is not needed to build.

## Toolchain

`ae` (and `aeb`, which nothing here needs yet) from aeb's `get.sh`, pinned
as `ci/versions.env` says. One line installs both into `~/.local` (no sudo;
`PREFIX=` to change it):

```sh
curl -fsSL https://raw.githubusercontent.com/aether-lang-dev/aeb/v0.340/get.sh \
  | AE_PIN=0.875.0 AEB_REF=v0.340 sh
```

That is enough for the bridges and the in-process tests. The contained
child needs `std.guest`, first published in ae 0.878.0 (`AE_CHILD_PIN`):

```sh
curl -fsSL https://raw.githubusercontent.com/aether-lang-dev/aeb/v0.340/get.sh \
  | AE_PIN=0.878.0 AEB_REF=v0.340 sh
```

The pins follow the rule used across the Aether family: a pin is the oldest
version this repository has been verified against, and moves only in the
commit that needs something newer, saying so.

## Tests

```sh
sh tests/run.sh               # specs, then the containment rows
sh tests/run.sh containment   # the rows only
LUA_VERSION=5.4 sh tests/run.sh
```

The containment rows build stunt from aether's source at
`AETHER_STUNT_REF` (three files, fetched once into `build/`; or set
`AETHER_SRC` to an aether checkout) to read its catalogue: release archives
ship `contrib/host/stunt` without `stunt.c` and `stunt_main.c`. On Linux
the in-process rows run under the release's `libaether_sandbox.so`; the
child rows run where ae is 0.878.0 or later and the kernel has Landlock
(`GHL_REQUIRE_CHILD=1` makes a missing child mode a failure, as CI does).

CI (`.github/workflows/ci.yml`): Linux with Ubuntu's liblua5.4-dev on
`AE_PIN` (in-process) and on `AE_CHILD_PIN` (in-process and contained
child), and macOS with Homebrew's lua (5.5) and lua@5.4 on `AE_PIN`.

## Moving from `contrib.host.lua`

The bridge was copied from aether at `d3732a0ed` (lua/PROVENANCE.md). Its
functions and names are unchanged, so moving is the import line and the
dependency:

```diff
-import contrib.host.lua
+import lua
```

plus the dependency (`ae add`, above). What changes for the better: no
`make contrib MODULES=lua` / `make install-contrib` step and no Lua dev
package at build time (a release install of ae could not link
`contrib.host.lua` without them), and Lua 5.5 works (aether's copy refuses
to load it: it looks for `luaL_openlibs`, which 5.5 does not export).

The plan for aether's copy: it stays in `contrib/host/lua` for a transition
period, with its README pointing here; fixes land here first. It is removed
from aether in a later release, announced in aether's changelog beforehand.
Do not import both in one program: they define the same C symbols.

## License

MIT, as aether: see [LICENSE](LICENSE).

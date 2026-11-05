# aether-guest-languages

Other languages run from [Aether](https://github.com/aether-lang-dev/aether)
programs: the bridges that were `contrib/host/<lang>` in aether's tree, and
more, each held to the same named checks.

A language here can be run four ways, and the repository does not choose
for you. Every way stays supported; they differ in how much they contain
([lua/README.md](lua/README.md#four-ways-to-run-lua-from-aether),
[python/README.md](python/README.md#four-ways-to-run-python-from-aether) and
[tcl/README.md](tcl/README.md#four-ways-to-run-tcl-from-aether)
show all four, with examples):

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

Above it, a **Stunt parity** table gives one row per stunt test (the
`tests/test_stunt_*.sh` scripts, named after aether's) and one cell per way:
"matches", or the rows that differ and how. It is generated from a run
(`sh tests/run.sh --write-parity`), and every run checks it in the columns
its host can run, so a README that no longer says what the tests do fails
the tests.

| Language | In-process | Contained child | Table |
|---|---|---|---|
| Lua 5.3, 5.4, 5.5 | `import lua` | `lua/worker` (std.guest) | [lua/README.md](lua/README.md#containment-table) |
| CPython 3.10 to 3.14 | `import python` | `python/worker` (std.guest) | [python/README.md](python/README.md#containment-table) |
| Tcl 8.5, 8.6 (9.0 in principle) | `import tcl` | `tcl/worker` (std.guest) | [tcl/README.md](tcl/README.md#containment-table) |

## Layout

```
aether.toml            the package: exports lua/, python/ and tcl/ (modules = "lua, python, tcl")
ci/versions.env        toolchain pins (AE_PIN, AE_CHILD_PIN, AE_SPAWN_PIN, AEB_REF, AETHER_STUNT_REF)
lua/
  module.ae            import lua: the bridge's API
  aether_host_lua.c    the bridge, compiled into the importing program (@source)
  lua_abi.h            the Lua types the bridge uses (no Lua dev package needed)
  test_host_lua*.ae    the bridge's specs
  worker/              lua-worker: Lua in a contained child (std.guest)
  containment/         the containment hosts and checks.lua, stunt's rows in Lua;
                       lang.sh, what the harness needs to know about Lua
  PROVENANCE.md        where the bridge came from, and what the move changed
python/
  module.ae            import python: the bridge's API
  aether_host_python.c the bridge, compiled into the importing program (@source)
  python_abi.h         the CPython types the bridge uses (no Python dev package needed)
  test_host_python*.ae the bridge's specs (test_support.c: their C helpers)
  worker/              python-worker: CPython in a contained child (std.guest)
  containment/         the containment hosts and checks.py, stunt's rows in Python;
                       lang.sh; test_imports.sh, importing under a sandbox
  PROVENANCE.md        where the bridge came from, and what the move changed
tcl/
  module.ae            import tcl: the bridge's API
  aether_host_tcl.c    the bridge, compiled into the importing program (@source)
  tcl_abi.h            the Tcl types the bridge uses (no Tcl dev package needed)
  test_host_tcl*.ae    the bridge's specs (test_support.c: their C helpers)
  worker/              tcl-worker: Tcl in a contained child (std.guest)
  containment/         the containment hosts and checks.tcl, stunt's rows in Tcl;
                       lang.sh, what the harness needs to know about Tcl
  PROVENANCE.md        where the bridge came from, and what the move changed
tests/
  run.sh               everything, every language (GUEST_LANG=<lang> for one)
  stunt_lib.sh         holds a language to stunt's catalogue, by name
  test_stunt_*.sh      one per policy: aether's contrib_host_stunt lists
  test_catalogue.sh    every catalogue row is named here or left out
  parity.sh            renders and checks a README's Stunt parity table
  lua_env.sh, python_env.sh, tcl_env.sh  find the runtime library on this machine
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

Then `import lua` resolves to `lua/module.ae`, `import python` to
`python/module.ae` and `import tcl` to `tcl/module.ae`:
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
same, since the checkout's root holds `lua/module.ae`, `python/module.ae`
and `tcl/module.ae`.

At run time the bridge `dlopen`s the language's library: for Lua, set
`AETHER_LUA_SONAME` (for example `liblua5.4.so.0`, or
`/opt/homebrew/lib/liblua.dylib`) unless the system has an unversioned
`liblua.so`; for Python, `AETHER_PYTHON_SONAME` (for example
`libpython3.12.so.1.0`) unless it has `libpython3.so`; for Tcl,
`AETHER_TCL_SONAME` (for example `libtcl8.6.so.0`), unless one of the names
tcl/README.md lists loads (Debian's `libtcl8.6.so.0`, Homebrew's tcl-tk,
macOS's own Tcl). No language's dev package is needed to build.

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
sh tests/run.sh                    # every language: specs, containment rows, parity check
sh tests/run.sh containment        # the rows only
GUEST_LANG=python sh tests/run.sh  # one language
GUEST_LANG=tcl TCL_VERSION=8.6 sh tests/run.sh
LUA_VERSION=5.4 GUEST_LANG=lua sh tests/run.sh
PYTHON=/usr/bin/python3.12 GUEST_LANG=python sh tests/run.sh
sh tests/run.sh --write-parity     # rewrite the parity tables' columns this host ran
```

The containment rows build stunt from aether's source at
`AETHER_STUNT_REF` (three files, fetched once into `build/`; or set
`AETHER_SRC` to an aether checkout) to read its catalogue: release archives
ship `contrib/host/stunt` without `stunt.c` and `stunt_main.c`. On Linux
the in-process rows run under the release's `libaether_sandbox.so`; the
child rows run where ae is 0.878.0 or later and the kernel has Landlock
(`GHL_REQUIRE_CHILD=1` makes a missing child mode a failure, as CI does).

CI (`.github/workflows/ci.yml`), one language per job: Linux with Ubuntu's
liblua5.4-dev, with its libpython3.12 and with its tcl8.6 (the runtime
library; no tcl-dev), each on `AE_PIN` (in-process), on `AE_CHILD_PIN`
(in-process and contained child) and on `AE_SPAWN_PIN`; macOS with
Homebrew's lua (5.5) and lua@5.4, its python@3.13 and its tcl-tk, on
`AE_PIN`.

## Moving from `contrib.host.lua`, `contrib.host.python` or `contrib.host.tcl`

The Lua bridge was copied from aether at `d3732a0ed` (lua/PROVENANCE.md).
Its functions and names are unchanged, so moving is the import line and the
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

The Python bridge was copied from aether at `9673641e6`
(python/PROVENANCE.md), and moves the same way, the import line and the
dependency:

```diff
-import contrib.host.python
+import python
```

What changes: no `make contrib MODULES=python` step and no python3-dev at
build time; the `_aether_map` module no longer depends on the build host's
Python.h; a free-threaded CPython is refused with a message rather than
loaded.

The Tcl bridge was copied from aether at `2609ec841`
(tcl/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.tcl
+import tcl
```

What changes: no `make contrib MODULES=tcl` step and no tcl-dev at build
time; one build runs against Tcl 8.5, 8.6 and 9.0 whatever `tcl.h` the build
host had; libtcl is found by name on Debian, Homebrew and macOS without
`AETHER_TCL_SONAME`; and `tcl.run_sandboxed_with_map` is exported.

The plan for aether's copies: they stay in `contrib/host/` for a transition
period, with their READMEs pointing here; fixes land here first. They are
removed from aether in a later release, announced in aether's changelog
beforehand. Do not import both copies of a language in one program: they
define the same C symbols.

## License

MIT, as aether: see [LICENSE](LICENSE).

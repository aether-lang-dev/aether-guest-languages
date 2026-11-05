# aether-guest-languages

Other languages run from [Aether](https://github.com/aether-lang-dev/aether)
programs: the bridges that were `contrib/host/<lang>` in aether's tree, and
more, each held to the same named checks.

A language here can be run four ways, and the repository does not choose
for you. Every way stays supported; they differ in how much they contain
([lua/README.md](lua/README.md#four-ways-to-run-lua-from-aether),
[python/README.md](python/README.md#four-ways-to-run-python-from-aether),
[duktape/README.md](duktape/README.md#four-ways-to-run-javascript-from-aether),
[quickjs/README.md](quickjs/README.md#four-ways-to-run-javascript-from-aether),
[tcl/README.md](tcl/README.md#four-ways-to-run-tcl-from-aether),
[perl/README.md](perl/README.md#four-ways-to-run-perl-from-aether),
[ruby/README.md](ruby/README.md#four-ways-to-run-ruby-from-aether),
[tinygo/README.md](tinygo/README.md#four-ways-to-run-go-from-aether),
[go/README.md](go/README.md#four-ways-to-run-go-from-aether) and
[java/README.md](java/README.md#four-ways-to-run-java-from-aether)
each show all four, with examples; Go and Java as child processes are never in your process, so their
first three ways are a child process with less and more around it; and a JVM does not start inside
ways 2 to 4 on Linux today, which java/README.md explains):

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
| JavaScript, Duktape 2.x (ES5.1) | `import duktape` (`import js`, the alias) | `duktape/worker` (std.guest) | [duktape/README.md](duktape/README.md#containment-table) |
| JavaScript, QuickJS (quickjs-ng 0.17.0, ES2023) | `import quickjs` | `quickjs/worker` (std.guest) | [quickjs/README.md](quickjs/README.md#containment-table) |
| Tcl 8.5, 8.6 (9.0 in principle) | `import tcl` | `tcl/worker` (std.guest) | [tcl/README.md](tcl/README.md#containment-table) |
| Perl 5 (threaded), 5.34 to 5.44 tested | `import perl` | `perl/worker` (std.guest) | [perl/README.md](perl/README.md#containment-table) |
| CRuby 2.6 to 3.4 (checked) | `import ruby` | `ruby/worker` (std.guest) | [ruby/README.md](ruby/README.md#containment-table) |
| Go 1.21 or later, as a c-shared library (compiled; `go build -buildmode=c-shared`) | `import tinygo` | `tinygo/worker` (std.guest) | [tinygo/README.md](tinygo/README.md#containment-table) |
| Go 1.18 or later (checked 1.22, 1.27), as a child process | `import go` (starts a program; no in-process way) | `go/worker` (std.guest, Go) | [go/README.md](go/README.md#containment-table) |
| Java 21 or later (checked 21, 25), as a JVM child process | `import java` (starts a JVM; no in-process way) | `java/worker` (std.guest, Java); the JVM does not start inside the preload yet | [java/README.md](java/README.md#containment-table) |

## Layout

```
aether.toml            the package: exports every language directory (modules = "lua, python, duktape, js, tcl, perl, ruby, quickjs, tinygo, go, java")
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
duktape/
  module.ae            import duktape: the bridge's API
  aether_host_duktape.c the bridge, compiled into the importing program (@source)
  duktape_abi.h        the Duktape types the bridge uses (no Duktape dev package needed)
  test_host_duktape*.ae the bridge's specs
  examples/            the programs in README.md's four ways
  worker/              duktape-worker: Duktape in a contained child (std.guest)
  containment/         the containment hosts and checks.js, stunt's rows in JavaScript;
                       lang.sh; most rows are left out, with why: Duktape has no I/O
  PROVENANCE.md        where the bridge came from, and what the move changed
quickjs/
  module.ae            import quickjs: the bridge's API
  aether_host_quickjs.c the bridge, compiled into the importing program (@source);
                       the engine is aether's contrib.quickjs, not a copy
  test_host_quickjs.ae, test_quickjs_bridge.ae  the bridge's specs
  examples/            the programs in README.md's four ways
  worker/              quickjs-worker: QuickJS in a contained child (std.guest)
  containment/         the containment hosts and checks.js, stunt's rows in JavaScript;
                       lang.sh; most rows are left out, with why: no I/O of its own
  PROVENANCE.md        where the bridge came from, and what the move changed
js/
  module.ae            import js: the back-compat alias of duktape
  test_js_alias.ae     the alias reaches the bridge
tcl/
  module.ae            import tcl: the bridge's API
  aether_host_tcl.c    the bridge, compiled into the importing program (@source)
  tcl_abi.h            the Tcl types the bridge uses (no Tcl dev package needed)
  test_host_tcl*.ae    the bridge's specs (test_support.c: their C helpers)
  worker/              tcl-worker: Tcl in a contained child (std.guest)
  containment/         the containment hosts and checks.tcl, stunt's rows in Tcl;
                       lang.sh, what the harness needs to know about Tcl
perl/
  module.ae            import perl: the bridge's API
  aether_host_perl.c   the bridge, compiled into the importing program (@source)
  perl_abi.h           the Perl types the bridge uses (no perl dev headers needed)
  test_host_perl*.ae   the bridge's specs (test_support.c: their C helpers)
  worker/              perl-worker: Perl in a contained child (std.guest)
  containment/         the containment hosts and checks.pl, stunt's rows in Perl;
                       lang.sh; test_imports.sh, loading modules under a sandbox
ruby/
  module.ae            import ruby: the bridge's API
  aether_host_ruby.c   the bridge, compiled into the importing program (@source)
  ruby_abi.h           the Ruby types the bridge uses (no Ruby dev package needed)
  test_host_ruby.ae    the bridge's spec (test_support.c: its C helpers)
  worker/              ruby-worker: CRuby in a contained child (std.guest)
  containment/         the containment hosts and checks.rb, stunt's rows in Ruby;
                       lang.sh; test_requires.sh, requiring under a sandbox
  PROVENANCE.md        where the bridge came from, and what the move changed
tinygo/
  module.ae            import tinygo: the bridge's API (the name is kept from aether's
                       contrib.host.tinygo; the libraries come from standard go)
  aether_host_tinygo.c the bridge, compiled into the importing program (@source)
  test_host_tinygo*.ae the bridge's specs (test_support/: a C stand-in for a Go library)
  examples/            the Go libraries and the programs in README.md's four ways
  worker/              tinygo-worker: a Go c-shared library in a contained child (std.guest)
  containment/         the containment hosts and checks.go, stunt's rows in Go, built
                       into a c-shared library; lang.sh; test_threads.sh, goroutines and
                       thread-local grants; test_examples.sh
go/
  module.ae            import go: the bridge's API (starts a Go program as a child)
  aether_host_go.c     the bridge, compiled into the importing program (@source)
  test_host_go.ae      the bridge's spec
  go.mod               one Go module: rows, guestchild, worker, containment, examples
  rows/                stunt's rows in Go (cgo for the libc ones), shared by the programs below
  guestchild/          std.guest's worker protocol, in Go
  worker/              go-worker: Go in a contained child (std.guest)
  containment/         the containment hosts and checks.go, stunt's rows in Go;
                       lang.sh; test_ways.sh, what the four ways are for a child process
  examples/            the programs in README.md's four ways
  PROVENANCE.md        where the bridge came from, and what the move changed
java/
  module.ae            import java: the bridge's API (grant_jvm_runtime, and java.run* to start a JVM)
  aether_host_java.c   the bridge, compiled into the importing program (@source): go's, for java
  build.sh             javac and jar: the agent jar and the classes below (JDK 21 or later)
  src/                 the agent (-javaagent): grant checker, hooks, shared map, Native (libc through FFM)
  rows/                stunt's rows in Java, shared by the programs below
  guestchild/          std.guest's worker protocol, in Java
  worker/              java-worker: Java in a contained child (std.guest)
  containment/         the containment hosts and Checks.java; lang.sh; test_ways.sh, the four ways
                       and what libc sees (libghltrace.c)
  examples/            the programs in README.md's four ways
  PROVENANCE.md        where the bridge came from, and what the move changed
tests/
  run.sh               everything, every language (GUEST_LANG=<lang> for one)
  stunt_lib.sh         holds a language to stunt's catalogue, by name
  test_stunt_*.sh      one per policy: aether's contrib_host_stunt lists
  test_catalogue.sh    every catalogue row is named here or left out
  parity.sh            renders and checks a README's Stunt parity table
  <lang>_env.sh        find the language's runtime library on this machine (lua, python,
                       duktape, tcl, perl, ruby; quickjs: checks ae ships its engine)
                       duktape, tcl, perl, ruby), or for tinygo the go toolchain, which
                       builds the c-shared libraries the tests load
                       duktape, tcl, perl, ruby), or for go the toolchain and cgo;
                       java_env.sh finds a JDK 21 or later (a JVM is started, not loaded)
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

Then `import lua` resolves to `lua/module.ae`, and likewise `import python`,
`import duktape` (`import js`, its alias), `import quickjs`, `import tcl`, `import perl` and
`import ruby` to their directories' `module.ae`:
`import duktape` (`import js`, its alias), `import tcl`, `import perl`,
`import ruby` and `import tinygo` to their directories' `module.ae`:
`import duktape` (`import js`, its alias), `import tcl`, `import perl` and
`import ruby` and `import go` to their directories' `module.ae` (and `import java`):
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
same, since the checkout's root holds each language's `<lang>/module.ae`.

At run time the bridge `dlopen`s the language's library (all but QuickJS,
whose engine is compiled into the program: nothing to find, nothing to set). Each bridge tries
the usual sonames for its platform first (its README lists them), and an
environment variable names an exact soname or path when that is not enough:
`AETHER_LUA_SONAME` (for example `liblua5.4.so.0`, or
`/opt/homebrew/lib/liblua.dylib`), `AETHER_PYTHON_SONAME` (for example
`libpython3.12.so.1.0`), `AETHER_DUKTAPE_SONAME` (`libduktape.so.207`, or
Homebrew's `/opt/homebrew/lib/libduktape.so`), `AETHER_TCL_SONAME`
(`libtcl8.6.so.0`), `AETHER_PERL_SONAME` (for example
`/usr/lib/x86_64-linux-gnu/libperl.so.5.38`) and `AETHER_RUBY_SONAME`
(for example `/usr/lib/libruby.so.3.4.12`). `tests/<lang>_env.sh` is one way
to find each. No language's dev package is needed to build. QuickJS is
compiled in from aether's `contrib.quickjs`, so there is nothing to find.
The two Go bridges are the exceptions to the pattern: `tinygo.load` takes
the path of a library you built with `go build -buildmode=c-shared` (the
bridge needs no Go toolchain, only the library's author does), and `go`
loads no library at all: `GO_BIN` names the `go` command for
`go.run_script`, and a built Go binary needs nothing. `java` loads none either:
`JAVA_BIN` names the `java` command, `AETHER_JAVA_FLAGS` its flags
(`--enable-native-access=ALL-UNNAMED`, and `--enable-preview` on JDK 21) and
`AETHER_JAVA_AGENT` the agent jar that sandboxed runs add; `JAVA_HOME` is only
how the tests find a JDK.

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
GUEST_LANG=duktape sh tests/run.sh
GUEST_LANG=tcl TCL_VERSION=8.6 sh tests/run.sh
RUBY=/opt/homebrew/opt/ruby/bin/ruby GUEST_LANG=ruby sh tests/run.sh
LUA_VERSION=5.4 GUEST_LANG=lua sh tests/run.sh
PYTHON=/usr/bin/python3.12 GUEST_LANG=python sh tests/run.sh
GUEST_LANG=perl sh tests/run.sh    # PERL=/path/to/perl to pick the perl
GUEST_LANG=tinygo sh tests/run.sh  # GO=/path/to/go to pick the go
GUEST_LANG=go sh tests/run.sh      # GO=/path/to/go to pick the go command
GUEST_LANG=java sh tests/run.sh    # JAVA_HOME=/path/to/jdk to pick the JDK (21 or later)
sh tests/run.sh --write-parity     # rewrite the parity tables' columns this host ran
```

The containment rows build stunt from aether's source at
`AETHER_STUNT_REF` (three files, fetched once into `build/`; or set
`AETHER_SRC` to an aether checkout) to read its catalogue: release archives
ship `contrib/host/stunt` without `stunt.c` and `stunt_main.c`. On Linux
the in-process rows run under the release's `libaether_sandbox.so`; the
child rows run where ae is 0.878.0 or later and the kernel has Landlock
(`GHL_REQUIRE_CHILD=1` makes a missing child mode a failure, as CI does).

CI (`.github/workflows/ci.yml`), one language per job, four jobs per
language: Linux (Ubuntu 24.04) on `AE_PIN` (in-process), on `AE_CHILD_PIN`
(in-process and contained child) and on `AE_SPAWN_PIN` (the same, on the
first ae whose preload holds the exec rows), and macOS on `AE_PIN`. Linux
installs only each language's runtime package, never a dev package:
liblua5.4-dev (Lua), libpython3.12, libduktape207, tcl8.6 (QuickJS needs nothing installed), perl and
libperl5.38t64, ruby and libruby3.2; macOS uses Homebrew's lua (5.5) and
lua@5.4, python@3.13, duktape, tcl-tk and ruby, and the system perl (5.34).
liblua5.4-dev (Lua), libpython3.12, libduktape207, tcl8.6, perl and
libperl5.38t64, ruby and libruby3.2, golang-go (Go, with the runner's gcc for cgo); macOS uses Homebrew's lua (5.5) and
lua@5.4, python@3.13, duktape, tcl-tk and ruby, the system perl (5.34), and the runner's Go.
liblua5.4-dev (Lua), libpython3.12, libduktape207, tcl8.6, perl and
libperl5.38t64, ruby and libruby3.2, golang-go (Go 1.22; gcc is the
runner's); macOS uses Homebrew's lua (5.5) and lua@5.4, python@3.13,
duktape, tcl-tk and ruby, the system perl (5.34), and the runner's Go
(`brew install go` if it has none). Java needs only a JDK, openjdk-21-jdk-headless
on Ubuntu 24.04 and a JDK 21 from the runner or `brew install openjdk@21` on
macOS; its Linux child jobs pass with the JVM blocked by the preload, which the
tests check (java/README.md).

## Moving from `contrib.host.<lang>`

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

The Duktape bridge was copied from aether at `2609ec841`
(duktape/PROVENANCE.md), together with its `contrib.host.js` alias, and moves
the same way:

```diff
-import contrib.host.duktape
+import duktape
```

(`import contrib.host.js` becomes `import js`.) What changes: no
`make contrib MODULES=duktape` step and no libduktape-dev at build time, and
libduktape is found under its usual names without `AETHER_DUKTAPE_SONAME`.
Duktape has no I/O of its own, so what it can do in a sandbox is what the
bridge's `env`, `readFile`, `writeFile` and `exec` are allowed: they hold on
macOS as on Linux, with no preload (duktape/README.md).
The QuickJS bridge was copied from aether at `2609ec841`
(quickjs/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.quickjs
+import quickjs
```

What changes: no `make contrib` step; and a script now has `print` and
`console.log` (aether's could only compute). The engine is not copied:
the bridge imports aether's `contrib.quickjs`, which every release ships with
its pinned quickjs-ng 0.17.0, so there is still one copy of QuickJS and one
pin. QuickJS has no I/O of its own (`quickjs-libc` is not compiled in), so
most of stunt's rows are left out and nothing in it needs the preload
(quickjs/README.md).

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
The Perl bridge was copied from aether at `2609ec841`
(perl/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.perl
+import perl
```

What changes: no `make contrib MODULES=perl` step and no perl dev headers
at build time, and fixes aether's copy needed to be usable: it ignored a
failed `perl_parse` (macOS's perl crashed on the first sub), could not load
XS modules, wrote a shared-map output the host never saw, spliced the map's
values into Perl source and unset the host's own environment variables for
good; host functions (`perl.register`) and `perl.last_error()` are new.
The Ruby bridge was copied from aether at `2609ec841`
(ruby/PROVENANCE.md), and moves the same way, the import line and the
dependency:

```diff
-import contrib.host.ruby
+import ruby
```

What changes: no `make contrib MODULES=ruby` step and no Ruby dev package at
build time; `ruby.finalize_host()` is terminal and says so (a later
`init_host` returns -1 where aether's returned 0 and crashed on the next
eval); a sandboxed run puts the process's environment back when it ends
(aether's deleted the ungranted variables for good); `aether_map_put` in
`run_sandboxed_with_map` writes to the host's map; `puts` output is no longer
lost at exit; and Ruby 3.3 and later get their built-in methods (aether's
copy raised `NoMethodError` for `Integer#times` and the like). RubyGems is not
loaded by default (ruby/README.md, "Lifecycle").

The Go bridge was copied from aether at `2609ec841`
(tinygo/PROVENANCE.md), and moves the same way, the import line and the
dependency (the module keeps the name `tinygo`: the libraries it loads are
standard Go's, since TinyGo's c-shared is wasm-only):

```diff
-import contrib.host.tinygo
+import tinygo
```

What changes: no `make contrib MODULES=tinygo` step. (`call_dynamic` is the
libffi-less stub, as aether's is when libffi was not found.) What does not
change, and is the thing to know: the bridge checks no grant itself. A Go
library makes its system calls without libc, so the preload holds only what
it does through cgo, and aether's `sandbox.enforce` levels are per thread
while Go has threads of its own; only the contained child's kernel layer
holds a Go library (tinygo/README.md, "Four ways").
(go/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.go
+import go
```

What changes: no `make contrib MODULES=go` step; an empty grant list is a
grant list (aether's copy returned -1 for it and started nothing); and the
README says what the preload does for a Go child, which is less than aether's
said: Go makes its own system calls, so on Linux only what a program reaches
through cgo is held, and a static binary is not held at all. `go.run_sandboxed`
still starts nothing on macOS. The containment that holds Go's own calls is
the contained child (go/README.md, "4"), new here.

The Java bridge was copied from aether at `2609ec841`
(java/PROVENANCE.md), and moves the same way:

```diff
-import contrib.host.java
+import java
```

`java.grant_jvm_runtime()` is unchanged. What changes: it builds on JDK 21
(aether's agent used JDK 22's `allocateFrom`; the Foreign Function calls are
one class now, `Native`), the agent's grant checker follows the matcher the
rest of Aether uses (ports in `tcp` grants), and the bridge starts the JVM
(`java.run`, `java.run_sandboxed`, `java.run_sandboxed_with_map`), where
aether left that to the caller's `spawn_sandboxed`. What does not change, and is
the thing to know: a JVM does not start under `libaether_sandbox.so`, which
refuses it executable memory, so on Linux neither `java.run_sandboxed` nor the
contained child can run one today (java/README.md, "The wall that stops a
JVM"). The agent and the bridge's other calls are unaffected.

The plan for aether's copies: they stay in `contrib/host/` for a transition
period, with their READMEs pointing here; fixes land here first. They are
removed from aether in a later release, announced in aether's changelog
beforehand. Do not import both copies of a language in one program: they
define the same C symbols.

## License

MIT, as aether: see [LICENSE](LICENSE).

# Provenance

The Ruby bridge was copied from aether's `contrib/host/ruby/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, the v0.889.0 release merge,
2026-11-05), with the parts of aether's tests that exercise it. aether is
MIT licensed, copyright "Aether Programming Language Contributors"; so is
this repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_ruby.c` | `contrib/host/ruby/aether_host_ruby.c` | see below |
| `aether_host_ruby.h` | `contrib/host/ruby/aether_host_ruby.h` | the host-function API added (below) |
| `module.ae` | `contrib/host/ruby/module.ae` | `@source("aether_host_ruby.c")`; header comment; the host-function API added |
| `test_host_ruby.ae` | `contrib/host/ruby/test_host_ruby.ae` | `import ruby` instead of `import contrib.host.ruby`; the `describe` name; two more cases for the original surface and the specs for what was added (below) |
| `test_support.c` | `tests/sandbox/test_shared_map_all.sh` | the C that script did inline that an Aether spec cannot: maps by token, a second thread |
| `ruby_abi.h` | (new) | the Ruby types the bridge uses |
| `containment/`, `worker/` | (new) | modelled on `../python/containment` and `../python/worker` |

Not brought over: `tests/integration/namespace_ruby/` (it tests `ae build
--namespace`, which generates a Ruby SDK for an Aether library over Fiddle:
the other direction, and the compiler's, not this bridge's),
`tests/sandbox/test_dlopen_ruby.rb` (a script that tries Fiddle on libc under
the preload, run by aether's sandbox suite: its cases are the containment
rows `getenv-home`, `dlopen-libc-by-name` and `dlopen-libc-by-path` here,
`containment/checks.rb`), and the Ruby case of
`tests/sandbox/test_shared_map_all.sh`, which became the "ruby shared map"
spec in `test_host_ruby.ae` (same script, same three assertions, plus that
the write reaches the host's map). aether's build of the bridge
(`tools/docker/scripts/install-ruby-headers.sh`, `make contrib MODULES=ruby`)
is what `@source` and `ruby_abi.h` replace.

## What the move changed in `aether_host_ruby.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_ruby.a`, with the headers of a Ruby dev package and
`-DAETHER_HAS_RUBY -DAETHER_HAS_SANDBOX` on the command line. Here it is
compiled into each program that imports it (`@source` in `module.ae`), with
the cflags `ae build` uses for any program. So:

- `#include "../../../runtime/aether_sandbox.h"` (and `aether_shared_map.h`)
  became `#include "aether_sandbox.h"` and so on: the installed runtime's
  headers, already on that include path. `#include "../aep_dl.h"` (contrib/
  host's dlopen shim, which maps the POSIX names onto Win32) became
  `#include <dlfcn.h>`, as the Lua and Python bridges have it: none of them
  builds for Windows here, so `ruby_sysinit` and the `_WIN32` branch are gone.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_RUBY` are defined in the file when the
  build does not define them.
- `<ruby.h>` was replaced by `ruby_abi.h`, which declares the two types the
  bridge needs (`VALUE`, `ID`). The bridge never linked against libruby (it
  uses `dlopen` and `dlsym`), so a Ruby dev package is no longer needed to
  build a program; the Ruby runtime is still needed to run one. The
  `#undef snprintf` that undid ruby.h's `subst.h` is gone with the header.
- `Qnil` was already read from the interpreter (aether #1618); `Qtrue` and
  `Qfalse` are now too, since the bridge now returns booleans to Ruby. Nothing
  about the special constants is baked in (`ruby_abi.h` says why).
- `ruby_init` became `ruby_setup`, which returns its failure instead of
  exiting the process.
- `RUBY_INIT_STACK` was replaced as before by a call to `ruby_init_stack`,
  but with the real top of the calling thread's stack rather than the address
  of a local in whichever frame happened to start Ruby: see the README,
  "Lifecycle".
- `ruby_options` is called after `ruby_setup` (with `--disable-gems
  --disable-rubyopt -e nil`) instead of `ruby_init_loadpath`. Without it
  Ruby 3.3 and later lack the methods they define in Ruby (`Integer#times`,
  `Kernel#Integer`, `GC.start`: `NoMethodError` on the first call), which
  aether's copy never noticed because its test calls none of them.
- `$stdout` and `$stderr` are set to `sync` at init, so `puts` is not lost
  when the program exits (aether's README described the loss and two
  workarounds).
- After `ruby_finalize_host()`, `ruby_init_host()` returns -1 and says why.
  aether's returned 0 and the next `ruby_run` segfaulted. Another thread than
  the one that started Ruby is refused (-1) too.
- `ruby_finalize_host()` calls `ruby_cleanup(0)` where aether's called
  `ruby_finalize()`, so `at_exit` handlers run and the streams are flushed.
- `run_sandboxed` and `run_sandboxed_with_map` hide the variables of `ENV`
  that the grants do not name, as before, but put the whole environment back
  when the run ends: aether's deleted them from the process for good, so a
  program that called `run_sandboxed` once had lost its own environment. A
  grant pattern with a quote in it is escaped; before it ended the string
  early and the rest ran as Ruby. Both entry points check the nesting depth
  before touching anything.
- `run_sandboxed_with_map`: `aether_map_get` and `aether_map_put` are C
  functions that read and write the host's map by token (and refuse a write
  to a key the host gave as an input), as in the Python bridge. aether's were
  Ruby methods over two hashes copied from the map at the start, and
  `aether_map_put` wrote to a hash nobody read: a script's output never
  reached the host's map.
- The message of the stub built with `AETHER_HAS_RUBY=0` names this module.

## What was added to the API

aether's bridge had five functions: `run`, `run_sandboxed`,
`run_sandboxed_with_map`, `init_host`, `finalize_host`, all unchanged here.
The containment tests and the contained child's worker need to give Ruby host
functions, so there are now, in the shape of the Python bridge's two-way VM
without the VM handle (Ruby has one interpreter per process):

`register`, `eval`, `result_str`, `set_instruction_limit`, `arg_count`,
`arg_type`, `arg_str`, `arg_int`, `arg_double`, `arg_bool`, `push_int`,
`push_str`, `push_bool`, `push_nil`, `push_double`, `raise_error`, and the
tags `RB_TNIL`, `RB_TBOOL`, `RB_TINT`, `RB_TSTR`, `RB_TOTHER`, `RB_TFLOAT`.

Nothing else in the file changed in the move. Later changes made here are in
this repository's history (`git log -- ruby/`).

aether keeps its copy for a transition period; see the top-level README.

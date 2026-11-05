# Provenance

The Perl bridge was copied from aether's `contrib/host/perl/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, 2026-11-05; the directory's
last change before it was `6854baa8a`, 2026-10-28), with the part of aether's
tests that exercises it. aether is MIT licensed, copyright "Aether
Programming Language Contributors"; so is this repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `aether_host_perl.c` | `contrib/host/perl/aether_host_perl.c` | see below |
| `aether_host_perl.h` | `contrib/host/perl/aether_host_perl.h` | the host-function and `last_error` declarations added |
| `module.ae` | `contrib/host/perl/module.ae` | `@source("aether_host_perl.c")`; header comment; the new functions |
| `test_host_perl.ae` | `contrib/host/perl/test_host_perl.ae` | `import perl` instead of `import contrib.host.perl`; the `describe` name |
| `test_host_perl_host.ae`, `test_support.c` | `tests/sandbox/test_shared_map_all.sh` (its Perl case, a C harness) | the shared-map case as an Aether spec that can see what the script wrote, and cases for what is new (host functions, `%ENV`, XS modules, re-init, two threads) |
| `perl_abi.h` | (new) | the Perl types and constants the bridge uses |
| `containment/`, `worker/` | (new) | modelled on `../python/containment` and `../python/worker` |
| `../tests/perl_env.sh` | (new) | finds libperl and perl's library directories from `Config` |

Not brought over: `README.md` (rewritten for this repository),
`tests/scripts/contrib_build.sh`'s perl case and
`tools/docker/scripts/install-perl-headers.sh` (they build and provision the
perl dev headers, which nothing here needs), and `tests/sandbox/test_dlopen_perl.pl`
(a script that tries `syscall`, `DynaLoader::dl_load_file` and `system` under
the preload, run by aether's sandbox suite: its cases are the containment
rows `syscall-fn-openat`, `dlopen-libc-by-name`, `dlopen-libc-by-path` and
`exec-bin-sh` here, `containment/checks.pl`).

## What the move changed in `aether_host_perl.c`

In aether the bridge is compiled by `make contrib` into
`libaether_host_perl.a`, with the headers of the perl dev package found by
`perl -MExtUtils::Embed` and `-DAETHER_HAS_PERL -DAETHER_HAS_SANDBOX` on the
command line. Here it is compiled into each program that imports it
(`@source` in `module.ae`), with the cflags `ae build` uses for any program.
So:

- `#include "../../../runtime/aether_sandbox.h"` (and
  `aether_shared_map.h`) became `#include "aether_sandbox.h"` and so on:
  the installed runtime's headers, already on that include path.
  `aether_sandbox_match.h` and the `list_size` / `list_get_raw` externs went
  with the environment scrub (below), which no longer reads the grant list.
- `AETHER_HAS_SANDBOX` and `AETHER_HAS_PERL` are defined in the file when
  the build does not define them.
- `<EXTERN.h>` and `<perl.h>` were replaced by `perl_abi.h`, which declares
  the few Perl types the bridge needs, and says why each is the same across
  Perl 5. The bridge never linked against libperl (it uses `dlopen` and
  `dlsym`), so the perl dev headers are no longer needed to build a program;
  the Perl runtime is still needed to run one. `XSINIT_t` and the
  `PerlInterpreter`, `SV`, `I32`, `U32` and `STRLEN` types are declared there;
  `SV_GMAGIC` is now the header's.
- The message of the stub built with `AETHER_HAS_PERL=0` names this module.
- `dlerror()` is read once when libperl cannot be loaded (the second call
  gave NULL, printed with `%s`), as in the Lua and Python bridges here.

## What was changed beyond the move

Aether's bridge could not be used the way the Lua and Python ones were, so
the following was fixed or added, each with a case in `test_host_perl_host.ae`
or the containment tests:

- **`PERL_SYS_INIT3`** (`Perl_sys_init3`, when libperl exports it) is called
  once per process before the first interpreter, and `perl_parse`'s status
  is checked. Without it macOS's system perl fails to parse (status 9, which
  aether's copy ignored) and the first sub defined after it crashes the
  process.
- **DynaLoader's native half**: `perl_parse` is given an `xs_init` that
  installs `boot_DynaLoader` (from libperl, where it is exported), so
  `use POSIX`, `use Socket` and every other XS module load. aether's passed
  none.
- **One interpreter, one thread at a time**: a recursive lock around every
  entry, and `Perl_set_context` (when exported) first, since a threaded
  perl's current interpreter is per thread.
- **A real shared map.** aether's `aether_map_put` wrote a hash
  (`%_aether_output`) the host never saw ("tracked in docs/next-steps.md"),
  its `aether_map_get` read only the inputs (so a script could not read back
  what it put), and the inputs were spliced into Perl source inside single
  quotes: a value with a quote ran as code. Now `aether_map_get` and
  `aether_map_put` reach the host's map through the token (`put` answers 1,
  or 0 for a frozen key), `%_aether_input` and `%_aether_output` are kept,
  and no value is spliced into source.
- **`%ENV` under `run_sandboxed`.** aether deleted from `%ENV` the variables
  the grants did not name, which also unset them in the process for good
  (perl's `%ENV` is tied to the C environment), and matched a pattern grant
  as a literal name (and interpolated it into source). Now each variable is
  asked of the grants in force (`aether_sandbox_check_slow("env", name)`, the
  runtime's own matcher), and `%ENV` is put back after the run.
- **Host functions** (`perl.register` and the `arg_*`, `push_*` and
  `raise_error` calls), which aether's bridge had none of, and
  `perl.last_error()`. A host function is a Perl sub that sets one scalar
  and reads another whose magic *get* hook runs the callback, so no XSUB
  argument-stack macro, and so no interpreter layout, is read.
- A perl built without `MULTIPLICITY` is refused at load with a message
  (`perl_abi.h` says how it is told) rather than called with the wrong
  arguments.

Nothing else in the file changed in the move. Later changes made here are in
this repository's history (`git log -- perl/`).

aether keeps its copy for a transition period; see the top-level README.

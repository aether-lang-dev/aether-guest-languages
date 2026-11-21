# Tcl's exec stunt row is refused from Aether 0.927.0 on

Found 2026-11-20 while moving AE_PIN to 0.931.0. Not understood as an Aether
bug or an intended tightening; for Aether's owners to say which.

## Repro

On Linux x86_64 (Ubuntu 22.04, kernel 5.15), with `tclsh` 8.6 and the Tcl bridge
of this repository:

    GUEST_LANG=tcl sh tests/test_stunt_exec.sh

Aether 0.923.0: the in-process row `exec-bin-sh` (policy `exec=/bin/sh,fork`,
nothing else granted) succeeds, as the catalogue (aether
tests/integration/contrib_host_stunt/test_stunt_exec.sh) expects.

Aether 0.927.0 and 0.931.0: the same row reports
`refused (exec: couldn't create error file for command: permission denied)`,
so `[FAIL] stunt_exec (inproc, policy exec=/bin/sh,fork)`.

Tcl's `exec` makes a temporary file for the command's stderr before it forks.
Until 0.923.0 the preload let that file be created with only `exec` and `fork`
granted; from 0.927.0 it does not (the same file creation is already why the
contained child's row is `stricter`). Perl, Lua, Python and Ruby rows of the
same test were not changed by the bump.

## What is needed

Either the preload should allow a temporary file for a granted exec (then
nothing changes here), or the catalogue's policy for this row needs an
`fs_write` for the temp directory (then lang.sh's `libc_row` for Tcl is
adjusted and the parity table regenerated). Nothing was changed here to hide
the row: it stays red on AE_PIN 0.931.0 for Tcl in-process until decided.

## RESOLVED 2026-11-21: an intended tightening (aether #3751), nothing for Aether to change

Not an Aether bug. Aether's 0.927.0 changelog says it: "Under the LD_PRELOAD
library ... a temporary file needs `fs_write` where it is made". Evidence on
ae 0.931.0, Ubuntu 22.04:

- The preload's own log (`aether-sandbox.log` in the working directory) for
  the failing row reads `AETHER_DENIED: env TMPDIR` and
  `AETHER_DENIED: fs_read /tmp/tcl_f8cgjl`: Tcl asks libc's `getenv` for
  `TMPDIR` (refused under `exec=/bin/sh,fork`, so it falls back to `/tmp`),
  then calls `mkstemp` on `/tmp/tclXXXXXX`.
- 0.927.0's preload interposes `mkstemp` (`fs_mkstemps_at` in
  `runtime/libaether_sandbox_preload.c`, which opens the new name
  `O_RDWR|O_CREAT|O_EXCL` through the checked open). 0.923.0 did not wrap it:
  glibc's `mkstemp` reached its internal open past the preload, which is why
  the row passed before.
- `strace` shows the call is a bare `exec`'s: `openat("/tmp/tcl_...",
  O_RDWR|O_CREAT|O_EXCL, 0600)` then `unlink`, once per `exec` with stderr not
  redirected. With `2>@1` or `2>@stderr` Tcl makes no file.

What this repository did: stunt's `exec-bin-sh` row is done with `2>@1` (it
asks whether `exec` and `fork` alone let a shell start, and they do), and the
bare `exec` is its own test, `tcl/containment/test_exec_tmpfile.sh`: refused
under `exec=/bin/sh,fork` from 0.927.0, and allowed under the narrowest grant,
`fs_read` and `fs_write` on one scratch directory plus `env=TMPDIR`. See
"A bare `exec` needs a temporary file" in `tcl/README.md`.

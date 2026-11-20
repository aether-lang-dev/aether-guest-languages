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

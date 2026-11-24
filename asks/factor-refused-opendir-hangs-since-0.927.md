# A Factor in-process row hangs when the preload refuses a directory listing (0.927.0 on)

**Resolved here, 2026-11-21, in the fork:** the hang is the embedded VM's vocabulary monitor, whose error path writes through an invalid buffer when a watched directory cannot be opened (it reproduces with `chmod 000` and no sandbox). The fork's `embed_vm_init` now passes `-no-monitors` (branch `embed-no-monitors`, 6b88e3b758) and this repository no longer grants the read of Factor's tree. Nothing is asked of Aether. Rechecked 2026-11-24 on cachyos with the fork built at 6b88e3b758 and private installs of ae 0.939.0 and 0.941.0 (`GUEST_LANG=factor GHL_REQUIRE_CHILD=1 FACTOR_HOME=<fork build> sh tests/run.sh`): both pass, 59 rows, 0 failures, 2m08s and 2m30s, no row near its timeout. A Factor tree without the fork's `-no-monitors` commit still hangs. What follows is the original write-up.

Found 2026-11-21 while getting this repository's CI green. The row is made to
pass here by a grant (below), so this is not blocking; it is written down because
what Factor does on the refusal looks like a crash that is not Factor's
ordinary handling of EACCES, and Aether's owners may want to look at it.

## What happens

Every in-process Factor row (`GUEST_LANG=factor sh tests/run.sh`, any
`tests/test_stunt_*.sh`) hangs at 100% CPU on ae 0.927.0, 0.931.0, and passes
on 0.926.0 and 0.923.0 (private installs, same fork build, same tree).
Evidence, on cachyos with the fork at the pinned ref:

- The preload's log (`aether-sandbox.log` in the row's working directory) says
  `AETHER_DENIED: fs_read` for `<factor tree>/core`, `basis`, `extra` and
  `work`, in that order: Factor lists its four vocabulary roots while it
  evaluates the script, and the policy (`exec=/bin/sh,fork`, or no grants)
  does not read them. Aether's 0.927.0 changelog says that is intended: "a
  listing (`opendir`, `scandir`) needs `fs_read` on the directory itself".
- With `fs_read=<factor tree>/*` added to the policy the row passes
  (exec-bin-sh succeeds, open-etc-passwd is still refused).
- Without it, gdb stops the host on SIGSEGV in a libc `memcpy` (a 32 to 64
  byte copy) whose destination is a garbage pointer (`rdi=0x55e75963ec70`),
  called from Factor's code heap; with the signal passed on, Factor's SIGSEGV
  handler returns to the same instruction forever (strace shows the same
  `si_addr` and `rt_sigreturn` thousands of times).
- Plain Factor (no Aether, no preload) given an unreadable directory
  (`chmod 000`) raises `libc-error { errno 13 }` from `directory-entries` and
  carries on. So a refused `opendir` is survivable; under the preload it is
  not. `opendir` in the preload returns NULL with `errno = EACCES` after
  logging, which is what the plain case gives Factor too, so what differs is
  not understood here: Factor's own error path while a level is in force
  (it prints the error through streams, and `factor/README.md` already notes
  that a Factor error before its output is set up dies), or the preload.

## Not reduced further

A repro that does not need this repository needs libfactor (the embed-api
fork) loaded in an Aether program under `sandbox.enforce`; the standalone
`LD_PRELOAD` plus `AETHER_SANDBOX_GRANTS` route does not work for Factor,
because that mode refuses its executable anonymous mapping
(`mmap_exec_anonymous`) before the script runs. The repro is therefore this
repository at the commit before the fix:

    GUEST_LANG=factor FACTOR_HOME=<fork build> sh tests/test_stunt_exec.sh

on ae 0.927.0 or later, with `with_tree` removed from `factor/containment/host.ae`.

## What this repository did

`factor/containment/host.ae` adds `fs_read` on `$FACTOR_HOME/*` to each row's
grants, nothing else, and says why in `factor/README.md` ("What a sandboxed
script can load"). `tests/stunt_lib.sh` ends a row after `GHL_ROW_TIMEOUT`
seconds so a hang is a failure.

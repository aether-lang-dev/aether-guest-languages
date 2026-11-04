#!/bin/sh
# Host-checked: what only the host can judge.
#
# spin is stopped by the host: in-process by the bridge's instruction limit,
# in a child by a CANCEL frame at the deadline, which it must answer.
# silent never answers the CANCEL: the host kills it after a grace period.
# oversize sends a 1 MiB frame: the host refuses it by its header.
# half-frame sends a header and part of the payload, then stalls: the host
# times out inside the frame (ipc.TIMED_OUT_INSIDE_FRAME) and hangs up.
# The checks are stunt's (aether contrib/host/stunt), done in the guest language; stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=stunt_host_checked
# The same policy and rows as aether tests/integration/contrib_host_stunt/test_stunt_host_checked.sh (v0.875.0).
. "$SCRIPT_DIR/stunt_lib.sh"

policy "-"
stunt spin host
stunt silent host
stunt oversize host
stunt half-frame host
done_checks

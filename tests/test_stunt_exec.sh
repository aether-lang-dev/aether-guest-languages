#!/bin/sh
# /bin/sh granted, and fork: the guest can start a shell, and only that.
#
# exec /bin/sh and fork. A shell starts; nothing else is granted.
# The checks are stunt's (aether contrib/host/stunt), done in the guest language; stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=stunt_exec
# The same policy and rows as aether tests/integration/contrib_host_stunt/test_stunt_exec.sh (v0.875.0).
. "$SCRIPT_DIR/stunt_lib.sh"

policy "exec=/bin/sh,fork"
stunt exec-bin-sh succeed
stunt fork succeed
stunt open-etc-passwd refused
done_checks

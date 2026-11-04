#!/bin/sh
# A native grant: libc can be loaded by name or by path.
#
# native *. Without it (test_stunt_no_grants.sh) a guest cannot get a handle
# on libc, which would let it call the real open/connect/execve past the
# preload; with it, it can.
# The checks are stunt's (aether contrib/host/stunt), done in the guest language; stunt_lib.sh runs each one in
# every mode that applies here and stops at the first mismatch.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=stunt_native
# The same policy and rows as aether tests/integration/contrib_host_stunt/test_stunt_native.sh (v0.875.0).
. "$SCRIPT_DIR/stunt_lib.sh"

policy "native=*"
stunt dlopen-libc-by-name succeed
stunt dlopen-libc-by-path succeed
stunt open-etc-passwd refused
done_checks

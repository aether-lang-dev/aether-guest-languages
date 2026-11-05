#!/bin/sh
# Ruby's own: a library required under the sandbox. Not a row of stunt's
# catalogue, but what decides whether a script runs at all: Ruby reads a
# library from disk the first time it is required, so what a policy grants on
# Ruby's library directories decides what a script can require.
#
#   in-process, no grants        refused under the preload (the open of the
#                                library's source); not held without it
#   in-process, the library read requires
#   contained child, any policy  a library the worker did not require before
#                                its lockdown is refused; one it did is there
#
# The hosts are the containment tests' (host.ae, child_host.ae), the row is
# checks.rb's require_feature, run as check "require:<library>".
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=ruby_requires
GUEST_LANG=ruby
. "$SCRIPT_DIR/stunt_lib.sh"

stdlib_policy=""
for d in $(echo "$GHL_RUBY_STDLIB" | tr ',' ' '); do
    stdlib_policy="${stdlib_policy:+$stdlib_policy,}fs_read=$d/*"
done

# securerandom is Ruby code on disk that nothing here requires first; set is
# built in from Ruby 3.5 but a file in every Ruby before it.
policy "-"
if [ -n "$PRELOAD" ]; then
    run_one PASS inproc refused require:securerandom
else
    run_one "NOT HELD" inproc succeed require:securerandom
fi
policy "$stdlib_policy"
run_one PASS inproc succeed require:securerandom
if [ "$CHILD" = 1 ]; then
    for p in "-" "$stdlib_policy"; do
        policy "$p"
        # securerandom is not in child_host's PREIMPORT; socket is.
        run_one PASS child refused require:securerandom
        run_one PASS child succeed require:socket
    done
fi
echo "  [PASS] $NAME"

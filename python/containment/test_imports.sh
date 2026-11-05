#!/bin/sh
# Python's own: a module imported under the sandbox. Not a row of stunt's
# catalogue, but what decides whether a script runs at all: CPython reads a
# module from disk the first time it is imported, so what a policy grants
# on the standard library decides what a script can import.
#
#   in-process, no grants        refused under the preload (the open of the
#                                module's source); not held without it
#   in-process, the stdlib read  imports
#   contained child, any policy  a module the worker did not import before
#                                its lockdown is refused; one it did import
#                                is there
#
# The hosts are the containment tests' (host.ae, child_host.ae), the row is
# checks.py's import_module, run as check "import:<module>".
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=python_imports
GUEST_LANG=python
. "$SCRIPT_DIR/stunt_lib.sh"

stdlib_policy=""
for d in $(echo "$GHL_PYTHON_STDLIB" | tr ',' ' '); do
    stdlib_policy="${stdlib_policy:+$stdlib_policy,}fs_read=$d/*"
done

policy "-"
if [ -n "$PRELOAD" ]; then
    run_one PASS inproc refused import:json
else
    run_one "NOT HELD" inproc succeed import:json
fi
policy "$stdlib_policy"
run_one PASS inproc succeed import:json
if [ "$CHILD" = 1 ]; then
    for p in "-" "$stdlib_policy"; do
        policy "$p"
        # decimal is not in child_host's PREIMPORT; socket is.
        run_one PASS child refused import:decimal
        run_one PASS child succeed import:socket
    done
fi
echo "  [PASS] $NAME"

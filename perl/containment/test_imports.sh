#!/bin/sh
# Perl's own: a module loaded under the sandbox. Not a row of stunt's
# catalogue, but what decides whether a script runs at all: Perl reads a
# module from disk the first time it is loaded, so what a policy grants on
# its library directories decides what a script can load.
#
#   in-process, no grants        refused under the preload (the open of the
#                                module's source); not held without it
#   in-process, the library read loads
#   contained child, any policy  a module the worker did not load before its
#                                lockdown is refused; one it did load is there
#
# The hosts are the containment tests' (host.ae, child_host.ae), the row is
# checks.pl's import_module, run as check "import:<module>".
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=perl_imports
GUEST_LANG=perl
. "$SCRIPT_DIR/stunt_lib.sh"

lib_policy=""
for d in $(echo "$GHL_PERL_LIB" | tr ',' ' '); do
    lib_policy="${lib_policy:+$lib_policy,}fs_read=$d/*"
done

policy "-"
if [ -n "$PRELOAD" ]; then
    run_one PASS inproc refused import:Text::Wrap
else
    run_one "NOT HELD" inproc succeed import:Text::Wrap
fi
policy "$lib_policy"
run_one PASS inproc succeed import:Text::Wrap
if [ "$CHILD" = 1 ]; then
    for p in "-" "$lib_policy"; do
        policy "$p"
        # Text::Wrap is not in child_host's PREIMPORT; Socket is.
        run_one PASS child refused import:Text::Wrap
        run_one PASS child succeed import:Socket
    done
fi
echo "  [PASS] $NAME"

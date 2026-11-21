#!/bin/sh
# QuickJS's own: Array.prototype.find over a large array and every element
# checked afterwards, in the bridge and in the contained child's worker
# (checks.js, find-intact). See quickjs/test_quickjs_find_interrupt.ae for
# the engine fault it guards; the worker sets no deadline, so what this proves
# there is that the loops end and leave the array whole.
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=quickjs_find
GUEST_LANG=quickjs
. "$SCRIPT_DIR/stunt_lib.sh"

policy "-"
run_one PASS inproc succeed find-intact
[ "$CHILD" = 1 ] && run_one PASS child succeed find-intact
echo "  [PASS] $NAME"

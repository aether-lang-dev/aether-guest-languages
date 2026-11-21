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
# Built with AddressSanitizer (quickjs/sanitize.sh) the loops take longer than
# the bridge's 5 s (and the child host's 5 s deadline), which then stops them: the engine's own case for that,
# test_quickjs_find_interrupt.ae, runs there instead.
if [ "${GHL_SANITIZER:-}" = 1 ]; then
    echo "  [SKIP] inproc find-intact: slower than the bridge's 5 s under AddressSanitizer"
else
    run_one PASS inproc succeed find-intact
fi
if [ "$CHILD" = 1 ] && [ "${GHL_SANITIZER:-}" = 1 ]; then
    echo "  [SKIP] child find-intact: slower than the host's 5 s deadline under AddressSanitizer"
elif [ "$CHILD" = 1 ]; then
    run_one PASS child succeed find-intact
fi
echo "  [PASS] $NAME"

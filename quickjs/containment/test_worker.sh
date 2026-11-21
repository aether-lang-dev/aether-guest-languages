#!/bin/sh
# QuickJS's own cases for its worker, beside the shared boundary tests
# (tests/boundary.py): worker_cases.ae has them, and why.
# Contained child only (Linux); GHL_REQUIRE_CHILD=1 makes its absence a
# failure, as stunt_lib.sh does.
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=quickjs_worker
GUEST_LANG=quickjs
. "$SCRIPT_DIR/stunt_lib.sh"

if [ "$CHILD" != 1 ]; then
    echo "  [SKIP] $NAME: no contained child here ($CHILD_WHY)"
    exit 0
fi
BHOST="$BUILD/quickjs-worker-cases"
build quickjs/containment/worker_cases.ae "$BHOST"
n=0
for c in cancel-catch loglevel fail-uncaught script-error unmounted host-types strings; do
    out="$TMP/worker-case.txt"
    (cd "$WORK" && $ROW_TIMEOUT "$BHOST" "$c") > "$out" 2>&1 || {
        echo "  [FAIL] $NAME: $c"
        sed 's/^/    /' "$out"
        exit 1
    }
    echo "  [PASS] child  $(tail -1 "$out")"
    n=$((n + 1))
done
echo "  [PASS] $NAME: $n cases"

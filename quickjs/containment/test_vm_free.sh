#!/bin/sh
# A freed VM's handle is refused: quickjs.vm_free's parameter is @frees, so
# the compiler follows the local and refuses a use or a second free after it
# (E0903), at build time, with no table of live VMs kept anywhere. A local
# bound afresh after the free builds. The fixtures are in fixtures/.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NAME=quickjs_vm_free
OUT="$ROOT/build/vm-free"
mkdir -p "$OUT"
fails=0
for f in use_after_free free_twice; do
    if (cd "$ROOT" && ae build "quickjs/containment/fixtures/$f.ae" -o "$OUT/$f") > "$OUT/$f.log" 2>&1; then
        echo "  [FAIL] $NAME: $f compiled"; fails=$((fails + 1))
    elif grep -q "E0903" "$OUT/$f.log"; then
        echo "  [PASS] $f: refused at build time ($(grep -m1 E0903 "$OUT/$f.log" | cut -c1-90))"
    else
        echo "  [FAIL] $NAME: $f failed to build, but not with E0903"; sed 's/^/    /' "$OUT/$f.log" | head -20; fails=$((fails + 1))
    fi
done
if (cd "$ROOT" && ae build quickjs/containment/fixtures/free_then_rebind.ae -o "$OUT/free_then_rebind") > "$OUT/rebind.log" 2>&1 && "$OUT/free_then_rebind"; then
    echo "  [PASS] free_then_rebind: a fresh VM in the same local builds and runs"
else
    echo "  [FAIL] $NAME: free_then_rebind"; sed 's/^/    /' "$OUT/rebind.log" | head -20; fails=$((fails + 1))
fi
[ "$fails" -eq 0 ] && echo "  [PASS] $NAME"
[ "$fails" -eq 0 ]

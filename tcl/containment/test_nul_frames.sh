#!/bin/sh
# A NUL crosses the contained child's frames whole: the worker's SAY text, the
# JSON arguments of a call (written \u0000, so no frame holds a zero byte), the
# host's answer read back, the script text itself with a raw NUL in it, and
# the error message that ends the script. nulframes.ae is the host (it prints
# what it received as decimal byte lists); this compares that with what the
# script says it should be. Until 2026-11-22 the worker refused every one of
# these (c753e03), so a run of this test on that code fails.

SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tcl_nul_frames
. "$SCRIPT_DIR/stunt_lib.sh"
[ "$CHILD" = 1 ] || skip_all "no contained child here: $CHILD_WHY"

(cd "$WORK" && $ROW_TIMEOUT "$CHILD_HOST" - nulframes x) > "$TMP/nulframes.txt" 2>&1 || die "the host failed" "$TMP/nulframes.txt"
# SAY lines are byte lists: "r=3", "raw=3" and "m=3" (the lengths the script
# measured of the answer, of the raw-NUL word in its own text, and of the caught error).
cat > "$TMP/expected.txt" <<'X'
SAY 97 0 98
CALL ["x\u0000y"]
SAY 114 61 51
SAY 114 97 119 61 51
SAY 109 61 51
END ERROR 102 105 110 0 97 108
X
if ! cmp -s "$TMP/expected.txt" "$TMP/nulframes.txt"; then
    diff "$TMP/expected.txt" "$TMP/nulframes.txt" | sed 's/^/    /'
    die "a frame with a NUL in it did not cross as expected" "$TMP/nulframes.txt"
fi
echo "  [PASS] $NAME: say, call arguments, an answer, the script text and the error message cross with their NULs"

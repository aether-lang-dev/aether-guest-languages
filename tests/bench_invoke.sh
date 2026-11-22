#!/bin/sh
# The cost of a call in each direction, and of the pull pattern, for one
# language's contained child (GUEST_LANG): 10,000 calls (BENCH_N) with 16-byte
# and 60 KB payloads, microseconds per call. Not part of tests/run.sh: a
# measurement, printed as "lang size g2h h2g pull".
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=bench
. "$SCRIPT_DIR/stunt_lib.sh"
[ "$CHILD" = 1 ] || skip_all "no contained child here: $CHILD_WHY"
python3 -I "$SCRIPT_DIR/boundary.py" bench "$LANG_NAME" "$CHILD_HOST" "$WORK" "${BENCH_N:-10000}"

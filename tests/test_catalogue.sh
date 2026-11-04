#!/bin/sh
# Every row of stunt's catalogue is accounted for by name: named in one of the
# test_stunt_*.sh lists beside this, or left out in stunt_lib.sh's left_out.
# A row stunt gains is a failure here until the language's lists and its
# README table say what it does with it.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=catalogue
. "$SCRIPT_DIR/stunt_lib.sh"

missing=""
for c in $(awk 'NR > 1 { print $1 }' "$CATALOGUE"); do
    if grep -q "^stunt $c " "$SCRIPT_DIR"/test_stunt_*.sh; then continue; fi
    left_out "$c" > /dev/null && continue
    missing="$missing $c"
done
for c in $(cat "$SCRIPT_DIR"/test_stunt_*.sh | awk '$1 == "stunt" { print $2 }' | sort -u); do
    [ -n "$(modes_of "$c")" ] || missing="$missing $c(not-in-catalogue)"
done
[ -z "$missing" ] || die "not accounted for:$missing"
echo "  [PASS] catalogue: all $(awk 'NR > 1' "$CATALOGUE" | wc -l | tr -d ' ') rows of stunt's catalogue ($AETHER_STUNT_REF) are named"

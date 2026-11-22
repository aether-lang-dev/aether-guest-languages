#!/bin/sh
# The shared tests in the other direction, the host calling the guest
# (std.guest's INVOKE; tests/boundary.py run-invoke, the host in
# tests/lib/invoke): a script per case that exports functions, written in the
# language (the x: snippets of <lang>/containment/boundary.tsv), run in its
# contained child with a plan of invocations. Records the outcomes in
# build/invoke-<lang>.tsv, from which tests/boundary_table.py renders and checks
# the language README's "Host calls guest" grid and its safety rows, and
# tests/readme_tables.sh the first table's columns for this direction.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=invoke
. "$SCRIPT_DIR/stunt_lib.sh"
rm -f "$BUILD/invoke-$LANG_NAME.tsv"
command -v python3 > /dev/null 2>&1 || skip_all "no python3"
[ "$CHILD" = 1 ] || skip_all "no contained child here: $CHILD_WHY"
grep -q '^x:back' "$ROOT/$LANG_NAME/containment/boundary.tsv" 2> /dev/null ||
    skip_all "$LANG_NAME's worker has nothing to export (its README says why)"
python3 -I "$SCRIPT_DIR/boundary.py" run-invoke "$LANG_NAME" "$CHILD_HOST" "$WORK" "$BUILD/invoke-$LANG_NAME.tsv" ||
    die "boundary.py run-invoke failed"
# The safety rows are held by name: one that is not held fails the run here,
# as a stunt row does, besides showing in the README's grid.
bad="$(awk -F '\t' '$1 ~ /^guest-/ && $3 ~ /^not held/' "$BUILD/invoke-$LANG_NAME.tsv")"
[ -z "$bad" ] || die "a host-calls-guest row is not held: $bad"
echo "  [PASS] $NAME: $(wc -l < "$BUILD/invoke-$LANG_NAME.tsv" | tr -d ' ') cases run in $LANG_NAME's child, the host calling the guest"

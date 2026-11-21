#!/bin/sh
# The shared boundary tests (tests/boundary.py) against the language's
# contained child: what types, callbacks, errors and cancels cross, found by
# running one script per case in the language's own worker. Records the
# outcomes in build/boundary-<lang>.tsv, from which tests/boundary_table.py
# renders and checks the README's "What crosses the boundary" table.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=boundary
. "$SCRIPT_DIR/stunt_lib.sh"
rm -f "$BUILD/boundary-$LANG_NAME.tsv"
[ "$CHILD" = 1 ] || skip_all "no contained child here: $CHILD_WHY"
command -v python3 > /dev/null 2>&1 || skip_all "no python3"
[ -f "$ROOT/$LANG_NAME/containment/boundary.tsv" ] || skip_all "$LANG_NAME has no snippets (its worker mounts no host functions)"
python3 -I "$SCRIPT_DIR/boundary.py" run "$LANG_NAME" "$CHILD_HOST" "$WORK" "$BUILD/boundary-$LANG_NAME.tsv" ||
    die "boundary.py failed"
echo "  [PASS] $NAME: $(wc -l < "$BUILD/boundary-$LANG_NAME.tsv" | tr -d ' ') cases run in $LANG_NAME's child"

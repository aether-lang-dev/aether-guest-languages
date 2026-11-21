#!/bin/sh
# The shared boundary tests (tests/boundary.py), twice for a language: against
# its bridge in this process (<lang>/containment/boundary_host.ae, through the
# API the bridge really has) and against its contained child (found by running
# one script per case in the language's own worker). Records the outcomes in
# build/boundary-inproc-<lang>.tsv and build/boundary-<lang>.tsv, from which
# tests/boundary_table.py renders and checks each language README's "What
# crosses the boundary" grids, and tests/readme_tables.sh the first table.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STUNT_TEST=boundary
. "$SCRIPT_DIR/stunt_lib.sh"
rm -f "$BUILD/boundary-$LANG_NAME.tsv" "$BUILD/boundary-inproc-$LANG_NAME.tsv"
command -v python3 > /dev/null 2>&1 || skip_all "no python3"

# In this process, whatever the platform: nothing here needs a contained child.
if [ -f "$ROOT/$LANG_NAME/containment/boundary_host.ae" ]; then
    IHOST="$BUILD/$LANG_NAME-boundary-host"
    build "$LANG_NAME/containment/boundary_host.ae" "$IHOST"
    python3 -I "$SCRIPT_DIR/boundary.py" run-inproc "$LANG_NAME" "$IHOST" "$WORK" "$BUILD/boundary-inproc-$LANG_NAME.tsv" ||
        die "boundary.py run-inproc failed"
    echo "  [PASS] $NAME: $(wc -l < "$BUILD/boundary-inproc-$LANG_NAME.tsv" | tr -d ' ') cases run in $LANG_NAME's bridge, in this process"
else
    echo "  [SKIP] $NAME: $LANG_NAME has no in-process boundary host"
fi

[ "$CHILD" = 1 ] || skip_all "no contained child here: $CHILD_WHY"
[ -f "$ROOT/$LANG_NAME/containment/boundary.tsv" ] || skip_all "$LANG_NAME has no snippets (its worker mounts no host functions)"
python3 -I "$SCRIPT_DIR/boundary.py" run "$LANG_NAME" "$CHILD_HOST" "$WORK" "$BUILD/boundary-$LANG_NAME.tsv" ||
    die "boundary.py failed"
echo "  [PASS] $NAME: $(wc -l < "$BUILD/boundary-$LANG_NAME.tsv" | tr -d ' ') cases run in $LANG_NAME's child"

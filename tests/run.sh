#!/bin/sh
# Runs everything, for every language here: the bridge's specs, then the
# containment rows, then the check that the language's README "Stunt parity"
# table says what this run did.
#
#   tests/run.sh                    all of it
#   tests/run.sh spec               the specs only
#   tests/run.sh containment        the catalogue and the test_stunt_*.sh lists
#   tests/run.sh --write-parity     all of it, and rewrite the README tables'
#                                   columns this host ran instead of checking
#   GUEST_LANG=lua tests/run.sh     one language (each <lang>/containment/lang.sh)
#
# Needs `ae` on PATH and each language's runtime (<lang>/containment/lang.sh
# says how it is found: tests/lua_env.sh, tests/python_env.sh). On GitHub
# Actions a spec that skips for want of the runtime is a failure.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
parity=check
what=all
for a in "$@"; do
    case "$a" in
        --write-parity) parity=write ;;
        *) what="$a" ;;
    esac
done
fails=0

# Which host produced a column of the parity table.
host_text() {
    case "$(uname -s)" in
        Darwin) os="macOS $(sw_vers -productVersion 2>/dev/null)" ;;
        Linux) os="$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-Linux}"), kernel $(uname -r | cut -d- -f1)" ;;
        *) os="$(uname -sr)" ;;
    esac
    echo "$os $(uname -m), ae $(ae version 2>/dev/null | awk 'NR == 1 { print $2 }'), $1"
}

# Every language with a <lang>/containment/lang.sh, unless GUEST_LANG names one.
langs="${GUEST_LANG:-$(for f in "$ROOT"/*/containment/lang.sh; do basename "$(dirname "$(dirname "$f")")"; done)}"
for lang in $langs; do
    export GUEST_LANG="$lang"
    . "$ROOT/$lang/containment/lang.sh"

    if [ "$what" = all ] || [ "$what" = spec ]; then
        lang_runtime || echo "  [SKIP] no $LANG_TITLE runtime found: the specs will skip"
        echo "$LANG_TITLE runtime: $(eval echo "\${$LANG_RUNTIME_ENV:-none}")"
        mkdir -p "$ROOT/build"
        for spec in $LANG_SPECS; do
            bin="$ROOT/build/$(basename "$spec" .ae)"
            if ! (cd "$ROOT" && ae build "$spec" -o "$bin") > "$ROOT/build/spec.log" 2>&1; then
                cat "$ROOT/build/spec.log"; fails=$((fails + 1)); continue
            fi
            out="$("$bin" 2>&1)"; rc=$?
            echo "$out"
            [ "$rc" -eq 0 ] || fails=$((fails + 1))
            if [ "${GITHUB_ACTIONS:-}" = true ] && echo "$out" | grep -qi "skipped"; then
                echo "  [FAIL] $spec skipped on a runner that has $LANG_TITLE"; fails=$((fails + 1))
            fi
        done
    fi

    if [ "$what" = all ] || [ "$what" = containment ]; then
        mkdir -p "$ROOT/build"
        : > "$ROOT/build/parity-$lang.tsv"
        lang_runtime && host_text "$LANG_TITLE $(lang_version 2>/dev/null)" > "$ROOT/build/parity-$lang.host"
        for t in "$SCRIPT_DIR"/test_catalogue.sh "$SCRIPT_DIR"/test_stunt_*.sh $LANG_EXTRA_TESTS; do
            echo "== $(basename "$t")"
            sh "$t" || fails=$((fails + 1))
        done
        echo "== parity ($lang)"
        sh "$SCRIPT_DIR/parity.sh" "$parity" "$lang" || fails=$((fails + 1))
    fi
done

[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
[ "$fails" -eq 0 ]

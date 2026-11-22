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

# Every worker is built on std.guest's host-calls-guest verb: an ae without it
# is said once, plainly, instead of as a build error in every test.
ROOT="$ROOT" sh "$SCRIPT_DIR/need_invoke.sh" || { echo "1 FAILED"; exit 1; }

# Which host produced a column of the parity table.
host_text() {
    case "$(uname -s)" in
        Darwin) os="macOS $(sw_vers -productVersion 2>/dev/null)" ;;
        Linux) os="$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-Linux}"), kernel $(uname -r | cut -d- -f1)" ;;
        *) os="$(uname -sr)" ;;
    esac
    echo "$os $(uname -m), ae $(ae version 2>/dev/null | awk 'NR == 1 { print $2 }'), $1"
}

# libaether_sandbox.so beside ae's libaether.a (a release install keeps it in
# lib/aether/ or lib/): the specs' programs are built and run directly, not by
# `ae run`, so nothing sets AETHER_HOME for them and the bridges' lookup (which
# honours AETHER_SANDBOX_PRELOAD first) would miss it. stunt_lib.sh does the same.
case "$(uname -s)" in Linux|FreeBSD)
    if [ -z "${AETHER_SANDBOX_PRELOAD:-}" ]; then
        libdir="$(ae cflags --libs 2>/dev/null | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)"
        [ -n "$libdir" ] && [ -f "$libdir/libaether_sandbox.so" ] && export AETHER_SANDBOX_PRELOAD="$libdir/libaether_sandbox.so"
    fi ;;
esac

# Every language with a <lang>/containment/lang.sh, unless GUEST_LANG names one.
langs="${GUEST_LANG:-$(for f in "$ROOT"/*/containment/lang.sh; do basename "$(dirname "$(dirname "$f")")"; done)}"
for lang in $langs; do
    export GUEST_LANG="$lang"
    # Set by a lang.sh whose programs import a library built elsewhere (Rexx).
    LANG_LIB=""
    . "$ROOT/$lang/containment/lang.sh"

    if [ "$what" = all ] || [ "$what" = spec ]; then
        lang_runtime || echo "  [SKIP] no $LANG_TITLE runtime found: the specs will skip"
        echo "$LANG_TITLE runtime: $(eval echo "\${$LANG_RUNTIME_ENV:-none}")"
        # A CI job says which runtime it has (GHL_CI_RUNTIME, which the README's
        # "CI today" column is made from). On Actions that claim is checked
        # against what was found: its version, or GHL_CI_RUNTIME_MATCH, a
        # substring of the runtime's path or version; "-" says the runtime
        # names no version to check.
        if [ -n "${GHL_CI_RUNTIME:-}" ] && [ "${GITHUB_ACTIONS:-}" = true ] && [ "${GHL_CI_RUNTIME_MATCH:-}" != "-" ]; then
            seen="$(eval echo "\${$LANG_RUNTIME_ENV:-none}") $(lang_version 2>/dev/null)"
            need="${GHL_CI_RUNTIME_MATCH:-$(echo "$GHL_CI_RUNTIME" | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)}"
            case "$seen" in
                *"$need"*) echo "  [PASS] CI runtime: $GHL_CI_RUNTIME (found $need in: $seen)" ;;
                *) echo "  [FAIL] CI runtime: the job says \"$GHL_CI_RUNTIME\" but the runtime found is: $seen"; fails=$((fails + 1)) ;;
            esac
        fi
        mkdir -p "$ROOT/build"
        # A language whose specs need its side built first (Java) says how.
        if type lang_spec_env > /dev/null 2>&1; then lang_spec_env || fails=$((fails + 1)); fi
        for spec in $LANG_SPECS; do
            bin="$ROOT/build/$(basename "$spec" .ae)"
            if ! (cd "$ROOT" && ae build "$spec" ${LANG_LIB:+--lib "$LANG_LIB"} -o "$bin") > "$ROOT/build/spec.log" 2>&1; then
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
        for t in "$SCRIPT_DIR"/test_catalogue.sh "$SCRIPT_DIR"/test_stunt_*.sh "$SCRIPT_DIR"/test_boundary.sh "$SCRIPT_DIR"/test_invoke.sh $LANG_EXTRA_TESTS; do
            echo "== $(basename "$t")"
            sh "$t" || fails=$((fails + 1))
        done
        echo "== boundary table ($lang)"
        if command -v python3 > /dev/null 2>&1; then
            for m in "" inproc invoke; do
                if [ "$parity" = write ]; then python3 -I "$SCRIPT_DIR/boundary_table.py" write "$lang" $m; else python3 -I "$SCRIPT_DIR/boundary_table.py" check "$lang" $m || fails=$((fails + 1)); fi
            done
        fi
        echo "== parity ($lang)"
        # A language may say its README table is not for this host's build
        # (lang_parity_ok in its lang.sh: Go without cgo runs fewer rows).
        if type lang_parity_ok > /dev/null 2>&1 && ! lang_parity_ok; then
            echo "  [SKIP] parity ($lang): the README's table is not for this build"
        else
            sh "$SCRIPT_DIR/parity.sh" "$parity" "$lang" || fails=$((fails + 1))
        fi
    fi
done

# The README's language table: its generated columns against ci.yml and the
# parity tables (the check reads the sources, not this host).
if [ "$what" = all ] || [ "$what" = containment ]; then
    echo "== readme tables"
    if [ "$parity" = write ]; then sh "$SCRIPT_DIR/readme_tables.sh" write; else sh "$SCRIPT_DIR/readme_tables.sh" check || fails=$((fails + 1)); fi
fi

[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
[ "$fails" -eq 0 ]

#!/bin/sh
# Runs everything: the Lua bridge's specs, then the containment rows.
#
#   tests/run.sh                 all of it
#   tests/run.sh spec            the specs only
#   tests/run.sh containment     the catalogue and the test_stunt_*.sh lists
#
# Needs `ae` on PATH and a Lua runtime (tests/lua_env.sh finds it, or set
# AETHER_LUA_SONAME; LUA_VERSION=5.4 picks one). On GitHub Actions a spec that
# skips for want of Lua is a failure.
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$SCRIPT_DIR/lua_env.sh"
what="${1:-all}"
fails=0

if [ "$what" = all ] || [ "$what" = spec ]; then
    lua_soname || echo "  [SKIP] no Lua runtime found: the specs will skip"
    echo "Lua runtime: ${AETHER_LUA_SONAME:-none}"
    mkdir -p "$ROOT/build"
    for spec in lua/test_host_lua.ae lua/test_host_lua_vm.ae; do
        bin="$ROOT/build/$(basename "$spec" .ae)"
        if ! (cd "$ROOT" && ae build "$spec" -o "$bin") > "$ROOT/build/spec.log" 2>&1; then
            cat "$ROOT/build/spec.log"; fails=$((fails + 1)); continue
        fi
        out="$("$bin" 2>&1)"; rc=$?
        echo "$out"
        [ "$rc" -eq 0 ] || fails=$((fails + 1))
        if [ "${GITHUB_ACTIONS:-}" = true ] && echo "$out" | grep -qi "skipped"; then
            echo "  [FAIL] $spec skipped on a runner that has Lua"; fails=$((fails + 1))
        fi
    done
fi

if [ "$what" = all ] || [ "$what" = containment ]; then
    for t in "$SCRIPT_DIR"/test_catalogue.sh "$SCRIPT_DIR"/test_stunt_*.sh; do
        echo "== $(basename "$t")"
        sh "$t" || fails=$((fails + 1))
    done
fi

[ "$fails" -eq 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
[ "$fails" -eq 0 ]

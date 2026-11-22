#!/bin/sh
# QuickJS's bridge, worker and hosts built with AddressSanitizer and
# LeakSanitizer, and run: the specs, every containment row (in-process and in
# the contained child), and the worker cases. Linux, with clang:
# clang links its ASan runtime into the program, so it comes first whatever
# the sandbox preload (LD_PRELOAD) puts in front of it, which gcc's shared
# libasan refuses.
#
#   sh quickjs/sanitize.sh
#
# A report from a host process is a file under build/sanitize/ (ASan's
# log_path); the contained worker cannot write one (Landlock), so its report
# would be on stderr and would fail the row it ran. LeakSanitizer cannot run
# inside the contained child (its seccomp filter refuses the fork it starts
# with: "TestPTrace() failed to fork"), so it is off in a process under a
# seccomp filter (sanitize_options.c, linked into every program built here),
# and the worker's own cases and the shared boundary tests are run a second
# time with the worker uncontained (GHL_UNCONTAINED=1), where it is on. Exits
# 0 when everything passed and no sanitizer said anything.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
command -v clang > /dev/null 2>&1 || { echo "  [SKIP] sanitize: no clang"; exit 0; }
[ "$(uname -s)" = Linux ] || { echo "  [SKIP] sanitize: Linux only"; exit 0; }
OUT="$ROOT/build/sanitize"
rm -rf "$OUT"
mkdir -p "$OUT"
# Binaries are rebuilt only when older than their sources: start clean, and
# leave clean, so an ordinary run never picks up an instrumented one.
clean() {
    rm -f "$ROOT"/build/quickjs-* "$ROOT"/build/test_host_quickjs* "$ROOT"/build/test_quickjs_*
}
clean
export AE_CC=clang
clang -c -O1 -o "$OUT/sanitize_options.o" "$ROOT/quickjs/sanitize_options.c" || exit 1
export AETHER_CFLAGS_EXTRA="-fsanitize=address -fno-omit-frame-pointer -g -O1"
export AETHER_LDFLAGS_EXTRA="-fsanitize=address $OUT/sanitize_options.o"
export AETHER_CACHE_DIR="$OUT/cache"
# detect_leaks is the default options' (sanitize_options.c); the environment
# reaches only the hosts (guest.launch gives the worker none).
export ASAN_OPTIONS="halt_on_error=1:log_path=$OUT/report"
export GHL_SANITIZER=1
fails=0
GHL_REQUIRE_CHILD=1 GUEST_LANG=quickjs sh "$ROOT/tests/run.sh" > "$OUT/run.log" 2>&1 || fails=$((fails + 1))
grep -E "passing|failing|\[FAIL\]|ALL PASSED|FAILED$" "$OUT/run.log" | sed 's/^/  /'

# The worker uncontained, so LeakSanitizer runs in it too.
. "$ROOT/tests/quickjs_env.sh"
export QUICKJS_WORKER="$ROOT/build/quickjs-worker"
WORK="$OUT/work"
mkdir -p "$WORK"
n=0
for c in types host-types nested fail-caught fail-uncaught script-error cancel-loop cancel-catch cancel-in-call loglevel unmounted strings; do
    if (cd "$WORK" && GHL_UNCONTAINED=1 "$ROOT/build/quickjs-worker-cases" "$c") > "$OUT/uncontained-$c.txt" 2>&1 &&
       ! grep -q "Sanitizer" "$OUT/uncontained-$c.txt"; then
        n=$((n + 1))
    else
        echo "  [FAIL] uncontained $c"; sed 's/^/    /' "$OUT/uncontained-$c.txt" | head -40; fails=$((fails + 1))
    fi
done
echo "  uncontained worker, LeakSanitizer on: $n of 12 worker cases clean"
if GHL_UNCONTAINED=1 GUEST_LANG=quickjs sh "$ROOT/tests/test_boundary.sh" > "$OUT/uncontained-boundary.txt" 2>&1 &&
   ! grep -q "Sanitizer" "$OUT/uncontained-boundary.txt"; then
    echo "  uncontained worker, LeakSanitizer on: $(tail -1 "$OUT/uncontained-boundary.txt" | sed 's/^ *//')"
else
    echo "  [FAIL] uncontained shared boundary tests"; sed 's/^/    /' "$OUT/uncontained-boundary.txt" | tail -30; fails=$((fails + 1))
fi

# The host calling the guest, uncontained: the serving loop, every type both
# ways, the errors and the safety rows, with LeakSanitizer on in the worker.
if GHL_UNCONTAINED=1 GUEST_LANG=quickjs sh "$ROOT/tests/test_invoke.sh" > "$OUT/uncontained-invoke.txt" 2>&1 &&
   ! grep -q "Sanitizer" "$OUT/uncontained-invoke.txt"; then
    echo "  uncontained worker, LeakSanitizer on: $(tail -1 "$OUT/uncontained-invoke.txt" | sed 's/^ *//')"
else
    echo "  [FAIL] uncontained host-calls-guest tests"; sed 's/^/    /' "$OUT/uncontained-invoke.txt" | tail -30; fails=$((fails + 1))
fi

reports="$(ls "$OUT"/report.* 2>/dev/null | wc -l | tr -d ' ')"
echo "  sanitizer reports from host processes: $reports"
for f in "$OUT"/report.*; do [ -f "$f" ] && { echo "  == $f"; head -40 "$f"; }; done
[ "$reports" = 0 ] || fails=$((fails + 1))
clean
[ "$fails" -eq 0 ] && echo "SANITIZE CLEAN" || echo "SANITIZE: $fails problem(s)"
[ "$fails" -eq 0 ]

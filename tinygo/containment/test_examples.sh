#!/bin/sh
# The four examples of tinygo/README.md, built and run, and what each prints
# on this host checked: way 1 everywhere, ways 2 and 3 as the same program
# without and with the preload (3 on Linux), way 4 in a contained child
# (Linux with Landlock; on macOS it must refuse and start nothing).
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tinygo_examples
GUEST_LANG=tinygo
. "$SCRIPT_DIR/stunt_lib.sh"

EX="$ROOT/tinygo/examples"
fail() { echo "  [FAIL] $NAME: $1"; [ -f "$2" ] && sed 's/^/    /' "$2" | tail -20; exit 1; }
has() { grep -Fq "$1" "$2" || fail "expected \"$1\"" "$2"; }

build_ex() {   # build_ex <name>
    (cd "$ROOT" && ae build "tinygo/examples/$1.ae" -o "$BUILD/$1") > "$TMP/b.log" 2>&1 || fail "$1.ae did not build" "$TMP/b.log"
}

# Way 1
build_ex way1_embed
TINYGO_LIB="$TINYGO_LIB" "$BUILD/way1_embed" > "$TMP/w1.txt" 2>&1 || fail "way 1 exited non-zero" "$TMP/w1.txt"
has "Answer = 42" "$TMP/w1.txt"
has "Add(2, 40) = 42" "$TMP/w1.txt"
has "hello, world" "$TMP/w1.txt"

# Way 2: a sandbox block granting HOME only, no preload. Nothing Go does is held.
build_ex way2_sandbox
[ "$PLAT" = macos ] && export TINYGO_LIBC=libSystem.B.dylib
(cd "$WORK" && env -u LD_PRELOAD USER=tester HOME=/home/tester TINYGO_LIB="$TINYGO_PROBE_LIB" "$BUILD/way2_sandbox") > "$TMP/w2.txt" 2>&1 || fail "way 2 exited non-zero" "$TMP/w2.txt"
has "os.Getenv(USER)            tester" "$TMP/w2.txt"
has "libc getenv(USER) (cgo)    tester" "$TMP/w2.txt"
has "os.ReadFile(/etc/passwd)   opened" "$TMP/w2.txt"
has "os/exec /bin/sh            ran /bin/sh" "$TMP/w2.txt"
has "dlopen libc (cgo)          loaded" "$TMP/w2.txt"

# Way 3: the same program under the preload. Only libc's own calls are held.
if [ -n "$PRELOAD" ]; then
    (cd "$WORK" && USER=tester HOME=/home/tester LD_PRELOAD="$PRELOAD" TINYGO_LIB="$TINYGO_PROBE_LIB" "$BUILD/way2_sandbox") > "$TMP/w3.txt" 2>&1 || fail "way 3 exited non-zero" "$TMP/w3.txt"
    has "os.Getenv(USER)            tester" "$TMP/w3.txt"
    has "libc getenv(USER) (cgo)    (NULL)" "$TMP/w3.txt"
    has "libc getenv(HOME) (cgo)    /home/tester" "$TMP/w3.txt"
    has "os.ReadFile(/etc/passwd)   opened" "$TMP/w3.txt"
    has "os/exec /bin/sh            ran /bin/sh" "$TMP/w3.txt"
    has "let through" "$TMP/w3.txt"
fi

# Way 4
build_ex way4_child
build "tinygo/worker/worker.ae" "$BUILD/tinygo-worker"
(cd "$WORK" && TINYGO_WORKER="$BUILD/tinygo-worker" TINYGO_LIB="$TINYGO_PROBE_LIB" "$BUILD/way4_child") > "$TMP/w4.txt" 2>&1
if [ "$CHILD" = 1 ]; then
    has "(NULL)" "$TMP/w4.txt"
    has "open /etc/passwd: permission denied" "$TMP/w4.txt"
    has "fork/exec /bin/sh: operation not permitted" "$TMP/w4.txt"
    has "socket: operation not permitted" "$TMP/w4.txt"
    has "refused" "$TMP/w4.txt"
    has "DONE" "$TMP/w4.txt"
elif [ "$PLAT" = macos ]; then
    has "not started: " "$TMP/w4.txt"
fi
echo "  [PASS] $NAME"

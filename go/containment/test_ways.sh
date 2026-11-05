#!/bin/sh
# Go's own: what the four ways are for a program that starts a process. Not
# rows of stunt's catalogue, but what decides which way holds a Go program.
#
#   way 1  go.run: no containment, whatever the platform
#   way 2  go.run_sandboxed on macOS: there is no libaether_sandbox.so, so it
#          starts nothing (fails closed, as std.guest's launch does)
#   way 3  go.run_sandboxed on Linux, a static Go binary (CGO_ENABLED=0): the
#          loader that would preload the library never runs, so nothing it
#          interposes is ever called
#   way 4  a static worker in the contained child: Landlock and seccomp hold
#          it, with no libc and no files read after its start
#
# The hosts are the containment tests' (host.ae, child_host.ae); the rows are
# go/rows, the static builds are the same sources with CGO_ENABLED=0.
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=go_ways
GUEST_LANG=go
. "$SCRIPT_DIR/stunt_lib.sh"

# ---- way 1: go.run is not contained -----------------------------------------
# Under the preload, libc's getenv is refused to a child with no env grant
# (the stunt_no_grants test); go.run starts the same binary with nothing.
policy "-"
if [ "$GO_CGO" = 1 ]; then
    GHL_GO_UNCONTAINED=1 run_one PASS inproc succeed getenv-home
else
    GHL_GO_UNCONTAINED=1 run_one PASS inproc succeed open-etc-passwd
fi

# ---- way 2: macOS starts nothing --------------------------------------------
if [ "$PLAT" = macos ]; then
    unset GHL_GO_UNCONTAINED
    (cd "$WORK" && "$HOST" inproc - open-etc-passwd refused) > "$TMP/refuse.txt" 2>&1
    rc=$?
    [ "$rc" = 3 ] && grep -q "the child did not start" "$TMP/refuse.txt" ||
        die "go.run_sandboxed on macOS should start nothing and return -1 (exit $rc)" "$TMP/refuse.txt"
    echo "  [PASS] inproc go.run_sandboxed on macOS: returned -1 and started nothing"
fi

# ---- the static builds ------------------------------------------------------
STATIC_CHECKS="$BUILD/go-checks-static"
STATIC_WORKER="$BUILD/go-worker-static"
if [ "$PLAT" = linux ]; then
    (cd "$ROOT/go" && CGO_ENABLED=0 "$GO" build -o "$STATIC_CHECKS" ./containment &&
        CGO_ENABLED=0 "$GO" build -o "$STATIC_WORKER" ./worker) > "$TMP/static.log" 2>&1 ||
        die "the static builds failed" "$TMP/static.log"
    if ldd "$STATIC_CHECKS" 2>&1 | grep -q "not a dynamic"; then :; else
        die "the CGO_ENABLED=0 build is not static: $(ldd "$STATIC_CHECKS" 2>&1 | head -2)"
    fi
    echo "  [PASS] CGO_ENABLED=0: a static binary, nothing for LD_PRELOAD to be loaded into"

    # ---- way 3, static: the preload is there and holds nothing ----------------
    GO_CHECKS="$STATIC_CHECKS"
    export GO_CHECKS
    policy "-"
    # Under the grants, with the library named in LD_PRELOAD: still opened.
    run_one ESCAPES inproc succeed open-etc-passwd
    # The same row in the cgo build is not held either (the rows' table says
    # why); a libc call is: getenv-home, which a static Go cannot make.
    run_one ESCAPES inproc succeed exec-bin-sh
    GO_CHECKS="$BUILD/go-checks"
    export GO_CHECKS

    # ---- way 4, static: the contained child holds a worker with no libc -------
    if [ "$CHILD" = 1 ]; then
        GO_WORKER="$STATIC_WORKER"
        export GO_WORKER
        policy "-"
        run_one PASS child refused open-etc-passwd
        run_one PASS child refused syscall-openat
        run_one PASS child refused syscall-socket
        run_one PASS child refused exec-bin-sh
        run_one PASS child refused connect-192.0.2.1
        run_one PASS child refused binding-read-etc-passwd
        run_one PASS child succeed callback-greet
        run_one PASS child succeed callback-add
        policy "fs_read=$WORK/stunt-fixture.txt"
        run_one PASS child succeed binding-read-granted-file
        # Landlock is the spawn's, made from the same grants: a program that
        # opens the granted file itself may (the dynamic worker of the rows
        # above is stricter, child_stricter in lang.sh), and nothing beside it.
        run_one PASS child succeed open-granted-file
        run_one PASS child refused open-etc-passwd
        GO_WORKER="$BUILD/go-worker"
        export GO_WORKER
    fi
fi
# ---- the programs in README.md's four ways ---------------------------------
(cd "$ROOT/go" && "$GO" build -o "$BUILD/go-hello" ./examples/hello &&
    CGO_ENABLED=0 "$GO" build -o "$BUILD/go-example-worker" ./examples/worker) > "$TMP/ex.log" 2>&1 ||
    die "the examples did not build" "$TMP/ex.log"
# contained.ae imports std.guest, which only an ae at AE_CHILD_PIN or later has.
(cd "$ROOT" && ae build go/examples/ways.ae --lib . -o "$BUILD/go-ways" &&
    { ! ver_ge "$ae_ver" "$AE_CHILD_PIN" || ae build go/examples/contained.ae -o "$BUILD/go-contained"; }) > "$TMP/ex.log" 2>&1 ||
    die "the examples' hosts did not build" "$TMP/ex.log"
(cd "$WORK" && "$BUILD/go-ways" "$BUILD/go-hello") > "$TMP/ways.txt" 2>&1
grep -q "^go.run: 0" "$TMP/ways.txt" || die "examples/ways.ae: go.run" "$TMP/ways.txt"
if [ "$PLAT" = macos ]; then
    grep -q "^go.run_sandboxed: -1" "$TMP/ways.txt" || die "examples/ways.ae: go.run_sandboxed on macOS" "$TMP/ways.txt"
else
    grep -q "^go.run_sandboxed: 0" "$TMP/ways.txt" || die "examples/ways.ae: go.run_sandboxed" "$TMP/ways.txt"
    # Both runs read /etc/passwd: the preload does not hold Go's own open.
    [ "$(grep -c '^/etc/passwd: read it' "$TMP/ways.txt")" = 2 ] || die "examples/ways.ae: the sandboxed child should read /etc/passwd too" "$TMP/ways.txt"
fi
echo "  [PASS] examples/ways.ae: ways 1 to 3 as README.md shows them"
if ! ver_ge "$ae_ver" "$AE_CHILD_PIN"; then
    echo "  [SKIP] $NAME: examples/contained.ae: $CHILD_WHY"
elif [ "$CHILD" = 1 ]; then
    (cd "$WORK" && "$BUILD/go-contained" "$BUILD/go-example-worker") > "$TMP/contained.txt" 2>&1
    grep -q '^the host answered VALUE "hello, Go"' "$TMP/contained.txt" &&
        grep -q 'open /etc/passwd: permission denied' "$TMP/contained.txt" &&
        grep -q '^DONE 0 ' "$TMP/contained.txt" || die "examples/contained.ae" "$TMP/contained.txt"
    echo "  [PASS] examples/contained.ae: way 4 as README.md shows it"
fi
echo "  [PASS] $NAME"

#!/bin/sh
# Java's own: what the four ways are for a Java program. Not rows of stunt's
# catalogue, but what decides which way holds a JVM, and the parts of the Java
# side the rows do not reach.
#
#   the agent     the grant checker holds to the one matcher, and the agent's two
#                 shared-memory readers work (GrantCheckerTest, AgentTest)
#   way 1         java.run: no containment, whatever the platform
#   way 2         java.run_sandboxed on macOS: there is no libaether_sandbox.so,
#                 so it starts nothing (fails closed, as std.guest's launch does)
#   ways 2 and 3  on Linux the preload refuses a JVM's executable memory: the JVM
#                 dies at startup, with BLOCK_MARK (checked by tests/stunt_lib.sh
#                 for every row; here for the bridge's own call)
#   what libc sees  libghltrace.so, a recording preload that refuses nothing:
#                 which of Java's own ways to a file, a socket, a process and a
#                 library are libc calls an interposer would see
#   the examples  the programs in README.md's four ways
#
# The hosts are the containment tests' (host.ae, child_host.ae); the rows are
# java/rows.
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=java_ways
GUEST_LANG=java
. "$SCRIPT_DIR/stunt_lib.sh"

JAVA_RUN="$JAVA_BIN $AETHER_JAVA_FLAGS -cp $JAVA_CLASSES"

# ---- the agent --------------------------------------------------------------
$JAVA_RUN ghl.containment.GrantCheckerTest > "$TMP/gc.txt" 2>&1 || die "GrantCheckerTest" "$TMP/gc.txt"
cat "$TMP/gc.txt"
$JAVA_RUN ghl.containment.AgentTest > "$TMP/agent.txt" 2>&1 || die "AgentTest" "$TMP/agent.txt"
cat "$TMP/agent.txt"

# ---- way 1: java.run is not contained ---------------------------------------
# Under the preload, libc's getenv is refused to a child with no env grant
# (the stunt_no_grants test); java.run starts the same program with nothing.
plain() {   # plain <row> <expect>: the row through java.run, whatever the platform
    (cd "$WORK" && env -u LD_PRELOAD GHL_JAVA_UNCONTAINED=1 "$HOST" inproc - "$1" "$2") > "$TMP/plain.txt" 2>&1 ||
        die "java.run: $1" "$TMP/plain.txt"
    printf '  [PASS] java.run %s\n' "$(tail -1 "$TMP/plain.txt")"
}
plain getenv-home succeed
plain open-etc-passwd succeed
plain exec-bin-sh succeed

# ---- way 2: macOS starts nothing --------------------------------------------
if [ "$PLAT" = macos ]; then
    (cd "$WORK" && env -u GHL_JAVA_UNCONTAINED "$HOST" inproc - open-etc-passwd refused) > "$TMP/refuse.txt" 2>&1
    rc=$?
    [ "$rc" = 3 ] && grep -q "the child did not start" "$TMP/refuse.txt" ||
        die "java.run_sandboxed on macOS should start nothing and return -1 (exit $rc)" "$TMP/refuse.txt"
    echo "  [PASS] java.run_sandboxed on macOS: returned -1 and started nothing"
fi

# ---- ways 2 and 3 on Linux: the JVM dies under the preload ------------------
if [ "$PLAT" = linux ] && [ -n "$PRELOAD" ]; then
    (cd "$WORK" && env -u GHL_JAVA_UNCONTAINED "$HOST" inproc - open-etc-passwd refused) > "$TMP/blocked.txt" 2>&1
    rc=$?
    if [ "$BLOCKED_INPROC" = 1 ]; then
        [ "$rc" != 0 ] && grep -q "$BLOCK_MARK" "$TMP/blocked.txt" ||
            die "java.run_sandboxed should start a JVM that dies with '$BLOCK_MARK' (exit $rc)" "$TMP/blocked.txt"
        [ "$(grep -c "$BLOCK_MARK" "$TMP/blocked.txt")" -ge 1 ] &&
            echo "  [PASS] java.run_sandboxed on Linux: the JVM started (the bridge did not return -1) and died: $BLOCK_MARK"
    fi
fi

# ---- what libc sees (Linux) -------------------------------------------------
if [ "$PLAT" = linux ] && command -v cc > /dev/null 2>&1; then
    cc -shared -fPIC -O1 -o "$BUILD/libghltrace.so" "$ROOT/java/containment/libghltrace.c" -ldl > "$TMP/cc.log" 2>&1 ||
        die "libghltrace.c did not build" "$TMP/cc.log"
    traced() {   # traced <row> <expect> <trace line to find, or "!" and a line not to find>
        : > "$TMP/trace.txt"
        (cd "$WORK" && env GHL_TRACE="$TMP/trace.txt" LD_PRELOAD="$BUILD/libghltrace.so" \
            GHL_HOME_SET=1 GHL_SH=1 GHL_LIBM="$GHL_LIBM" GHL_LIBC_NAME="$GHL_LIBC_NAME" GHL_LIBC_PATH="$GHL_LIBC_PATH" \
            $JAVA_RUN ghl.containment.Checks "$1" "$2") > "$TMP/traced.txt" 2>&1 ||
            die "traced $1" "$TMP/traced.txt"
        rm -f "$WORK/stunt-written.txt"
        case "$3" in
            '!'*) if grep -q "^${3#!}" "$TMP/trace.txt"; then die "$1: libc saw '${3#!}'" "$TMP/trace.txt"; fi
                  printf '  [PASS] libc does not see: %-19s (%s)\n' "$1" "${3#!}" ;;
            *) grep -q "^$3" "$TMP/trace.txt" || die "$1: libc did not see '$3'" "$TMP/trace.txt"
               printf '  [PASS] libc sees:         %-19s (%s)\n' "$1" "$3" ;;
        esac
    }
    traced open-etc-passwd succeed "open /etc/passwd"
    traced open-granted-file succeed "open ./stunt-fixture.txt"
    traced write-granted-file succeed "open ./stunt-written.txt"
    traced getenv-home succeed "getenv HOME"
    traced env-cached succeed "!getenv HOME"
    traced connect-192.0.2.1 succeed "connect AF_INET 192.0.2.1"
    traced connect-ipv4-mapped succeed "connect AF_INET6 ::ffff:192.0.2.1"
    traced exec-bin-sh succeed "posix_spawn .*jspawnhelper"
    traced fork succeed "fork"
    traced dlopen-libm succeed "dlopen $GHL_LIBM"
    traced dlopen-libc-by-path succeed "dlopen $GHL_LIBC_PATH"
    traced syscall-fn-openat succeed "syscall openat"
    # The lookup matters: Linker.defaultLookup() finds libc's own getenv, past
    # an interposer; the dlsym(RTLD_DEFAULT) Native does finds the interposer's.
    : > "$TMP/trace.txt"
    (cd "$WORK" && env GHL_TRACE="$TMP/trace.txt" LD_PRELOAD="$BUILD/libghltrace.so" $JAVA_RUN ghl.containment.LookupTrace) > "$TMP/lookup.txt" 2>&1 ||
        die "LookupTrace" "$TMP/lookup.txt"
    [ "$(grep -c '^getenv HOME' "$TMP/trace.txt")" = 1 ] ||
        die "LookupTrace: exactly one of two getenv calls should reach the interposer" "$TMP/trace.txt"
    echo "  [PASS] libc sees: getenv through Native (dlsym RTLD_DEFAULT), not through Linker.defaultLookup()"
fi

# ---- the worker with no wall around it ------------------------------------
# guest.launch_uncontained starts the worker with no preload and no kernel
# layer, so the worker, its protocol (java/guestchild) and its rows run where the
# wall stops a JVM from starting, and on macOS: the host functions, the
# descriptor of a granted file (Linux: the receipt is Linux's msghdr), the shared
# map, and the four rows the host kills or cancels a worker for.
if [ -n "${CHILD_HOST:-}" ]; then
    unc() {   # unc <policy> <row> <expect>
        (cd "$WORK" && env GHL_CHILD_UNCONTAINED=1 "$CHILD_HOST" "$1" "$2" "$3") > "$TMP/unc.txt" 2>&1 ||
            die "uncontained worker: $2 under $1" "$TMP/unc.txt"
        printf '  [PASS] worker, no wall: %s\n' "$(tail -1 "$TMP/unc.txt")"
        rm -f "$WORK/stunt-written.txt"
    }
    unc - callback-greet succeed
    unc - callback-add succeed
    unc - callback-unregistered refused
    unc - binding-read-etc-passwd refused
    unc "env=HOME" binding-getenv-home succeed
    unc - open-etc-passwd succeed
    unc - map-get succeed
    unc - map-set succeed
    unc - map-overwrite-input refused
    unc - spin host
    unc - silent host
    unc - oversize host
    unc - half-frame host
    if [ "$PLAT" = linux ]; then
        unc "fs_read=$WORK/stunt-fixture.txt" binding-read-granted-file succeed
        unc - syscall-openat succeed
        unc - syscall-socket succeed
        unc - fork succeed
    fi
fi

# ---- the programs in README.md's four ways ---------------------------------
(cd "$ROOT" && ae build java/examples/ways.ae --lib . -o "$BUILD/java-ways") > "$TMP/ex.log" 2>&1 ||
    die "examples/ways.ae did not build" "$TMP/ex.log"
(cd "$WORK" && "$BUILD/java-ways" "$JAVA_CLASSES") > "$TMP/ways.txt" 2>&1
grep -q "^java.run: 0" "$TMP/ways.txt" || die "examples/ways.ae: java.run" "$TMP/ways.txt"
grep -q "^hello from Java" "$TMP/ways.txt" || die "examples/ways.ae: the program did not run" "$TMP/ways.txt"
if [ "$PLAT" = macos ]; then
    grep -q "^java.run_sandboxed: -1" "$TMP/ways.txt" || die "examples/ways.ae: java.run_sandboxed on macOS" "$TMP/ways.txt"
elif [ "$BLOCKED_INPROC" = 1 ]; then
    grep -q "^java.run_sandboxed: 1" "$TMP/ways.txt" && grep -q "$BLOCK_MARK" "$TMP/ways.txt" ||
        die "examples/ways.ae: java.run_sandboxed should start a JVM that dies" "$TMP/ways.txt"
else
    grep -q "^java.run_sandboxed: 0" "$TMP/ways.txt" || die "examples/ways.ae: java.run_sandboxed" "$TMP/ways.txt"
fi
echo "  [PASS] examples/ways.ae: ways 1 to 3 as README.md shows them"
if [ -z "${CHILD_HOST:-}" ]; then
    echo "  [SKIP] examples/contained.ae: ae $ae_ver has no std.guest"
    echo "  [PASS] $NAME"
    exit 0
fi
(cd "$ROOT" && ae build java/examples/contained.ae -o "$BUILD/java-contained") > "$TMP/ex.log" 2>&1 ||
    die "examples/contained.ae did not build" "$TMP/ex.log"
(cd "$WORK" && "$BUILD/java-contained" "$JAVA_CLASSES") > "$TMP/contained.txt" 2>&1
if [ "$CHILD" = 1 ]; then
    grep -q '^the host answered' "$TMP/contained.txt" || grep -q '^VALUE' "$TMP/contained.txt" || die "examples/contained.ae" "$TMP/contained.txt"
    grep -q '^DONE 0 ' "$TMP/contained.txt" || die "examples/contained.ae did not end DONE" "$TMP/contained.txt"
elif [ "$CHILD_BLOCKED" = 1 ]; then
    grep -q "^KILLED\|^ERROR" "$TMP/contained.txt" || die "examples/contained.ae: the worker should die at startup" "$TMP/contained.txt"
else
    grep -q "^not started: " "$TMP/contained.txt" || die "examples/contained.ae: macOS should start nothing" "$TMP/contained.txt"
fi
echo "  [PASS] examples/contained.ae: way 4 as README.md shows it"
echo "  [PASS] $NAME"

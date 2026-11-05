# Sourced by the test_stunt_*.sh beside it. Holds a guest language to stunt's
# catalogue by name: aether's tests/integration/contrib_host_stunt lists, one
# script per policy, run against this repository's language instead of the
# stunt engine. Gives the test two calls:
#
#   policy <grants>                 the grants the following checks run under
#   stunt <check> <expectation>     run one row under them, in every mode that
#                                   applies here; stop at the first mismatch
#
# The catalogue is stunt's own (`stunt list`), built from aether's source at
# the ref ci/versions.env pins (or $AETHER_SRC, a checkout), so a name that is
# not stunt's is an error, not a new row.
#
# Modes, per row:
#   inproc  the language's bridge in this process, run by its host
#           (<lang>/containment/host.ae) under libaether_sandbox.so where the
#           preload exists (Linux). Without it (macOS) nothing in the process
#           sees the language's own libc calls: those rows run expecting
#           `succeed` and print NOT HELD, so the table's claim is checked too.
#   child   the language's worker (<lang>/worker) in a contained child,
#           started by std.guest's guest.launch (sandbox.spawn_socket:
#           Landlock and seccomp, and the preload), Linux only.
#
# A row the language cannot express is LEFT OUT by name below, with why.
#
# Paths in a policy may use $WORK, the directory the checks run in, which
# holds stunt-fixture.txt.

LANG_NAME="${GUEST_LANG:-lua}"
NAME="${STUNT_TEST:-stunt}"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/ci/versions.env"

skip_all() { echo "  [SKIP] $NAME: $1"; exit 0; }
die() {
    echo "  [FAIL] $NAME: $1"
    [ -n "${2:-}" ] && [ -f "$2" ] && sed 's/^/    /' "$2" | tail -30
    exit 1
}

case "$(uname -s 2>/dev/null)" in
    Linux) PLAT=linux ;;
    Darwin) PLAT=macos ;;
    FreeBSD) PLAT=freebsd ;;
    *) PLAT=other ;;
esac
command -v ae > /dev/null 2>&1 || die "ae is not on PATH"
. "$SCRIPT_DIR/lua_env.sh"
lua_soname || skip_all "no Lua runtime found (set AETHER_LUA_SONAME)"

BUILD="$ROOT/build"
mkdir -p "$BUILD"
TMP="$(mktemp -d)"
TMP="$(cd "$TMP" && pwd -P)"
trap 'rm -rf "$TMP"' EXIT
WORK="$TMP/work"
mkdir -p "$WORK"
echo "stunt fixture" > "$WORK/stunt-fixture.txt"

# ---- stunt's catalogue ------------------------------------------------------

stunt_src() {
    if [ -n "${AETHER_SRC:-}" ]; then
        echo "$AETHER_SRC/contrib/host/stunt"
        return
    fi
    d="$BUILD/stunt-src-$AETHER_STUNT_REF"
    if [ ! -f "$d/stunt_main.c" ]; then
        mkdir -p "$d.part"
        for f in stunt.c stunt.h stunt_main.c; do
            curl -fsSL "https://raw.githubusercontent.com/aether-lang-dev/aether/$AETHER_STUNT_REF/contrib/host/stunt/$f" \
                -o "$d.part/$f" || die "cannot fetch stunt's $f at $AETHER_STUNT_REF"
        done
        rm -rf "$d" && mv "$d.part" "$d"
    fi
    echo "$d"
}
CATALOGUE="$BUILD/catalogue.txt"
if [ ! -s "$CATALOGUE" ] || [ -n "${AETHER_SRC:-}" ]; then
    src="$(stunt_src)"
    DL=""
    [ "$PLAT" = linux ] && DL="-ldl"
    ${CC:-cc} -std=c11 -O1 -o "$BUILD/stunt" "$src/stunt.c" "$src/stunt_main.c" $DL > "$TMP/cc.log" 2>&1 ||
        die "stunt did not build from $src" "$TMP/cc.log"
    "$BUILD/stunt" list > "$CATALOGUE" || die "stunt list failed" "$CATALOGUE"
fi

modes_of() { awk -v c="$1" 'NR > 1 && $1 == c { print $2 }' "$CATALOGUE"; }

# ---- the language's hosts ---------------------------------------------------

HOST_SRC="$ROOT/$LANG_NAME/containment"
stale() {   # stale <binary>: older than any source it is built from
    [ -x "$1" ] || return 0
    for f in "$HOST_SRC"/*.ae "$HOST_SRC"/*.c "$HOST_SRC"/policy/*.ae "$ROOT/$LANG_NAME"/*.ae \
             "$ROOT/$LANG_NAME"/*.c "$ROOT/$LANG_NAME"/*.h "$ROOT/$LANG_NAME"/worker/*; do
        [ -e "$f" ] && [ "$f" -nt "$1" ] && return 0
    done
    return 1
}
build() {   # build <source .ae> <binary>
    stale "$2" || return 0
    (cd "$ROOT" && ae build "$1" --lib "$LANG_NAME/containment" -o "$2") > "$TMP/build.log" 2>&1 ||
        die "$1 did not build" "$TMP/build.log"
}
HOST="$BUILD/$LANG_NAME-host"
build "$LANG_NAME/containment/host.ae" "$HOST"
export LUA_CHECKS="$HOST_SRC/checks.lua"
case "$PLAT" in
    macos) export GHL_LIBM=/usr/lib/libm.dylib GHL_LIBC_NAME=libSystem.B.dylib GHL_LIBC_PATH=/usr/lib/libSystem.B.dylib ;;
    freebsd) export GHL_LIBM=libm.so.5 GHL_LIBC_NAME=libc.so.7 ;;
    *) export GHL_LIBM=libm.so.6 GHL_LIBC_NAME=libc.so.6 ;;
esac
if [ "$PLAT" = linux ] || [ "$PLAT" = freebsd ]; then
    # The path the loader loaded libc from, as stunt's dlopen-libc-by-path uses.
    GHL_LIBC_PATH="$(ldd "$HOST" 2>/dev/null | awk '/libc\.so/ { print $3; exit }')"
    export GHL_LIBC_PATH
fi

# In-process, the language's own libc calls are held by the preload alone.
# The contained child is started with it too (sandbox.spawn_socket).
PRELOAD=""
if [ "$PLAT" = linux ] || [ "$PLAT" = freebsd ]; then
    libdir="$(ae cflags --libs 2>/dev/null | tr ' ' '\n' | sed -n 's/^-L//p' | head -1)"
    for p in "${AETHER_SANDBOX_PRELOAD:-}" "$libdir/libaether_sandbox.so"; do
        [ -n "$p" ] && [ -f "$p" ] && { PRELOAD="$p"; break; }
    done
    if [ -z "$PRELOAD" ]; then
        [ "${GITHUB_ACTIONS:-}" = true ] && die "libaether_sandbox.so not found beside ae's libaether.a on this runner"
        echo "  [SKIP] $NAME: libaether_sandbox.so not found: the libc rows run as NOT HELD"
    else
        export AETHER_SANDBOX_PRELOAD="$PRELOAD"
    fi
fi

# The contained child: std.guest (ae >= AE_CHILD_PIN) and a kernel that
# spawn_socket can contain a child on (Linux with Landlock). GHL_REQUIRE_CHILD=1
# (the CI job that is there to run it) makes its absence a failure.
CHILD=0
CHILD_WHY=""
ae_ver="$(ae version 2>/dev/null | awk 'NR == 1 { print $2 }')"
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }
if ! ver_ge "$ae_ver" "$AE_CHILD_PIN"; then
    CHILD_WHY="ae $ae_ver has no std.guest (needs $AE_CHILD_PIN)"
else
    CHILD_HOST="$BUILD/$LANG_NAME-child-host"
    WORKER="$BUILD/$LANG_NAME-worker"
    build "$LANG_NAME/containment/child_host.ae" "$CHILD_HOST"
    build "$LANG_NAME/worker/worker.ae" "$WORKER"
    export LUA_WORKER="$WORKER"
    (cd "$WORK" && "$CHILD_HOST" - callback-greet succeed) > "$TMP/probe.txt" 2>&1
    if grep -q "callback-greet: succeeded" "$TMP/probe.txt"; then
        CHILD=1
        [ "$PLAT" = linux ] || die "a child started on $PLAT, where spawn_socket should refuse" "$TMP/probe.txt"
    elif [ "$PLAT" = macos ] && grep -q "cannot contain a child on macOS, so nothing was started" "$TMP/probe.txt"; then
        # Fails closed, as it should: that is checked, not skipped.
        CHILD_WHY="guest.launch refuses on macOS and starts nothing (checked: $(tail -1 "$TMP/probe.txt" | cut -c1-90)...)"
    else
        CHILD_WHY="the contained child did not start: $(tail -1 "$TMP/probe.txt")"
    fi
fi
if [ "$CHILD" = 0 ]; then
    [ "${GHL_REQUIRE_CHILD:-}" = 1 ] && die "child mode required: $CHILD_WHY"
    echo "  [SKIP] $NAME: child mode: $CHILD_WHY"
fi

# ---- what applies -----------------------------------------------------------

# Rows Lua cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        env-cached) echo "Lua keeps no copy of the environment: os.getenv is libc's getenv (getenv-home)" ;;
        connect-*) echo "Lua's standard library has no sockets" ;;
        fork) echo "Lua's standard library has no fork" ;;
        syscall-*) echo "Lua's standard library has no syscall()" ;;
        half-frame) echo "a Lua script cannot write part of a frame: the worker frames what it says" ;;
        *) return 1 ;;
    esac
}

# The rows that go to libc from the language's own runtime: in-process,
# only the preload holds them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for this language and this
# ae: run expecting `succeed`, so the day the preload holds them this goes red
# and the table in <lang>/README.md is brought up to date. exec-bin-sh is held
# from AE_SPAWN_PIN on (aether #3580).
escapes_preload() {
    case "$1" in
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: os.execute is system(), and glibc starts /bin/sh with posix_spawn, a clone and an execve inside libc that this preload's execve and fork do not see" ;;
        *) return 1 ;;
    esac
}

# Rows the contained child refuses where the policy would allow them: the
# std.guest worker locks itself down with no grants and is given no
# environment, so a granted file reaches the script only as a descriptor
# (binding-read-granted-file), a variable only as a host function's answer
# (binding-getenv-home), and libc never as a handle. Run expecting `refused`.
child_stricter() {
    case "$1" in
        open-granted-file|write-granted-file) echo "the worker's own lockdown: a granted file comes as a descriptor, through read_file" ;;
        getenv-home) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" || return 1
            echo "the worker's own lockdown has no exec or fork grant, and from ae $AE_SPAWN_PIN the preload holds system() to them" ;;
        *) return 1 ;;
    esac
}

POLICY="-"
policy() { POLICY="$1"; }

RAN=0
NOT_HELD=0
LEFT=0
STRICTER=0
run_one() {   # run_one <label> <mode> <expectation-to-run> <check>
    out="$TMP/out.txt"
    case "$2" in
        inproc)
            if [ -n "$PRELOAD" ]; then
                (cd "$WORK" && LD_PRELOAD="$PRELOAD" "$HOST" inproc "$POLICY" "$4" "$3") > "$out" 2>&1
            else
                (cd "$WORK" && "$HOST" inproc "$POLICY" "$4" "$3") > "$out" 2>&1
            fi ;;
        child) (cd "$WORK" && "$CHILD_HOST" "$POLICY" "$4" "$3") > "$out" 2>&1 ;;
    esac
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  [FAIL] $NAME ($2, policy $POLICY): exit $rc"
        sed 's/^/    /' "$out"
        exit 1
    fi
    printf '  [%s] %-6s %s\n' "$1" "$2" "$(tail -1 "$out")"
    rm -f "$WORK/stunt-written.txt"
}

stunt() {
    check="$1"
    expect="$2"
    modes="$(modes_of "$check")"
    [ -n "$modes" ] || die "'$check' is not in stunt's catalogue ($AETHER_STUNT_REF)"
    if why="$(left_out "$check")"; then
        printf '  [LEFT OUT] %s: %s\n' "$check" "$why"
        LEFT=$((LEFT + 1))
        return
    fi
    case "$modes" in *in-process*) stunt_inproc "$check" "$expect" ;; esac
    # "<outer>|<inner>" nests the bridge's level in an enforce block: an
    # in-process shape.
    case "$POLICY" in *"|"*) return ;; esac
    if [ "$CHILD" = 1 ]; then
        case "$modes" in *child*) stunt_child "$check" "$expect" ;; esac
    fi
}

stunt_inproc() {
    if [ "$2" = refused ] && libc_row "$1"; then
        if [ -z "$PRELOAD" ]; then
            run_one "NOT HELD" inproc succeed "$1"
            NOT_HELD=$((NOT_HELD + 1))
            return
        fi
        if why="$(escapes_preload "$1")"; then
            run_one "ESCAPES" inproc succeed "$1"
            printf '             (%s)\n' "$why"
            NOT_HELD=$((NOT_HELD + 1))
            return
        fi
    fi
    run_one PASS inproc "$2" "$1"
    RAN=$((RAN + 1))
}

stunt_child() {
    if [ "$2" = succeed ] && why="$(child_stricter "$1")"; then
        run_one "STRICTER" child refused "$1"
        printf '             (%s)\n' "$why"
        STRICTER=$((STRICTER + 1))
        return
    fi
    run_one PASS child "$2" "$1"
    RAN=$((RAN + 1))
}

done_checks() {
    echo "  [PASS] $NAME: $RAN as expected, $NOT_HELD not held in-process, $STRICTER stricter in the child, $LEFT left out"
}

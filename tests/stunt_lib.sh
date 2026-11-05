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
# What differs between languages is in <lang>/containment/lang.sh: how to
# find the runtime, where the checks and the worker are, and the rows that
# differ from stunt's expectation, by name and with why: left out (the
# language cannot express it, in both modes or in-process only), escapes or
# not held in-process, stricter in the child, or escaping the child's wall
# (child_escapes). A language whose checks and worker are programs of their
# own (Go) builds them in a lang_build hook.
#
# Every outcome is also appended to build/parity-<lang>.tsv, from which
# tests/parity.sh renders and checks the README's "Stunt parity" table.
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
# A language's lang.sh may name rows where the preload itself fails (an
# error where the policy says succeed or refused); by default there are none.
preload_fails() { return 1; }
# And rows a contained child lets through where stunt expects a refusal: by
# default none (a language whose worker locks itself down, as std.guest.child
# does, is held by that; one that cannot, like Go, has only the spawn's wall).
child_escapes() { return 1; }
# And rows the language cannot express in-process only (the contained child
# runs them): left_out is for both.
left_out_inproc() { return 1; }
. "$ROOT/$LANG_NAME/containment/lang.sh"
lang_runtime || skip_all "$LANG_NO_RUNTIME"

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
eval "export $LANG_CHECKS_ENV=\"\$HOST_SRC/\$LANG_CHECKS_FILE\""
# A language whose checks and worker are programs of its own (Go) builds them
# here: lang_build exports the checks' variable and sets LANG_WORKER_BUILT.
LANG_WORKER_BUILT=""
type lang_build > /dev/null 2>&1 && lang_build
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
    if [ -n "$LANG_WORKER_BUILT" ]; then WORKER="$LANG_WORKER_BUILT"; else build "$LANG_NAME/worker/worker.ae" "$WORKER"; fi
    eval "export $LANG_WORKER_ENV=\"\$WORKER\""
    # The row that shows a child can run at all: a host function's answer by
    # default (callback-greet); a language with no host functions names
    # another that succeeds with no grants (lang.sh's LANG_PROBE).
    probe="${LANG_PROBE:-callback-greet}"
    (cd "$WORK" && "$CHILD_HOST" - "$probe" succeed) > "$TMP/probe.txt" 2>&1
    if grep -q "$probe: succeeded" "$TMP/probe.txt"; then
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

# left_out, libc_row, escapes_preload and child_stricter are the language's
# (lang.sh, sourced above).

POLICY="-"
policy() { POLICY="$1"; }

# ---- what the README's Stunt parity table is made from ----------------------

# One line per outcome: test, column, ae side of AE_SPAWN_PIN, row, kind.
# The columns are the ways of lua/README.md's "Four ways": 2 the sandbox
# block with no preload, 3 the same under LD_PRELOAD, 4 the contained child.
PARITY="$BUILD/parity-$LANG_NAME.tsv"
if ver_ge "$ae_ver" "$AE_SPAWN_PIN"; then SIDE=from; else SIDE=before; fi
# FreeBSD's preload is not Linux's: its runs are recorded apart (3f), and
# the table, whose column 3 is Linux's, does not take them.
inproc_column() {
    if [ -z "$PRELOAD" ]; then echo 2
    elif [ "$PLAT" = freebsd ]; then echo 3f
    else echo 3; fi
}
record() {   # record <mode> <check> <kind>
    case "$1" in inproc) col="$(inproc_column)" ;; *) col=4 ;; esac
    printf '%s\t%s\t%s\t%s\t%s\n' "${STUNT_TEST:-stunt}" "$col" "$SIDE" "$2" "$3" >> "$PARITY"
}

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
    record "$2" "$4" "$1"
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
        # Left out in every mode stunt runs it in that runs here.
        case "$modes" in *in-process*) record inproc "$check" "LEFT OUT" ;; esac
        case "$POLICY" in *"|"*) return ;; esac
        if [ "$CHILD" = 1 ]; then
            case "$modes" in *child*) record child "$check" "LEFT OUT" ;; esac
        fi
        return
    fi
    case "$modes" in
        *in-process*)
            if why="$(left_out_inproc "$check")"; then
                printf '  [LEFT OUT] %s (in-process): %s\n' "$check" "$why"
                LEFT=$((LEFT + 1))
                record inproc "$check" "LEFT OUT"
            else
                stunt_inproc "$check" "$expect"
            fi ;;
    esac
    # "<outer>|<inner>" nests the bridge's level in an enforce block: an
    # in-process shape.
    case "$POLICY" in *"|"*) return ;; esac
    if [ "$CHILD" = 1 ]; then
        case "$modes" in *child*) stunt_child "$check" "$expect" ;; esac
    fi
}

stunt_inproc() {
    if [ -n "$PRELOAD" ] && why="$(preload_fails "$1")"; then
        run_one "PRELOAD FAILS" inproc error "$1"
        printf '             (%s)\n' "$why"
        NOT_HELD=$((NOT_HELD + 1))
        return
    fi
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
    if [ "$2" = refused ] && why="$(child_escapes "$1")"; then
        run_one "ESCAPES" child succeed "$1"
        printf '             (%s)\n' "$why"
        NOT_HELD=$((NOT_HELD + 1))
        return
    fi
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
    echo "  [PASS] $NAME: $RAN as expected, $NOT_HELD not held (in-process, or escaping the child), $STRICTER stricter in the child, $LEFT left out"
}

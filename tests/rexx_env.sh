# Sourced: finds RexxAE, the Rexx engine written in Aether that rexx/ runs,
# and builds from it what the tests need. RexxAE is another repository
# (github.com/aether-lang-dev/RexxAE), not a library on this machine: there
# is no package to install and nothing to dlopen. So this takes its source at
# the commit ci/versions.env pins (REXXAE_REF; or REXXAE_SRC, a checkout of
# your own) and builds, with the ae on PATH, into build/rexxae-lib/:
#
#   librexxae.so (.dylib on macOS)  RexxAE's Aether binary library, the
#                                   shared-runtime variant (`make aether-lib`
#                                   in RexxAE): what `import rexxae`, and so
#                                   `import rexx`, links against
#   rexxae-worker                   RexxAE's own worker (worker/worker.ae),
#                                   the contained child: the engine compiled
#                                   in from source, on std.guest.child
#
# and exports REXXAE_LIB_DIR (that directory, the --lib to build against),
# REXXAE_SRC_DIR (the source it was built from) and REXXAE_COMMIT. A build is
# kept until the source commit or the ae changes (build/rexxae-lib/stamp).
REXXAE_ENV_ROOT="$(cd "$(dirname "${SCRIPT_DIR:-.}/x")/.." 2>/dev/null && pwd)"
[ -n "${REXXAE_REF:-}" ] || . "$REXXAE_ENV_ROOT/ci/versions.env"

rexxae_fetch() {   # the source at REXXAE_REF, fetched once into build/
    d="$REXXAE_ENV_ROOT/build/rexxae-src-$REXXAE_REF"
    if [ ! -f "$d/src/engine.ae" ]; then
        command -v git > /dev/null 2>&1 || { echo "  rexx: git is needed to fetch RexxAE at $REXXAE_REF" >&2; return 1; }
        rm -rf "$d.part"
        mkdir -p "$d.part"
        (cd "$d.part" && git init -q && git fetch -q --depth 1 "$REXXAE_REPO" "$REXXAE_REF" &&
            git checkout -q FETCH_HEAD) > "$REXXAE_ENV_ROOT/build/rexxae-fetch.log" 2>&1 ||
            { echo "  rexx: cannot fetch RexxAE $REXXAE_REF from $REXXAE_REPO" >&2; cat "$REXXAE_ENV_ROOT/build/rexxae-fetch.log" >&2; return 1; }
        rm -rf "$d" && mv "$d.part" "$d"
    fi
    echo "$d"
}

rexxae_engine() {
    if [ -n "${REXXAE_LIB_DIR:-}" ] && [ -f "$REXXAE_LIB_DIR/stamp" ]; then return 0; fi
    command -v ae > /dev/null 2>&1 || return 1
    mkdir -p "$REXXAE_ENV_ROOT/build"
    if [ -n "${REXXAE_SRC:-}" ]; then
        src="$(cd "$REXXAE_SRC" && pwd)" || return 1
    else
        src="$(rexxae_fetch)" || return 1
    fi
    [ -f "$src/src/engine.ae" ] || { echo "  rexx: $src is not a RexxAE checkout" >&2; return 1; }
    commit="$(git -C "$src" rev-parse HEAD 2>/dev/null || echo "$REXXAE_REF")"
    case "$(uname -s)" in Darwin) ext=dylib ;; *) ext=so ;; esac
    out="$REXXAE_ENV_ROOT/build/rexxae-lib"
    want="$commit $(ae version 2>/dev/null | awk 'NR == 1 { print $2 }') $src"
    if [ ! -f "$out/stamp" ] || [ "$(cat "$out/stamp")" != "$want" ] || [ ! -f "$out/librexxae.$ext" ] || [ ! -x "$out/rexxae-worker" ]; then
        rm -rf "$out"
        mkdir -p "$out"
        log="$REXXAE_ENV_ROOT/build/rexxae-build.log"
        # Built from RexxAE's root, as its Makefile does: the worker's
        # `import src.engine` resolves there.
        if ! (cd "$src" && ae build --emit=lib --shared-runtime src/engine.ae -o "$out/librexxae.$ext" &&
              ae build worker/worker.ae -o "$out/rexxae-worker") > "$log" 2>&1; then
            echo "  rexx: RexxAE did not build (from $src):" >&2
            tail -30 "$log" >&2
            rm -rf "$out"
            return 1
        fi
        echo "$want" > "$out/stamp"
        # The hosts link librexxae: a new one makes them stale.
        rm -f "$REXXAE_ENV_ROOT/build/rexx-host" "$REXXAE_ENV_ROOT/build/rexx-child-host" \
              "$REXXAE_ENV_ROOT/build/rexx-surface" "$REXXAE_ENV_ROOT/build/test_host_rexx"
    fi
    export REXXAE_LIB_DIR="$out" REXXAE_SRC_DIR="$src" REXXAE_COMMIT="$commit"
}

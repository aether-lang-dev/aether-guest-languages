# Sourced: finds the Go toolchain and builds the c-shared libraries the
# tinygo bridge's tests load, into build/, exporting where each is:
#
#   TINYGO_LIB          examples/greet.go, for test_host_tinygo.ae
#   TINYGO_PROBE_LIB    examples/probe.go, for the README's four ways
#   TINYGO_FAKE_LIB     test_support/fake_cshared.c, a C stand-in for a
#                       Go library (needs only the C compiler)
#   TINYGO_CHECKS_LIB   containment/checks.go, the stunt rows in Go
#
# The toolchain is `go`: $GO, then PATH, then the usual install places.
# TinyGo's c-shared is wasm-only (tinygo/README.md, "Toolchain note"), so
# the libraries are built by standard go whether or not tinygo is installed;
# TINYGO_FOR_TESTS names a tinygo if one is found, for the parity table's
# version line only. c-shared needs cgo, so a C compiler (cc, or $CC) is
# needed too. Without go, tinygo_runtime fails and the tests skip.
tinygo_go() {
    g="${GO:-}"
    if [ -z "$g" ]; then
        g="$(command -v go 2>/dev/null)"
    fi
    if [ -z "$g" ]; then
        for p in /opt/homebrew/bin/go /usr/local/go/bin/go /usr/lib/go/bin/go "$HOME/go/bin/go"; do
            [ -x "$p" ] && { g="$p"; break; }
        done
    fi
    [ -n "$g" ] && [ -x "$g" ] || return 1
    GO_FOR_TESTS="$g"
    TINYGO_FOR_TESTS="$(command -v tinygo 2>/dev/null || true)"
    return 0
}

tinygo_ext() {
    case "$(uname -s 2>/dev/null)" in Darwin) echo dylib ;; *) echo so ;; esac
}

# Builds <out> from <source> unless it is newer than the source.
tinygo_gobuild() {   # tinygo_gobuild <source dir> <out>
    [ -f "$2" ] && [ -z "$(find "$1" -name '*.go' -newer "$2" 2>/dev/null)" ] && return 0
    (cd "$1" && CGO_ENABLED=1 "$GO_FOR_TESTS" build -buildmode=c-shared -o "$2" .) \
        > "$ROOT/build/tinygo-go-build.log" 2>&1 || { cat "$ROOT/build/tinygo-go-build.log"; return 1; }
    rm -f "${2%.*}.h"
}

tinygo_runtime() {
    tinygo_go || return 1
    command -v "${CC:-cc}" > /dev/null 2>&1 || return 1
    mkdir -p "$ROOT/build"
    ext="$(tinygo_ext)"
    # Each example is one file; `go build .` wants a directory of its own.
    for ex in greet probe; do
        if [ ! -f "$ROOT/build/$ex-src/$ex.go" ] || [ "$ROOT/tinygo/examples/$ex.go" -nt "$ROOT/build/$ex-src/$ex.go" ]; then
            mkdir -p "$ROOT/build/$ex-src"
            cp "$ROOT/tinygo/examples/$ex.go" "$ROOT/build/$ex-src/$ex.go"
            [ -f "$ROOT/build/$ex-src/go.mod" ] || printf 'module %s\n\ngo 1.21\n' "$ex" > "$ROOT/build/$ex-src/go.mod"
        fi
        tinygo_gobuild "$ROOT/build/$ex-src" "$ROOT/build/lib$ex.$ext" || return 1
    done
    tinygo_gobuild "$ROOT/tinygo/containment" "$ROOT/build/libchecks.$ext" || return 1
    fake="$ROOT/build/libfake.$ext"
    if [ ! -f "$fake" ] || [ "$ROOT/tinygo/test_support/fake_cshared.c" -nt "$fake" ]; then
        "${CC:-cc}" -shared -fPIC -o "$fake" "$ROOT/tinygo/test_support/fake_cshared.c" || return 1
    fi
    export TINYGO_LIB="$ROOT/build/libgreet.$ext"
    export TINYGO_PROBE_LIB="$ROOT/build/libprobe.$ext"
    export TINYGO_FAKE_LIB="$fake"
    export TINYGO_CHECKS_LIB="$ROOT/build/libchecks.$ext"
    export GO_FOR_TESTS
    return 0
}

tinygo_version() {
    "$GO_FOR_TESTS" version 2>/dev/null | awk '{ sub(/^go/, "", $3); print $3 }'
}

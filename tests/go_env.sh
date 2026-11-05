# Sourced: finds the Go toolchain the checks and the worker are built with
# and exports it as GO (and GO_BIN, which the bridge's go.run_script uses),
# unless GO is set already. Go is not a library the bridge loads: the bridge
# starts a program, so what the tests need is the compiler, and whether cgo
# works, since the rows that go through libc (getenv, dlopen, fork, libc's
# syscall(), shm_open) are cgo's.
#
# GO         the go command (default: go on PATH, then Homebrew's and the
#            official tarball's)
# GO_CGO     1 when `go build` can use cgo here (CGO_ENABLED=1 and a C
#            compiler), else 0: the cgo rows are then left out
go_tool() {
    if [ -z "${GO:-}" ]; then
        GO="$(command -v go 2>/dev/null)"
        for f in /opt/homebrew/bin/go /usr/local/go/bin/go /usr/lib/go/bin/go; do
            [ -n "$GO" ] && break
            [ -x "$f" ] && GO="$f"
        done
    fi
    [ -n "$GO" ] && [ -x "$GO" ] || return 1
    export GO GO_BIN="$GO"
    return 0
}

go_cgo() {
    go_tool || return 1
    cc="${CC:-$("$GO" env CC 2>/dev/null)}"
    if [ "$("$GO" env CGO_ENABLED 2>/dev/null)" = 1 ] && command -v "${cc%% *}" > /dev/null 2>&1; then
        GO_CGO=1
    else
        GO_CGO=0
    fi
    export GO_CGO
}

# The platform has no libaether_sandbox.so (macOS), so go.run_sandboxed starts
# nothing: the containment hosts then use go.run, which the rows' "not held"
# result is about.
go_uncontained() {
    if [ "$(uname -s 2>/dev/null)" = Darwin ]; then
        export GHL_GO_UNCONTAINED=1
    fi
}

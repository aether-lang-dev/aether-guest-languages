# Go's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/). Go is not embedded: the
# bridge starts a program, so the "runtime" is the go toolchain that builds
# the checks and the worker, and what differs from stunt's expectation is
# mostly that Go reaches the system without libc.

. "$SCRIPT_DIR/go_env.sh"

LANG_TITLE=Go
# The bridge's spec, and the containment tests with
# go/containment/test_ways.sh (LANG_EXTRA_TESTS).
LANG_SPECS="go/test_host_go.ae"
LANG_RUNTIME_ENV=GO
LANG_NO_RUNTIME="no go toolchain found (set GO)"
lang_runtime() { go_cgo && go_uncontained; }
# The README's tables are for a cgo build; without cgo the libc rows are left
# out and the parity check would only say so.
lang_parity_ok() { go_cgo && [ "$GO_CGO" = 1 ]; }
lang_version() { "$GO" env GOVERSION 2>/dev/null | sed 's/^go//'; }
LANG_CHECKS_ENV=GO_CHECKS
LANG_CHECKS_FILE=checks.go
LANG_WORKER_ENV=GO_WORKER
LANG_EXTRA_TESTS="$ROOT/go/containment/test_ways.sh"

# Builds the checks (ways 1 to 3) and the worker (way 4), once per run, with
# cgo (the catalogue's libc rows are cgo's). The build hook of stunt_lib.sh.
lang_build() {
    stale_go() {   # stale_go <binary>: older than a Go source or go.mod
        [ -x "$1" ] || return 0
        for f in "$ROOT"/go/go.mod "$ROOT"/go/*/*.go; do
            [ -e "$f" ] && [ "$f" -nt "$1" ] && return 0
        done
        return 1
    }
    mkdir -p "$BUILD"
    if stale_go "$BUILD/go-checks"; then
        (cd "$ROOT/go" && "$GO" build -o "$BUILD/go-checks" ./containment) > "$TMP/gobuild.log" 2>&1 ||
            die "go build ./containment failed" "$TMP/gobuild.log"
    fi
    if stale_go "$BUILD/go-worker"; then
        (cd "$ROOT/go" && "$GO" build -o "$BUILD/go-worker" ./worker) > "$TMP/gobuild.log" 2>&1 ||
            die "go build ./worker failed" "$TMP/gobuild.log"
    fi
    export GO_CHECKS="$BUILD/go-checks"
    LANG_WORKER_BUILT="$BUILD/go-worker"
}

# Rows Go cannot express, by name. Without cgo (CGO_ENABLED=0, or no C
# compiler) there is no libc to call: getenv, dlopen, fork, libc's syscall()
# and shm_open are cgo's. The raw system call rows are Linux's and only run
# in the contained child, which is Linux only.
left_out() {
    [ "$GO_CGO" = 1 ] && return 1
    case "$1" in
        getenv-home|fork|dlopen-*|syscall-fn-openat|map-*)
            echo "built without cgo: Go has no libc to call, and $1 is a libc call" ;;
        *) return 1 ;;
    esac
}

# Rows a plain child cannot run, the contained child can: a child process has
# no host functions (the bridge gives it no way to call back: its channels
# are its exit status, its output and the shared map), and the host cannot
# stop a child that loops (the bridge waits for it).
left_out_inproc() {
    case "$1" in
        binding-*|callback-*) echo "a child process has no host functions: the bridge gives it an exit status, its output and the shared map, and no way to call the host" ;;
        spin) echo "go.run_sandboxed waits for the child to exit: there is no instruction limit or cancel to stop a loop (std.guest's deadline is the contained child's)" ;;
        map-*)
            [ "$PLAT" = macos ] || return 1
            echo "macOS has no libaether_sandbox.so, so the _with_map run, the only one that hands the child the map, does not start" ;;
        *) return 1 ;;
    esac
}

# The rows that reach the system from Go's own runtime rather than through
# the host: in a plain child, only the preload could hold them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|env-cached|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Go: run expecting
# `succeed`, so the day they are held this goes red and the table in
# go/README.md is brought up to date. Go's os, net and os/exec make the
# system calls themselves, so the preload never sees them; what goes through
# cgo's libc calls (getenv, dlopen) it does.
escapes_preload() {
    case "$1" in
        open-*|write-*|connect-*) echo "os, net and syscall make the openat, write and connect system calls themselves, not through libc: nothing the preload interposes is called" ;;
        env-cached) echo "os.Getenv is the copy of the environment the Go runtime took when it started; no libc getenv is called" ;;
        exec-bin-sh) echo "os/exec's fork and execve are system calls the Go runtime makes itself, not libc's fork, execve or system(): the preload does not see them whatever ae it is" ;;
        *) return 1 ;;
    esac
}

# Rows where the preload itself fails: none for Go.
preload_fails() { return 1; }

# Rows the contained child refuses where the policy would allow them: the
# std.guest worker is started with no environment, so a variable comes only
# through the host's getenv. Nothing else: python-worker locks itself down
# with no grants once its script starts (std.guest.child.run), so a file, a
# socket or a shell the policy grants still does not reach its script. Go
# cannot enter an Aether sandbox block, so a grant on the spawn is the
# worker's own authority: Landlock lets it open a granted file itself,
# seccomp lets it make a socket, and the preload (which it is dynamic enough
# to load, with cgo) lets libc's fork and dlopen through. Run expecting
# `refused`.
child_stricter() {
    case "$1" in
        getenv-home|env-cached) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        exec-bin-sh)
            [ "$GO_CGO" = 0 ] || return 1
            echo "a static worker (built without cgo) cannot execve even a granted /bin/sh: EACCES, where the cgo worker can (not investigated)" ;;
        *) return 1 ;;
    esac
}

# Rows the contained child lets through where stunt expects a refusal. Any
# tcp grant opens seccomp's socket(AF_INET) for the process, and the address
# check is the preload's, on libc's connect: Go makes the connect system call
# itself. So a worker granted one address can reach every address.
child_escapes() {
    case "$1:$POLICY" in
        connect-*:*tcp=*) echo "seccomp lets socket(AF_INET) through for any tcp grant and Landlock filters ports, not addresses; the address is the preload's check on libc's connect, which Go's connect system call does not call" ;;
        *) return 1 ;;
    esac
}

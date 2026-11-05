# Go's side of the test harness (the tinygo bridge), sourced by tests/run.sh
# and tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find the Go
# toolchain and build the c-shared libraries, which specs to run, where the
# checks and the worker are, and how Go's rows of stunt's catalogue differ
# from stunt's own expectation, each with why. python/containment/lang.sh is
# the same for Python.

. "$SCRIPT_DIR/tinygo_env.sh"

LANG_TITLE=Go
LANG_SPECS="tinygo/test_host_tinygo.ae tinygo/test_host_tinygo_owned.ae"
LANG_RUNTIME_ENV=TINYGO_LIB
LANG_NO_RUNTIME="no Go toolchain found (set GO), or no C compiler for cgo"
lang_runtime() { tinygo_runtime; }
# What the parity table says the column ran against.
lang_version() { tinygo_version; }
# The checks are a Go package built into a library (tests/tinygo_env.sh); this
# is only the source, which stunt_lib.sh exports by the name below and nothing
# reads.
LANG_CHECKS_ENV=TINYGO_CHECKS_SRC
LANG_CHECKS_FILE=checks.go
LANG_WORKER_ENV=TINYGO_WORKER
# Go's own, beside the stunt ones.
LANG_EXTRA_TESTS="$ROOT/tinygo/containment/test_threads.sh $ROOT/tinygo/containment/test_examples.sh"
# The bridge has no host functions, so the row that shows a contained child
# can run at all is not callback-greet: dlopen of libm, which loads with no
# grants (the control of test_stunt_no_grants.sh).
LANG_PROBE=dlopen-libm

# Rows Go cannot express because the bridge has nothing to carry them, by
# name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        binding-*) echo "the bridge has no host functions: a Go library cannot ask the host's read_file or getenv, so there is no checked binding to hold" ;;
        callback-*) echo "the bridge has no host functions: nothing to register, call or find unregistered" ;;
        map-*) echo "the bridge has no shared map: a Go library has no aether_map_get or aether_map_put" ;;
        *) return 1 ;;
    esac
}

# Rows Go cannot express in-process only: the contained child runs them.
left_out_inproc() {
    case "$1" in
        spin) echo "the bridge has no instruction limit and a Go call cannot be interrupted from outside: it runs to its end on the caller's thread; the contained child cancels at a deadline instead" ;;
        *) return 1 ;;
    esac
}

# The rows that reach the system from Go's own runtime rather than through
# the host: in-process, only the preload could hold them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|env-cached|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Go: run expecting
# `succeed`, so the day they are held this goes red and the table in
# tinygo/README.md is brought up to date. On Linux Go makes its system calls
# with the syscall instruction and never calls libc, which is all the preload
# interposes. Only what is done through cgo (getenv-home, dlopen-*) is held.
escapes_preload() {
    case "$1" in
        open-*|write-*|connect-*|exec-bin-sh) echo "Go's os, net and os/exec make these system calls with the syscall instruction, not through libc: the preload never sees them" ;;
        env-cached) echo "os.Getenv and os.Environ read the copy of the environment Go took when the library loaded, before any sandbox: no libc call, nothing for an interposer to see" ;;
        *) return 1 ;;
    esac
}

# Rows the contained child refuses where the policy would allow them: the
# std.guest worker is given no environment, so libc's getenv and Go's own
# copy of the environment have no HOME. Everything else follows the policy:
# the kernel layer the policy builds holds Go's raw system calls, and the
# worker's own lockdown (a user-level layer) sees only the libc calls.
child_stricter() {
    case "$1" in
        getenv-home) echo "guest.launch gives the worker no environment: libc's getenv has no HOME" ;;
        env-cached) echo "guest.launch gives the worker no environment: Go's copy of it has no HOME" ;;
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy (cgo's dlopen is libc's, on a thread that took the lockdown's level)" ;;
        *) return 1 ;;
    esac
}

# Rows the contained child lets through where the policy refuses them. A tcp
# grant is checked by address only in the preload, at libc's connect(); the
# kernel layer (seccomp) lets an AF_INET socket be made with any tcp grant,
# and Go calls connect(2) itself. So a child granted one address can connect
# anywhere.
child_escapes() {
    case "$POLICY:$1" in
        *tcp=*:connect-*) echo "Go's connect(2) is a system call, not libc's connect(): the preload's address check never runs, and the kernel layer only gates socket(AF_INET) on there being some tcp grant" ;;
        *) return 1 ;;
    esac
}

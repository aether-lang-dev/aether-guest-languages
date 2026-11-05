# Tcl's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find libtcl,
# which specs to run, where the checks and the worker are, and how Tcl's rows
# of stunt's catalogue differ from stunt's own expectation, each with why.
# lua/containment/lang.sh and python/containment/lang.sh are the same for
# Lua and Python.

. "$SCRIPT_DIR/tcl_env.sh"

LANG_TITLE=Tcl
LANG_SPECS="tcl/test_host_tcl.ae tcl/test_host_tcl_vm.ae"
LANG_RUNTIME_ENV=AETHER_TCL_SONAME
LANG_NO_RUNTIME="no libtcl found (set AETHER_TCL_SONAME)"
lang_runtime() { tcl_soname && { tcl_library || true; }; }
# What the parity table says the column ran against.
lang_version() { tcl_version; }
LANG_CHECKS_ENV=TCL_CHECKS
LANG_CHECKS_FILE=checks.tcl
LANG_WORKER_ENV=TCL_WORKER
# Tcl's own tests beside the stunt ones: none.
LANG_EXTRA_TESTS=""

# Rows Tcl cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        getenv-home) echo "Tcl never calls getenv: \$env is a copy it took when the interpreter was made, which env-cached is about" ;;
        connect-ipv4-mapped)
            [ "$PLAT" = macos ] || return 1
            echo "macOS has no preload to hold it, and the system's Tcl 8.5 resolves no IPv6 literal (socket ::ffff:192.0.2.1 443 fails in getaddrinfo, granted or not)" ;;
        dlopen-libc-*) echo "Tcl 8.6's load formats the NULL dlerror() of a refused dlopen and dies (SIGSEGV in TclpDlopen): the worker is killed, the script never sees a refusal" ;;
        fork) echo "Tcl has no fork (exec forks and execs in one step: exec-bin-sh)" ;;
        syscall-*) echo "Tcl has no syscall() and no way to run machine code without a compiled extension" ;;
        half-frame) echo "a Tcl script cannot write to a descriptor it is not given a path for: the worker's socket is not one it can open" ;;
        *) return 1 ;;
    esac
}

# The rows that reach the system from Tcl's own runtime rather than through
# the host: in-process, only the preload could hold them.
libc_row() {
    case "$1" in
        open-*|write-*|env-cached|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Tcl and this ae: run
# expecting `succeed`, so the day it holds them this goes red and the table in
# tcl/README.md is brought up to date.
escapes_preload() {
    case "$1" in
        env-cached) echo "\$env is the copy of the environment Tcl took in Tcl_Init, before any sandbox: reading it is an array read, which no libc interposer sees" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: Tcl's exec is vfork and execvp, and glibc's execvp reaches execve inside libc, past the preload's execve wrapper" ;;
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
        env-cached) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        connect-*) echo "the worker's own lockdown has no tcp grant: the preload refuses the connect whatever the policy" ;;
        exec-bin-sh) echo "the worker's own lockdown can create no file, and Tcl's exec makes a temporary file for the command's stderr before it forks" ;;
        *) return 1 ;;
    esac
}

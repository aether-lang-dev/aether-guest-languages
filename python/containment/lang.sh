# Python's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find libpython,
# which specs to run, where the checks and the worker are, and how Python's
# rows of stunt's catalogue differ from stunt's own expectation, each with
# why. lua/containment/lang.sh is the same for Lua.

. "$SCRIPT_DIR/python_env.sh"

LANG_TITLE=Python
LANG_SPECS="python/test_host_python.ae python/test_host_python_vm.ae"
LANG_RUNTIME_ENV=AETHER_PYTHON_SONAME
LANG_NO_RUNTIME="no libpython found (set AETHER_PYTHON_SONAME)"
lang_runtime() { python_soname && python_stdlib; }
# What the parity table says the column ran against.
lang_version() {
    "${PYTHON_FOR_TESTS:-python3}" -c 'import platform; print(platform.python_version())' 2>/dev/null
}
LANG_CHECKS_ENV=PYTHON_CHECKS
LANG_CHECKS_FILE=checks.py
LANG_WORKER_ENV=PYTHON_WORKER
# Python's own tests, beside the stunt ones.
LANG_EXTRA_TESTS="$ROOT/python/containment/test_imports.sh"

# Every row of the catalogue can be said in Python (checks.py), so none is
# left out: sockets, fork and ctypes (libc's syscall(), and the syscall
# instruction from a few bytes of machine code) are in the standard
# library, os.environ is the cached copy env-cached is about, and a script
# in the contained child can write part of a frame to the socket its worker
# talks to the host on.
left_out() { return 1; }

# The rows that reach the system from Python's own runtime rather than
# through the host: in-process, only the preload could hold them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|env-cached|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Python and this ae:
# run expecting `succeed`, so the day they are held this goes red and the
# table in python/README.md is brought up to date.
escapes_preload() {
    case "$1" in
        env-cached) echo "os.environ is the copy of the environment CPython took when it started, before any sandbox: reading it is a dict lookup, which no libc interposer sees" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: os.system is system(), and glibc starts /bin/sh with posix_spawn, a clone and an execve inside libc that this preload's execve and fork do not see; subprocess, which uses posix_spawn too on CPython 3.13 and later, gets through the same way" ;;
        *) return 1 ;;
    esac
}

# Rows where the preload itself fails, run expecting an error. FreeBSD's
# preload answers fork() and system() with ENOSYS even when fork and exec
# are granted (posix_spawn and vfork get through), so os.system and
# subprocess cannot start /bin/sh whatever the policy says.
preload_fails() {
    case "$PLAT:$1" in
        freebsd:exec-bin-sh) echo "FreeBSD's libaether_sandbox.so answers fork() and system() with ENOSYS, granted or not: a bug in the preload, not a refusal" ;;
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
        getenv-home|env-cached) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy" ;;
        connect-*) echo "the worker's own lockdown has no tcp grant: the preload refuses the connect whatever the policy" ;;
        fork) echo "the worker's own lockdown has no fork grant: the preload refuses os.fork whatever the policy" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" || return 1
            echo "the worker's own lockdown has no exec or fork grant, and from ae $AE_SPAWN_PIN the preload holds system() to them" ;;
        *) return 1 ;;
    esac
}

# Factor's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find libfactor
# and its image, which specs to run, where the checks and the worker are, and
# how Factor's rows of stunt's catalogue differ from stunt's own expectation,
# each with why. lua/containment/lang.sh is the same for Lua.

. "$SCRIPT_DIR/factor_env.sh"

LANG_TITLE=Factor
LANG_SPECS="factor/test_host_factor.ae"
LANG_RUNTIME_ENV=AETHER_FACTOR_SONAME
LANG_NO_RUNTIME="no embed-api libfactor found (set AETHER_FACTOR_SONAME and AETHER_FACTOR_IMAGE; a distribution's Factor has no embedding API: factor/README.md, Caveat 1)"
lang_runtime() { factor_soname; }
# What the parity table says the column ran against: the fork's commit, when
# libfactor sits in its checkout, else the library's file.
lang_version() {
    d="$(dirname "$AETHER_FACTOR_SONAME")"
    c="$(git -C "$d" log -1 --format=%h 2>/dev/null)"
    if [ -n "$c" ]; then echo "the embed-api fork at $c"; else basename "$AETHER_FACTOR_SONAME"; fi
}
LANG_CHECKS_ENV=FACTOR_CHECKS
LANG_CHECKS_FILE=checks.factor
LANG_WORKER_ENV=FACTOR_WORKER
export FACTOR_PRELOAD="$ROOT/factor/containment/preload.factor"
LANG_EXTRA_TESTS=""

# Rows Factor cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        spin) echo "nothing can interrupt a Factor word that loops: the bridge has no instruction limit, and the contained child has no hook to look for a CANCEL from (the silent row is the same loop, ended by the host's kill)" ;;
        connect-*|syscall-*)
            [ "$PLAT" = linux ] && return 1
            echo "checks.factor builds the sockaddr and the system call numbers for Linux by hand" ;;
        *) return 1 ;;
    esac
}

# The rows that reach the system from Factor's own runtime rather than
# through the host: in-process, only the preload could hold them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|env-cached|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Factor and this ae: run
# expecting `succeed`, so the day they are held this goes red and the table in
# factor/README.md is brought up to date.
escapes_preload() {
    case "$1" in
        env-cached) echo "os-envs reads the process's environ array itself rather than asking getenv, so no interposer on getenv sees it" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: libc's system() starts /bin/sh with posix_spawn, a clone and an execve inside libc that this preload's execve and fork do not see" ;;
        *) return 1 ;;
    esac
}

child_stricter() {
    case "$1" in
        open-granted-file|write-granted-file) echo "the worker's own lockdown: a granted file comes as a descriptor, through read_file" ;;
        getenv-home|env-cached) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy" ;;
        connect-*) echo "the worker's own lockdown has no tcp grant: the preload refuses the connect whatever the policy" ;;
        fork) echo "the worker's own lockdown has no fork grant: the preload refuses fork whatever the policy" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" || return 1
            echo "the worker's own lockdown has no exec or fork grant, and from ae $AE_SPAWN_PIN the preload holds system() to them" ;;
        *) return 1 ;;
    esac
}

# The contained child cannot start Factor: libaether_sandbox.so, which
# sandbox.spawn_socket puts in every child, refuses an anonymous executable
# mmap (and any mprotect to PROT_EXEC) whatever the grants, and Factor's VM
# maps its code and callback heaps that way as it starts, so it dies in
# factor_embed_eval with "fatal_error: Out of memory in mmap". Given the
# probe's output, this says so (and the probe passes again the day the
# preload lets a JIT map its code: stunt_lib.sh then runs the child mode).
# factor/README.md has what runs in the child once it is let through.
lang_child_blocked() {
    grep -q "fatal_error: Out of memory in mmap" "$1" || return 1
    echo "Factor's VM maps its code heap anonymous and executable as it starts, which the preload in every spawn_socket child refuses (mmap_exec_anonymous): no Factor in a contained child on this ae (factor/README.md)"
}

# Perl's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find libperl,
# which specs to run, where the checks and the worker are, and how Perl's
# rows of stunt's catalogue differ from stunt's own expectation, each with
# why. python/containment/lang.sh is the same for Python.

. "$SCRIPT_DIR/perl_env.sh"

LANG_TITLE=Perl
LANG_SPECS="perl/test_host_perl.ae perl/test_host_perl_host.ae"
LANG_RUNTIME_ENV=AETHER_PERL_SONAME
LANG_NO_RUNTIME="no libperl found (set AETHER_PERL_SONAME)"
lang_runtime() { perl_soname && perl_libdirs; }
# What the parity table says the column ran against.
lang_version() {
    "${PERL_FOR_TESTS:-perl}" -e 'printf "%vd\n", $^V' 2>/dev/null
}
LANG_CHECKS_ENV=PERL_CHECKS
LANG_CHECKS_FILE=checks.pl
LANG_WORKER_ENV=PERL_WORKER
# Perl's own tests, beside the stunt ones.
LANG_EXTRA_TESTS="$ROOT/perl/containment/test_imports.sh"

# Rows Perl cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        getenv-home) echo "core Perl cannot ask libc's getenv: %ENV is the copy it took at start (env-cached), and the only other way to a variable is to start a program that prints it (exec-bin-sh)" ;;
        syscall-openat|syscall-socket) echo "Perl has no way to run the syscall instruction itself: that takes machine code in executable memory, and the only way to such memory without an XS module is mmap by libc's syscall(), which the sandbox preload answers EPERM (syscall-fn-openat is that row); DynaLoader runs code only from a shared object loaded from disk" ;;
        *) return 1 ;;
    esac
}

# Rows Perl cannot express in-process only, by name: the same for the
# in-process column, while the contained child runs them.
left_out_inproc() {
    case "$1" in
        spin) echo "the bridge has no instruction limit and Perl has no hook per op short of replacing the interpreter's run loop, which is interpreter layout; the contained child cancels at a deadline instead" ;;
        *) return 1 ;;
    esac
}

# The rows that go to libc from the language's own runtime: in-process,
# only the preload holds them. (env-cached is not one: the bridge hides what
# the grants do not grant from %ENV itself, with or without the preload.)
libc_row() {
    case "$1" in
        open-*|write-*|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Perl and this ae: run
# expecting `succeed`, so the day they are held this goes red and the table
# in perl/README.md is brought up to date. exec-bin-sh is held from
# AE_SPAWN_PIN on (aether #3580).
escapes_preload() {
    case "$1" in
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: Perl's system and backticks fork and then call execvp, and the preload before it neither refuses that fork (a plain fork() in a sandbox with no fork grant succeeds, observed) nor sees the exec, which glibc's execvp makes inside libc" ;;
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
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy" ;;
        connect-*) echo "the worker's own lockdown has no tcp grant: the preload refuses the connect whatever the policy" ;;
        fork) echo "the worker's own lockdown has no fork grant: it is refused whatever the policy" ;;
        exec-bin-sh) echo "the worker's own lockdown has no fork grant, and Perl starts a shell with fork, which is refused whatever the policy" ;;
        *) return 1 ;;
    esac
}

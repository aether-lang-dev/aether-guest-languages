# Racket's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find Racket CS's
# library, which specs to run, where the checks and the worker are, and how
# Racket's rows of stunt's catalogue differ from stunt's own expectation,
# each with why. python/containment/lang.sh is the same for Python.

. "$SCRIPT_DIR/racket_env.sh"

LANG_TITLE=Racket
# The rhombus spec only where this Racket has the rhombus collection (Racket
# 9's main distribution does not ship it: raco pkg install rhombus; Ubuntu
# 24.04's 8.10 predates it): where it does not, the spec would only skip.
LANG_SPECS="racket/test_host_racket.ae"
racket_has_rhombus && LANG_SPECS="$LANG_SPECS rhombus/test_host_rhombus.ae"
LANG_RUNTIME_ENV=AETHER_RACKET_SONAME
LANG_NO_RUNTIME="no Racket CS library found (set AETHER_RACKET_SONAME; tests/racket_env.sh says how one is found or made)"
lang_runtime() { racket_soname; }
# What the parity table says the column ran against.
lang_version() { racket_version; }
LANG_CHECKS_ENV=RACKET_CHECKS
LANG_CHECKS_FILE=checks.rkt
LANG_WORKER_ENV=RACKET_WORKER
# Racket's own tests, beside the stunt ones: none.
LANG_EXTRA_TESTS=""

# Rows Racket cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        env-cached) echo "Racket keeps no copy of the environment: getenv is libc's getenv (getenv-home)" ;;
        spin) echo "nothing stops an endless evaluation: a time limit is a Racket thread, and Racket CS's embedded scheduler does not reliably run one beside a computing thread (racket/README.md, Stopping a run)" ;;
        *) return 1 ;;
    esac
}

# The rows that reach the system from Racket's own runtime rather than
# through the host: in-process, only the preload could hold them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Racket and this ae:
# run expecting `succeed`, so the day they are held this goes red and the
# table in racket/README.md is brought up to date.
escapes_preload() { return 1; }

# Rows the contained child refuses where the policy would allow them: the
# std.guest worker locks itself down with no grants and is given no
# environment, so a granted file reaches the script only as a descriptor
# (binding-read-granted-file), a variable only as a host function's answer
# (binding-getenv-home), and libc never as a handle. Run expecting `refused`.
child_stricter() {
    case "$1" in
        open-granted-file|write-granted-file) echo "the worker's own lockdown: a granted file comes as a descriptor, through read_file" ;;
        getenv-home) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy" ;;
        connect-*) echo "the worker's own lockdown has no tcp grant: the preload refuses the connect whatever the policy" ;;
        fork) echo "the worker's own lockdown has no fork grant: the preload refuses fork whatever the policy" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" || return 1
            echo "the worker's own lockdown has no exec or fork grant, and from ae $AE_SPAWN_PIN the preload holds system() to them" ;;
        *) return 1 ;;
    esac
}

# The contained child (racket/worker) is the way this language does not have:
# the worker boots Racket CS before its lockdown and fails at the first
# evaluation after it, "allocation failed for code; maybe write+execute
# permission is not allowed". Chez Scheme maps anonymous executable memory
# for its generated code, and the preload that spawn_socket gives every
# contained child (sandbox_active) refuses exactly that (mmap_exec_anonymous,
# mprotect_exec). stunt_lib.sh's probe finds that out each run and skips child
# mode with the worker's own words; GHL_REQUIRE_CHILD=1 is not set for this
# language in CI. The Stunt parity table's column 4 says so by hand.

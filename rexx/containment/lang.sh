# Rexx's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/). Everything the harness
# needs to know about a language is here: how to find its runtime, which
# specs to run, where its checks and worker are, and how its rows of stunt's
# catalogue differ from stunt's own expectation, each with why.
#
# Rexx here is RexxAE, a Rexx engine written in Aether, from its own
# repository at the commit ci/versions.env pins (tests/rexx_env.sh builds its
# binary library and its worker). RexxAE has no I/O of its own, by design:
# its 71 builtins and 8 extensions compute (strings, numbers, JSON, cron
# arithmetic, log events the host receives), it has no STREAM, LINEIN,
# LINEOUT, CHARIN, CHAROUT or VALUE(..., 'ENVIRONMENT'), none of RexxJS's 79
# host-effect functions (FILE_READ, GETENV, HTTP_GET, ...), no way to load a
# library or call C, and ADDRESS is a request to the host, never a shell.
# test_surface.sh holds that over the engine's whole surface. So the rows
# that reach the system are not expressible at all, and what is left is the
# host's requests: its functions (binding-*, callback-*), the shared map, an
# ADDRESS environment (exec-bin-sh, through the host's SYSTEM environment in
# process; RexxAE's worker mounts none) and the host-checked rows.

. "$SCRIPT_DIR/rexx_env.sh"

LANG_TITLE=Rexx
# The bridge's spec, from the repository root.
LANG_SPECS="rexx/test_host_rexx.ae"
# lang_runtime builds RexxAE (tests/rexx_env.sh) and says where, as LANG_RUNTIME_ENV.
LANG_RUNTIME_ENV=REXXAE_LIB_DIR
LANG_NO_RUNTIME="RexxAE could not be fetched or built (REXXAE_SRC names a checkout; tests/rexx_env.sh)"
lang_runtime() {
    rexxae_engine || return 1
    # The hosts and the spec import rexxae from here: tests/stunt_lib.sh and
    # tests/run.sh add LANG_LIB to their module search path (--lib; ae's
    # AETHER_LIB_DIR does not find a binary library, asks/ae-lib-dir-binary-library.md).
    LANG_LIB="$REXXAE_LIB_DIR"
}
# What the parity table says the column ran against: RexxAE's commit.
lang_version() { echo "RexxAE $(echo "${REXXAE_COMMIT:-$REXXAE_REF}" | cut -c1-7)"; }
# The variables the containment hosts read: the checks' source, and the
# worker the contained-child host starts.
LANG_CHECKS_ENV=REXX_CHECKS
LANG_CHECKS_FILE=checks.rexx
LANG_WORKER_ENV=REXX_WORKER
# The contained child is RexxAE's own worker (worker/worker.ae at the pinned
# commit), built by tests/rexx_env.sh: there is no copy of it here.
lang_build() { LANG_WORKER_BUILT="$REXXAE_LIB_DIR/rexxae-worker"; }
# Rexx's own test beside the stunt ones: the engine's whole surface, held to
# reaching nothing.
LANG_EXTRA_TESTS="$ROOT/rexx/containment/test_surface.sh"

# Rows Rexx on RexxAE cannot express, by name. Each prints LEFT OUT and why.
# test_surface.sh is the evidence for every "has no" below: each name a
# script could reach for is called, under no grants, and comes out an
# unknown function or a computation that asked the host for nothing.
left_out() {
    case "$1" in
        open-*|write-granted-file) echo "RexxAE has no stream or file I/O: STREAM, LINEIN, LINEOUT, LINES, CHARIN, CHAROUT, CHARS and RexxJS's FILE_* are unknown functions (test_surface.sh); a file comes only through a host function (binding-read-*)" ;;
        getenv-home) echo "RexxAE has no VALUE(name,, 'ENVIRONMENT') and no GETENV of its own (an unknown function: test_surface.sh); a variable comes only through a host function (binding-getenv-home)" ;;
        env-cached) echo "RexxAE keeps no copy of the environment: the engine never reads it" ;;
        connect-*) echo "RexxAE has no sockets: RexxJS's HTTP_* are unknown functions (test_surface.sh), and ADDRESS is a request to the host, which provides no network environment" ;;
        fork) echo "RexxAE has no process API: ADDRESS is a request to the host, never a fork of its own" ;;
        syscall-*) echo "RexxAE has no syscall() and no way to call C: the engine is Aether code whose entry points are @pure and whose imports RexxAE pins (tools/check_containment.py)" ;;
        dlopen-*) echo "RexxAE cannot load a library: it has no FFI, no extern in the engine, and REQUIRE goes to a resolver the host mounts (none here)" ;;
        silent) echo "a Rexx script cannot keep RexxAE's worker from answering a CANCEL: the worker looks for one every 1000 instructions and while it waits on a call, and no single builtin runs long (a result is capped at 1 MiB), so a loop is spin's row, cancelled with an answer (the host's kill of a worker that never answers is RexxAE's worker-test, with a stand-in worker)" ;;
        half-frame) echo "a script cannot write part of a frame: the worker frames what it says" ;;
        *) return 1 ;;
    esac
}

# Rows that go to libc from the language's own runtime: in-process, only the
# preload would hold them. None: the engine reaches libc for nothing a script
# can ask for (test_surface.sh checks that under the preload too).
libc_row() { return 1; }

# Rows the contained child refuses where the policy would allow them. RexxAE's
# worker mounts no ADDRESS environment: every command is answered RC 1, "not
# mounted", whatever the grants (the in-process host's SYSTEM environment is
# the host's, not the worker's).
child_stricter() {
    case "$1" in
        exec-bin-sh) echo "RexxAE's worker mounts no ADDRESS environment: ADDRESS SYSTEM is answered RC 1, not mounted, whatever the grants" ;;
        *) return 1 ;;
    esac
}

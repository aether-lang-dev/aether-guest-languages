# Duktape's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/). Everything the harness
# needs to know about a language is here: how to find its runtime, which
# specs to run, where its checks and worker are, and how its rows of stunt's
# catalogue differ from stunt's own expectation, each with why.
#
# Duktape is ES5.1 and nothing else: no file, environment, process or socket
# API of its own. What a script can reach is what the bridge gives it as
# globals (print, env, readFile, fileExists, writeFile, exec), each checking
# the sandbox's grants itself, plus the host functions the host registers.
# So most rows are not libc calls from the interpreter that only the preload
# could hold (as for Lua): they are the bridge's own bindings, held on every
# platform, preload or none. The rows Duktape has no way to express are left
# out, below.

. "$SCRIPT_DIR/duktape_env.sh"

LANG_TITLE=Duktape
# The bridge's specs, from the repository root.
LANG_SPECS="duktape/test_host_duktape.ae duktape/test_host_duktape_vm.ae js/test_js_alias.ae"
# lang_runtime finds the runtime library and exports it as LANG_RUNTIME_ENV.
LANG_RUNTIME_ENV=AETHER_DUKTAPE_SONAME
LANG_NO_RUNTIME="no Duktape runtime found (set AETHER_DUKTAPE_SONAME)"
lang_runtime() { duktape_soname; }
# What the parity table says the column ran against: the library's file.
lang_version() {
    f="$AETHER_DUKTAPE_SONAME"
    real="$(readlink -f "$f" 2>/dev/null || echo "$f")"
    basename "$real"
}
# The variables the containment hosts read: the checks' source, and the
# worker the contained-child host starts.
LANG_CHECKS_ENV=DUKTAPE_CHECKS
LANG_CHECKS_FILE=checks.js
LANG_WORKER_ENV=DUKTAPE_WORKER
# Duktape's own tests beside the stunt ones: none.
LANG_EXTRA_TESTS=""

# Rows Duktape cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        env-cached) echo "Duktape keeps no copy of the environment: the bridge's env() is libc's getenv (getenv-home)" ;;
        connect-*) echo "Duktape has no sockets, and the bridge gives the script none" ;;
        fork) echo "Duktape has no fork, and the bridge gives the script none" ;;
        syscall-*) echo "Duktape has no syscall(), and no way to call C" ;;
        dlopen-*) echo "Duktape cannot load a library: it has no FFI, and the bridge gives the script none" ;;
        spin) echo "libduktape as packaged is built without DUK_USE_EXEC_TIMEOUT_CHECK, so the bridge has no instruction limit (vm_set_instruction_limit says so) and an in-process loop cannot be stopped; the worker has no cancel hook either (the silent row, below, is the host killing it)" ;;
        half-frame) echo "a script cannot write part of a frame: the worker frames what it says" ;;
        *) return 1 ;;
    esac
}

# The rows that go to libc from the language's own runtime: in-process,
# only the preload would hold them. None here: the bridge's bindings check
# the grants themselves before they call libc.
libc_row() { return 1; }

# Rows the contained child refuses where the policy would allow them: the
# std.guest worker locks itself down with no grants and is given no
# environment, and its script is not given the bridge's env, readFile,
# writeFile, fileExists or exec at all (worker/worker.ae removes them): a
# granted file reaches the script only as a descriptor (binding-read-granted-file),
# a variable only as a host function's answer (binding-getenv-home), and a
# process never. Run expecting `refused`.
child_stricter() {
    case "$1" in
        open-granted-file|write-granted-file) echo "the worker's script has no readFile or writeFile: a granted file comes as a descriptor, through read_file" ;;
        getenv-home) echo "the worker's script has no env(), and guest.launch gives the worker no environment: a variable comes through getenv" ;;
        exec-bin-sh) echo "the worker's script has no exec(), and its own lockdown has no exec or fork grant" ;;
        *) return 1 ;;
    esac
}

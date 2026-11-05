# QuickJS's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/). Everything the harness
# needs to know about a language is here: how to find its runtime, which
# specs to run, where its checks and worker are, and how its rows of stunt's
# catalogue differ from stunt's own expectation, each with why.
#
# QuickJS is ES2023 and nothing else: with quickjs-libc not compiled in it has
# no file, environment, process or socket API of its own, and no way to load
# a library or call C. The bridge gives a script `print`, `console.log` and,
# in a run with the shared map, aether_map_get and aether_map_put; the
# engine's host functions (contrib.quickjs) are what the host registers.
# Unlike Duktape's bridge it has no env, readFile, writeFile or exec. So the
# rows that reach the system are not expressible at all, and what is left is
# the host functions (binding-*, callback-*), the shared map and the host-
# checked rows.
#
# The engine is compiled into the program (aether's contrib.quickjs), so
# there is no runtime to find and no soname to give the worker.

. "$SCRIPT_DIR/quickjs_env.sh"

LANG_TITLE=QuickJS
# The bridge's specs, from the repository root.
LANG_SPECS="quickjs/test_host_quickjs.ae quickjs/test_quickjs_bridge.ae"
# lang_runtime says what the run used, as LANG_RUNTIME_ENV.
LANG_RUNTIME_ENV=QUICKJS_ENGINE
LANG_NO_RUNTIME="no contrib.quickjs in this ae (needs a release that ships it)"
lang_runtime() { quickjs_engine; }
# What the parity table says the column ran against: the engine's version.
lang_version() { echo "quickjs-ng $QUICKJS_VERSION"; }
# The variables the containment hosts read: the checks' source, and the
# worker the contained-child host starts.
LANG_CHECKS_ENV=QUICKJS_CHECKS
LANG_CHECKS_FILE=checks.js
LANG_WORKER_ENV=QUICKJS_WORKER
# QuickJS's own tests beside the stunt ones: none.
LANG_EXTRA_TESTS=""

# Rows QuickJS cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        env-cached) echo "QuickJS keeps no copy of the environment, and the bridge gives a script no env(): there is nothing to read it from (getenv-home)" ;;
        connect-*) echo "QuickJS has no sockets, and the bridge gives the script none" ;;
        fork) echo "QuickJS has no fork, and quickjs-libc (os.exec, os.fork) is not compiled in" ;;
        syscall-*) echo "QuickJS has no syscall(), and no way to call C" ;;
        dlopen-*) echo "QuickJS cannot load a library: it has no FFI, and quickjs-libc's os.dlopen is not compiled in" ;;
        open-*|write-granted-file) echo "the bridge gives a script no readFile or writeFile, and quickjs-libc (std.open, os.open) is not compiled in: a file comes only through a host function (binding-read-granted-file)" ;;
        getenv-home) echo "the bridge gives a script no env(), and quickjs-libc (std.getenv) is not compiled in: a variable comes only through a host function (binding-getenv-home)" ;;
        exec-bin-sh) echo "the bridge gives a script no exec(), and quickjs-libc (os.exec, std.popen) is not compiled in" ;;
        half-frame) echo "a script cannot write part of a frame: the worker frames what it says" ;;
        *) return 1 ;;
    esac
}

# Rows that go to libc from the language's own runtime: in-process, only the
# preload would hold them. None: the engine reaches libc for nothing a script
# can ask for.
libc_row() { return 1; }

# Rows the contained child refuses where the policy would allow them: none
# here. The bridge's in-process script has no more than the worker's does.
child_stricter() { return 1; }

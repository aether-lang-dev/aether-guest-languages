# Lua's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/). Everything the harness
# needs to know about a language is here: how to find its runtime, which
# specs to run, where its checks and worker are, and how its rows of stunt's
# catalogue differ from stunt's own expectation, each with why.

. "$SCRIPT_DIR/lua_env.sh"

LANG_TITLE=Lua
# The bridge's specs, from the repository root.
LANG_SPECS="lua/test_host_lua.ae lua/test_host_lua_vm.ae"
# lang_runtime finds the runtime library and exports it as LANG_RUNTIME_ENV.
LANG_RUNTIME_ENV=AETHER_LUA_SONAME
LANG_NO_RUNTIME="no Lua runtime found (set AETHER_LUA_SONAME)"
lang_runtime() { lua_soname; }
# What the parity table says the column ran against: the library's file.
lang_version() {
    f="$AETHER_LUA_SONAME"
    [ -e "$f" ] && f="$(cd "$(dirname "$f")" && basename "$(readlink "$(basename "$f")" || echo "$f")")"
    basename "$f"
}
# The variables the containment hosts read: the checks' source, and the
# worker the contained-child host starts.
LANG_CHECKS_ENV=LUA_CHECKS
LANG_CHECKS_FILE=checks.lua
LANG_WORKER_ENV=LUA_WORKER
# Lua's own tests beside the stunt ones: none.
LANG_EXTRA_TESTS=""

# Rows Lua cannot express, by name. Each prints LEFT OUT and why.
left_out() {
    case "$1" in
        env-cached) echo "Lua keeps no copy of the environment: os.getenv is libc's getenv (getenv-home)" ;;
        connect-*) echo "Lua's standard library has no sockets" ;;
        fork) echo "Lua's standard library has no fork" ;;
        syscall-*) echo "Lua's standard library has no syscall()" ;;
        half-frame) echo "a Lua script cannot write part of a frame: the worker frames what it says" ;;
        *) return 1 ;;
    esac
}

# The rows that go to libc from the language's own runtime: in-process,
# only the preload holds them.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for this language and this
# ae: run expecting `succeed`, so the day the preload holds them this goes red
# and the table in <lang>/README.md is brought up to date. exec-bin-sh is held
# from AE_SPAWN_PIN on (aether #3580).
escapes_preload() {
    case "$1" in
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: os.execute is system(), and glibc starts /bin/sh with posix_spawn, a clone and an execve inside libc that this preload's execve and fork do not see" ;;
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
        getenv-home) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        dlopen-libc-*) echo "the worker's own lockdown has no native grant: the preload refuses it a handle on libc whatever the policy" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" || return 1
            echo "the worker's own lockdown has no exec or fork grant, and from ae $AE_SPAWN_PIN the preload holds system() to them" ;;
        *) return 1 ;;
    esac
}

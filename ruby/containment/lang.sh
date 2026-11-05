# Ruby's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/): how to find libruby,
# which specs to run, where the checks and the worker are, and how Ruby's
# rows of stunt's catalogue differ from stunt's own expectation, each with
# why. python/containment/lang.sh is the same for Python.

. "$SCRIPT_DIR/ruby_env.sh"

LANG_TITLE=Ruby
LANG_SPECS="ruby/test_host_ruby.ae"
LANG_RUNTIME_ENV=AETHER_RUBY_SONAME
LANG_NO_RUNTIME="no libruby found (set AETHER_RUBY_SONAME)"
lang_runtime() { ruby_soname && ruby_stdlib; }
# What the parity table says the column ran against.
lang_version() {
    "${RUBY_FOR_TESTS:-ruby}" -e 'puts RUBY_VERSION' 2>/dev/null
}
LANG_CHECKS_ENV=RUBY_CHECKS
LANG_CHECKS_FILE=checks.rb
LANG_WORKER_ENV=RUBY_WORKER
# Ruby's own tests, beside the stunt ones.
LANG_EXTRA_TESTS="$ROOT/ruby/containment/test_requires.sh"

# Every row of the catalogue can be said in Ruby (checks.rb), so none is left
# out: sockets and fork are in the standard library (socket, Kernel#fork),
# libc's syscall() is Kernel#syscall, and the syscall instruction and the
# dlopen rows are Fiddle; ENV is the environment (see escapes_preload), and a
# script in the contained child can write part of a frame to the socket its
# worker talks to the host on.
left_out() { return 1; }

# The rows that reach the system from Ruby's own runtime rather than through
# the host: in-process, only the preload could hold them. getenv-home and
# env-cached are NOT among them, though they are libc rows: the bridge's
# run_sandboxed deletes the variables the grants do not name from the
# environment for the length of the run (it restores them after), so both
# are refused on every platform, with or without the preload.
libc_row() {
    case "$1" in
        open-*|write-*|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Ruby and this ae: run
# expecting `succeed`, so the day they are held this goes red and the table
# in ruby/README.md is brought up to date.
escapes_preload() {
    case "$1" in
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" && return 1
            echo "ae $ae_ver predates $AE_SPAWN_PIN: Kernel#system and backticks start /bin/sh with fork or posix_spawn, a clone and an execve this preload's execve and fork may not see" ;;
        *) return 1 ;;
    esac
}

# Rows where the preload itself fails, run expecting an error. FreeBSD's
# preload answers fork() and system() with ENOSYS even when fork and exec are
# granted (posix_spawn and vfork get through), so Kernel#system and backticks
# cannot start /bin/sh whatever the policy says.
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
        fork) echo "the worker's own lockdown has no fork grant: the preload refuses Kernel#fork whatever the policy" ;;
        exec-bin-sh)
            ver_ge "$ae_ver" "$AE_SPAWN_PIN" || return 1
            echo "the worker's own lockdown has no exec or fork grant, and from ae $AE_SPAWN_PIN the preload holds system() and spawning to them" ;;
        *) return 1 ;;
    esac
}

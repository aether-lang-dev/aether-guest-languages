#!/bin/sh
# Go's own: grant levels are per thread, and Go has threads of its own. Not a
# row of stunt's catalogue, but what decides whether a libc call a Go library
# makes is held: it is held only when made on the thread that holds the level.
# A goroutine started with `go` runs on another thread of the Go runtime's.
# checks.go's *-goroutine rows make the same libc calls from one.
#
#   in-process, no grants, preload   getenv-home is refused on the caller's
#                                    thread (test_stunt_no_grants.sh) and
#                                    succeeds from a goroutine: the
#                                    sandbox.enforce block's level is the
#                                    caller's thread's
#   contained child, native=*        dlopen of libc is refused on the thread
#                                    the worker locked down (test_stunt_
#                                    native.sh) and succeeds from a goroutine,
#                                    whose thread has only the policy's
#                                    process-wide grants; with no grants the
#                                    policy refuses it there too
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tinygo_threads
GUEST_LANG=tinygo
. "$SCRIPT_DIR/stunt_lib.sh"

policy "-"
if [ -n "$PRELOAD" ]; then
    run_one ESCAPES inproc succeed getenv-home-goroutine
else
    run_one "NOT HELD" inproc succeed getenv-home-goroutine
fi
if [ "$CHILD" = 1 ]; then
    policy "native=*"
    run_one ESCAPES child succeed dlopen-libc-by-name-goroutine
    policy "-"
    run_one PASS child refused dlopen-libc-by-name-goroutine
    run_one PASS child succeed threads
fi
if [ "$PLAT" = linux ]; then
    run_one PASS inproc succeed threads
fi
echo "  [PASS] $NAME"

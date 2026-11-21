#!/bin/sh
# What a script can do with Tcl's own library once the sandbox is in force:
# clock format, an encoding that is not built in, parray, each read from the
# library the first time it is used (checks.tcl, check::library-first-use).
#
# Where the library is files on disk (Tcl 8, a Tcl 9 built without zipfs), with
# no grant on the library directory:
#   in-process, with the preload   refused (the preload checks Tcl's open)
#   in-process, without a preload  succeeds (macOS: nothing holds Tcl's open)
#   contained child                refused (the worker locked itself down, so the
#                                  library is out of reach unless its warm-up
#                                  used it)
# Where it is in a zip attached to libtcl (a default Tcl 9, "zipfs"): it
# succeeds in every mode, because Tcl read the archive into memory when it
# mounted it, so using the library opens no file. The build is told by
# tests/tcl_env.sh (tcl_library: GHL_TCL_LIBRARY is empty for a zipfs build),
# and checked here against `info library` as the script itself sees it.

SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tcl_library_first_use
. "$SCRIPT_DIR/stunt_lib.sh"

# No grant at all: not even the library directory, which the child's host adds
# for Tcl_Init (GHL_TCL_LIBRARY): that one is for the start, and the first use
# is after the lockdown, whatever the policy.
if [ -z "${GHL_TCL_LIBRARY:-}" ]; then zipfs=1; else zipfs=0; fi

policy "-"
if [ "$zipfs" = 1 ] || [ -z "$PRELOAD" ]; then inproc=succeed; else inproc=refused; fi
run_one PASS inproc "$inproc" library-first-use
RAN=$((RAN + 1))
if [ "$CHILD" = 1 ]; then
    if [ "$zipfs" = 1 ]; then child=succeed; else child=refused; fi
    run_one PASS child "$child" library-first-use
    RAN=$((RAN + 1))
fi
if [ "$zipfs" = 1 ]; then
    echo "  [PASS] $NAME: $RAN as expected (the library is in the zip: no grant needed)"
elif [ -z "$PRELOAD" ]; then
    echo "  [PASS] $NAME: $RAN as expected (the library is files, and nothing here holds Tcl's open)"
else
    echo "  [PASS] $NAME: $RAN as expected (the library is files: a read grant, or the warm-up)"
fi

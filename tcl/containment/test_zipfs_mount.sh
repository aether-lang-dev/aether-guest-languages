#!/bin/sh
# A Tcl 9 with zipfs (a default build) reads what is already mounted, its own
# library, out of memory, with no open (test_library_first_use.sh). Mounting a
# zip archive of the script's own is different: Tcl opens the file, with a libc
# open, so the grants that hold `open` hold it:
#
#   no grants, with the preload          refused
#   no grants, without it (macOS)        succeeds (nothing holds Tcl's open)
#   fs_read on the archive, in-process   succeeds
#   contained child                      refused under every policy (the worker's
#                                        lockdown has no grant to give a script)
#
# Only for a build that has zipfs; any other Tcl has no zip to mount and the
# test says so. The archive is made here with python3's zipfile (no zip tool
# is assumed).

SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tcl_zipfs_mount
. "$SCRIPT_DIR/stunt_lib.sh"

[ -z "${GHL_TCL_LIBRARY:-}" ] && [ -n "${GHL_TCL_ZIPFS_LIB:-}" ] ||
    skip_all "this Tcl keeps its library in files, not in a zip (no zipfs)"
command -v python3 > /dev/null 2>&1 || skip_all "no python3 to make the archive with"
python3 -I -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("hello.txt", "from a zip\n")
' "$WORK/stunt-fixture.zip" || die "could not make the archive"

if [ -z "$PRELOAD" ]; then none=succeed; else none=refused; fi
policy "-"
run_one PASS inproc "$none" zipfs-mount
RAN=$((RAN + 1))
policy "fs_read=$WORK/*"
run_one PASS inproc succeed zipfs-mount
RAN=$((RAN + 1))
if [ "$CHILD" = 1 ]; then
    policy "fs_read=$WORK/*"
    run_one PASS child refused zipfs-mount
    RAN=$((RAN + 1))
fi
echo "  [PASS] $NAME: $RAN as expected (mounting an archive is an open the grants hold)"

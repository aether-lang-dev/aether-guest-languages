#!/bin/sh
# Tcl's script library, found three ways, with the spec tcl/test_host_tcl_library.ae
# run as a program each time:
#
#   1. as the build has it (on disk for Tcl 8 and a no-zipfs Tcl 9; in the zip
#      attached to libtcl for a default Tcl 9, where the bridge has to mount
#      it: tcl/README.md, "Tcl 9 and its zipfs");
#   2. the library copied out of wherever it is, into a directory;
#   3. TCL_LIBRARY naming that directory: Tcl reads it first, whatever the
#      build, so `info library` is the directory and not the zip.
#
# No grants and no sandbox: this is about initialisation, which every mode of
# the bridge does before any sandbox is in force. The rows in which a sandbox
# is in force are the containment tests.

SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tcl_library
. "$SCRIPT_DIR/stunt_lib.sh"

SPEC="$BUILD/test_host_tcl_library"
(cd "$ROOT" && ae build tcl/test_host_tcl_library.ae -o "$SPEC") > "$TMP/spec-build.log" 2>&1 ||
    die "tcl/test_host_tcl_library.ae did not build" "$TMP/spec-build.log"

# 1. As the build has it. TCL_LIBRARY is removed, in case the caller has one.
(cd "$WORK" && env -u TCL_LIBRARY "$SPEC") > "$TMP/as-built.txt" 2>&1 ||
    die "the library spec failed with the build's own library" "$TMP/as-built.txt"
echo "  [PASS] $NAME: the build's own library initialises and works"

# 2. Copied out. The target must not exist: `file copy` of a directory onto a
# new name makes that directory.
UNPACKED="$TMP/unpacked-library"
(cd "$WORK" && env -u TCL_LIBRARY GHL_UNPACK_TCL_LIBRARY="$UNPACKED" "$SPEC") > "$TMP/unpack.txt" 2>&1 ||
    die "copying the library out failed" "$TMP/unpack.txt"
[ -f "$UNPACKED/init.tcl" ] || die "no init.tcl in the copied library" "$TMP/unpack.txt"
from="$(sed -n 's/^UNPACKED_FROM //p' "$TMP/unpack.txt")"
echo "  [PASS] $NAME: the library copied out of $from"

# 3. TCL_LIBRARY names it.
(cd "$WORK" && TCL_LIBRARY="$UNPACKED" GHL_EXPECT_TCL_LIBRARY="$UNPACKED" "$SPEC") > "$TMP/tcl-library.txt" 2>&1 ||
    die "TCL_LIBRARY was not honoured" "$TMP/tcl-library.txt"
echo "  [PASS] $NAME: TCL_LIBRARY=<dir> is honoured, over $from"

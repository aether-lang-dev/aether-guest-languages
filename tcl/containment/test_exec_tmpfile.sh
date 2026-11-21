#!/bin/sh
# Tcl's bare `exec` (stderr not redirected) makes a temporary file for the
# command's stderr before it forks. That file is made by libc's mkstemp, which
# the preload checks (aether #3751, "a temporary file needs
# fs_write where it is made"; the open is read and write, so fs_read as well).
# Not a row of stunt's catalogue: stunt's exec-bin-sh is done with stderr
# redirected (checks.tcl, check::exec-bin-sh), and this test is what a Tcl
# script that does not redirect it meets. In-process only: the contained child
# has no fork grant, so its exec is refused whatever the policy.
#
#   exec=/bin/sh,fork                     bare exec: refused
#   the same with fs_read and fs_write on ./tmp/* and env=TMPDIR
#                                         bare exec with TMPDIR=./tmp: succeeds
# Where there is no preload (macOS) nothing holds Tcl's libc calls, and both
# succeed. The narrowest grant that lets the bare exec run is the second.

SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=tcl_exec_tmpfile
. "$SCRIPT_DIR/stunt_lib.sh"

mkdir -p "$WORK/tmp"
if [ -n "$PRELOAD" ]; then bare=refused; else bare=succeed; fi

policy "exec=/bin/sh,fork"
run_one PASS inproc "$bare" exec-bare
RAN=$((RAN + 1))
policy "exec=/bin/sh,fork,fs_read=$WORK/tmp/*,fs_write=$WORK/tmp/*,env=TMPDIR"
run_one PASS inproc succeed exec-bare-tmpdir
RAN=$((RAN + 1))
echo "  [PASS] $NAME: $RAN as expected (bare exec $bare under exec and fork alone)"

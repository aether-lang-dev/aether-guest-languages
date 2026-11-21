#!/bin/sh
# Rexx's own test beside the stunt ones: RexxAE's whole guest surface, held
# to reaching nothing (surface.ae says what is called and how it is judged).
# It is the evidence for the rows lang.sh leaves out as "RexxAE has no ...":
# those are claims about every name a script could reach for, from RexxAE's
# own inventories at the pinned commit, not about one spelling.
#
# Three parts:
#   1. Statically, from the engine's source at that commit: its function
#      lists (src/*.ae, the *_FUNCTIONS constants) are exactly
#      tests/guest_functions.json's names, so the sweep below covers what the
#      engine defines; and the engine imports no module that reaches the
#      system and declares no extern (RexxAE's tools/check_containment.py
#      holds the same with the compiler's @pure and --emit=lib probes).
#   2. surface.ae, in this process, inside sandbox.enforce with no grants:
#      no permission check made, nothing asked of the host; and every builtin
#      timed on 1 MiB strings, each within 3 s.
#   3. On Linux, the same under libaether_sandbox.so with denials logged to
#      stderr: none but the control's (a libc fopen of /etc/passwd the
#      program makes itself, to show the log would have caught one).
SCRIPT_DIR="$(cd "$(dirname "$0")/../../tests" && pwd)"
STUNT_TEST=rexx_surface
GUEST_LANG=rexx
. "$SCRIPT_DIR/stunt_lib.sh"

src="$REXXAE_SRC_DIR"

# ---- 1. the source ------------------------------------------------------------
listed="$(sed -n 's/^ *"\([A-Z][A-Z0-9_]*\)",\{0,1\}$/\1/p' "$src/tests/guest_functions.json" | sort -u)"
defined="$(sed -nE 's/^const (TEXT|MATH|VALUE|EXTENSION)_FUNCTIONS = "(.*)"$/\2/p' "$src"/src/*.ae | tr ' ' '\n' | sed '/^$/d' | sort -u)"
[ -n "$listed" ] || die "no names in $src/tests/guest_functions.json"
[ "$listed" = "$defined" ] ||
    die "the engine's function lists and tests/guest_functions.json differ: $(printf '%s\n' "$listed" "$defined" | sort | uniq -u | tr '\n' ' ')"
echo "  [PASS] source: $(echo "$listed" | wc -l | tr -d ' ') names defined in src/*.ae, the same as tests/guest_functions.json"
bad="$(grep -hE '^import ' "$src"/src/*.ae | awk '{ print $2 }' | sort -u |
       grep -vE '^(src\..*|std\.(string|map|json|math|strbuilder|list|sync|mem|bytes|host))$')"
[ -z "$bad" ] || die "the engine imports a module that could reach the system: $bad"
ext="$(grep -nE '^[[:space:]]*(@extern|extern )' "$src"/src/*.ae)"
[ -z "$ext" ] || die "the engine declares an extern: $ext"
echo "  [PASS] source: the engine imports only std.string, map, json, math, strbuilder, list, sync, mem, bytes and host (its quota, as RexxAE's check_containment.py pins), and declares no extern"

# ---- 2 and 3. the sweep ---------------------------------------------------------
SURFACE="$BUILD/rexx-surface"
build "rexx/containment/surface.ae" "$SURFACE"
(cd "$WORK" && $ROW_TIMEOUT "$SURFACE" "$src") > "$TMP/surface.txt" 2>&1 ||
    die "the surface sweep failed" "$TMP/surface.txt"
sed 's/^/  /' "$TMP/surface.txt" | grep -v '\[NOTE\]'
if [ -n "$PRELOAD" ]; then
    (cd "$WORK" && AETHER_SANDBOX_LOG=stderr LD_PRELOAD="$PRELOAD" $ROW_TIMEOUT "$SURFACE" "$src" control-libc) > "$TMP/preload.txt" 2>&1 ||
        die "the surface sweep failed under the preload" "$TMP/preload.txt"
    grep -q '^  \[PASS\] control: ' "$TMP/preload.txt" ||
        die "the control's libc fopen of /etc/passwd was not refused under the preload" "$TMP/preload.txt"
    denied="$(grep 'AETHER_DENIED' "$TMP/preload.txt")"
    others="$(printf '%s\n' "$denied" | grep -v '/etc/passwd' | sed '/^$/d')"
    [ -n "$denied" ] || die "the preload logged no denial, not even the control's" "$TMP/preload.txt"
    [ -z "$others" ] || die "the preload refused something during the sweep: $others" "$TMP/preload.txt"
    echo "  [PASS] preload: the sweep again under $(basename "$PRELOAD"), denials logged: only the control's ($(printf '%s\n' "$denied" | wc -l | tr -d ' ') line(s), /etc/passwd)"
else
    echo "  [SKIP] preload: no libaether_sandbox.so here (macOS): the sweep ran in-process only"
fi
echo "  [PASS] $NAME"

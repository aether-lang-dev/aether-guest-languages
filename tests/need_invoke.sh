#!/bin/sh
# Whether the `ae` on PATH has the host-calls-guest verb (std.guest's invoke,
# std.guest.child's ready and serve), which every worker here is built on. An
# ae without it would fail every build here with "Undefined function" errors
# that say nothing about why; this says it once, plainly. Exit 0 when it is
# there. Sourced or run with ROOT set (tests/run.sh, tests/stunt_lib.sh); the
# answer is kept per ae version in build/.
#
# The first release with the verb is the one after 0.934.0 that contains
# std.guest.invoke.
: "${ROOT:=$(cd "$(dirname "$0")/.." && pwd)}"
mkdir -p "$ROOT/build"
ver="$(ae version 2>/dev/null | awk 'NR == 1 { print $2 }')"
mark="$ROOT/build/.has-invoke-$ver"
if [ ! -f "$mark" ]; then
    probe="$ROOT/build/.invoke-probe"
    mkdir -p "$probe"
    cat > "$probe/probe.ae" <<'AE'
import std.guest
import std.guest.child
fn call_it(g: *guest.Guest) -> string {
    return guest.invoke(g, "f", "[]")
}
main() {
    h = child.attach(-1, "probe")
    if h.ch == 0 {
        inv = child.serve(h)
        println(inv.kind)
    }
    println(h.name)
}
AE
    if (cd "$probe" && ae build probe.ae -o probe) > "$probe/log" 2>&1; then
        : > "$mark"
    else
        echo "  [FAIL] this ae ($ver) has no host-calls-guest verb (std.guest's invoke, std.guest.child's serve)," \
             "which the workers here are built on: the first release with it is the one after 0.934.0 that contains std.guest.invoke"
        sed 's/^/    /' "$probe/log" | head -5
        exit 1
    fi
fi
exit 0

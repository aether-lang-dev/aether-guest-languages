# Sourced: QuickJS is compiled into the program (aether's contrib.quickjs,
# which a release ships with its amalgamation), so there is no library to find
# and no soname to give. This checks that the ae on PATH has the engine, sets
# QUICKJS_ENGINE to say where it is, and QUICKJS_VERSION to the pin it reads
# from the engine's lock file.
quickjs_engine() {
    [ -n "${QUICKJS_ENGINE:-}" ] && return 0
    home="$(ae version 2>/dev/null | sed -n 's/^Binary: *//p' | head -1)"
    # <prefix>/bin/ae -> <prefix>/share/aether/contrib/quickjs
    dir="$(dirname "$(dirname "$home")")/share/aether/contrib/quickjs"
    if [ -f "$dir/module.ae" ] && [ -f "$dir/amalgamation/quickjs-amalgam.c" ]; then
        export QUICKJS_ENGINE="$dir"
        QUICKJS_VERSION="$(sed -n 's/^QUICKJS_VERSION=//p' "$dir/amalgamation.lock")"
        export QUICKJS_VERSION
        return 0
    fi
    return 1
}

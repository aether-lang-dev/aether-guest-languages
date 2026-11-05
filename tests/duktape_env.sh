# Sourced: finds the Duktape runtime library the bridge dlopens and exports
# it as AETHER_DUKTAPE_SONAME, unless that is set already. The bridge itself
# tries AETHER_DUKTAPE_SONAME and then the usual names (libduktape.so.207,
# libduktape.so.2, libduktape.so, Homebrew's); this is the orchestrator's
# probe, so a run says which library it used and a machine with a name the
# bridge does not know is still found.
duktape_soname() {
    [ -n "${AETHER_DUKTAPE_SONAME:-}" ] && return 0
    case "$(uname -s 2>/dev/null)" in
        Darwin)
            # Homebrew's duktape installs libduktape.so (not .dylib).
            prefix="$(brew --prefix duktape 2>/dev/null)"
            for f in "$prefix/lib/libduktape.207.so" "$prefix/lib/libduktape.so" \
                     "$prefix/lib/libduktape.dylib" /opt/homebrew/lib/libduktape.so \
                     /opt/homebrew/lib/libduktape.dylib /usr/local/lib/libduktape.so \
                     /usr/local/lib/libduktape.dylib; do
                [ -f "$f" ] && { export AETHER_DUKTAPE_SONAME="$f"; return 0; }
            done ;;
        *)
            # Debian/Ubuntu: libduktape.so.207 (package libduktape207); Arch
            # and Fedora: libduktape.so.207 or libduktape.so.2.x. The newest
            # the loader's cache has wins.
            cache="$(/sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null)"
            so="$(printf '%s\n' "$cache" | awk '{print $1}' |
                  grep -E '^libduktape\.so(\.[0-9.]+)?$' | sort -V | tail -1)"
            [ -n "$so" ] && { export AETHER_DUKTAPE_SONAME="$so"; return 0; } ;;
    esac
    return 1
}

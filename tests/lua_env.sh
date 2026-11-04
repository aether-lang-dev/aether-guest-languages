# Sourced: finds the Lua runtime library the bridge dlopens and exports it as
# AETHER_LUA_SONAME, unless that is set already. The bridge itself tries only
# AETHER_LUA_SONAME and then liblua.so (lua/README.md); this is the
# orchestrator's probe it leaves to the caller.
lua_soname() {
    [ -n "${AETHER_LUA_SONAME:-}" ] && return 0
    case "$(uname -s 2>/dev/null)" in
        Darwin)
            # Homebrew's lua is the newest; LUA_VERSION=5.4 asks for lua@5.4.
            keg=lua
            [ -n "${LUA_VERSION:-}" ] && keg="lua@$LUA_VERSION"
            for f in "$(brew --prefix "$keg" 2>/dev/null)/lib/liblua.dylib" \
                     /opt/homebrew/lib/liblua.dylib /usr/local/lib/liblua.dylib; do
                [ -f "$f" ] && { export AETHER_LUA_SONAME="$f"; return 0; }
            done ;;
        *)
            # Debian/Ubuntu name it liblua5.4.so.0, Arch liblua5.4.so.5.4 or
            # liblua.so.5.5, Fedora liblua-5.4.so. The newest Lua the loader's
            # cache has wins; LUA_VERSION=5.4 asks for that one.
            cache="$(/sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null)"
            for v in ${LUA_VERSION:-5.5 5.4 5.3}; do
                so="$(printf '%s\n' "$cache" | awk '{print $1}' |
                      grep -E "^(liblua$v\.so\.[0-9.]+|liblua\.so\.$v(\.[0-9]+)?|liblua-$v\.so)\$" | head -1)"
                [ -n "$so" ] && { export AETHER_LUA_SONAME="$so"; return 0; }
            done ;;
    esac
    return 1
}

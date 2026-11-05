# Sourced: finds the Racket CS library the bridge dlopens and the files it
# boots from, and exports them as AETHER_RACKET_SONAME, AETHER_RACKET_BOOT_DIR,
# AETHER_RACKET_COLLECTS and AETHER_RACKET_CONFIG, unless AETHER_RACKET_SONAME
# is set already (then it is all yours). The bridge itself finds the boot
# files and collects beside the library when they are not given
# (racket/README.md); this probe is the orchestrator's, and the only part that
# makes a library where Racket does not ship one.
#
# What Racket CS needs to boot embedded:
#   the library       Racket.framework/Versions/<v>/Racket on macOS (Racket's
#                     own distribution, brew install --cask racket). Upstream
#                     builds no shared libracketcs elsewhere ("--enable-shared
#                     is not currently supported"); Debian and Ubuntu ship the
#                     static libracketcs.a, from which this makes
#                     build/libracketcs.so (cc -shared -Wl,-Bsymbolic, no
#                     Racket dev package: the archive is in the racket
#                     package itself).
#   the boot images   petite.boot, scheme.boot and racket.boot (racket.so in
#                     some builds): Racket.framework/Versions/<v>/boot on
#                     macOS, /usr/lib/racket on Debian and Ubuntu.
#   the collects      the installation's collects directory (the module
#                     library, read as boot finishes and for each require),
#                     and its etc/ (config.rktd) as the config directory.
#
# RACKET_HOME names an installation's root (default: the newest /Applications/
# Racket v*, else the directory above `racket`'s bin); RACKET_LIB names a
# libracketcs.a to make the shared library from.
# The directories a contained child must be able to read for Racket to boot
# in it (GHL_RACKET_READ, comma-separated): the library's, the boot images',
# the collects directory (and the installation's share/racket around it, where
# the package links and the config live) and the config directory. No row of
# stunt's catalogue reads there. Racket reads its module library from disk the
# first time a module is required (racket/README.md, "In a contained child").
ghl_racket_read() {
    r="$(dirname "$AETHER_RACKET_SONAME"),$AETHER_RACKET_BOOT_DIR,$AETHER_RACKET_COLLECTS,$AETHER_RACKET_CONFIG"
    [ -n "$AETHER_RACKET_COLLECTS" ] && r="$r,$(dirname "$AETHER_RACKET_COLLECTS")"
    export GHL_RACKET_READ="$r"
}

racket_soname() {
    [ -n "${AETHER_RACKET_SONAME:-}" ] && return 0
    here="${SCRIPT_DIR:-$(cd "$(dirname "$0")" && pwd)}"
    case "$(uname -s 2>/dev/null)" in
        Darwin)
            home="${RACKET_HOME:-}"
            if [ -z "$home" ]; then
                for d in "/Applications/Racket v"*; do [ -d "$d" ] && home="$d"; done
            fi
            [ -z "$home" ] && home="$(brew --prefix minimal-racket 2>/dev/null)"
            fw=""
            for f in "$home"/lib/Racket.framework/Versions/*/Racket "$home"/Racket.framework/Versions/*/Racket; do
                [ -f "$f" ] && fw="$f"
            done
            [ -n "$fw" ] || return 1
            fwdir="$(dirname "$fw")"
            export AETHER_RACKET_SONAME="$fw"
            export AETHER_RACKET_BOOT_DIR="$fwdir/boot"
            [ -d "$home/collects" ] && export AETHER_RACKET_COLLECTS="$home/collects"
            [ -d "$home/etc" ] && export AETHER_RACKET_CONFIG="$home/etc"
            ghl_racket_read
            [ -d "$AETHER_RACKET_BOOT_DIR" ] ;;
        *)
            lib="${RACKET_LIB:-}"
            if [ -z "$lib" ]; then
                for f in /usr/lib/libracketcs.a /usr/lib64/libracketcs.a /usr/local/lib/libracketcs.a \
                         /usr/lib/*-linux-gnu/libracketcs.a; do
                    [ -f "$f" ] && { lib="$f"; break; }
                done
            fi
            [ -n "$lib" ] || return 1
            boot=""
            for d in /usr/lib/racket /usr/local/lib/racket /usr/lib64/racket "$(dirname "$lib")/racket"; do
                [ -f "$d/petite.boot" ] && [ -f "$d/scheme.boot" ] && { boot="$d"; break; }
            done
            [ -n "$boot" ] || return 1
            coll=""
            for d in /usr/share/racket/collects /usr/local/share/racket/collects; do
                [ -d "$d" ] && { coll="$d"; break; }
            done
            [ -n "$coll" ] || return 1
            out="$here/../build/libracketcs.so"
            mkdir -p "$(dirname "$out")"
            if [ ! -f "$out" ] || [ "$lib" -nt "$out" ]; then
                # -Bsymbolic binds the archive's symbols locally, which is what
                # lets position-dependent objects into a shared library;
                # libz, liblz4 and libtinfo are what the racket executable links.
                ${CC:-cc} -shared -Wl,-Bsymbolic -o "$out" \
                    -Wl,--whole-archive "$lib" -Wl,--no-whole-archive \
                    -lm -ldl -lpthread -l:libz.so.1 -l:liblz4.so.1 -l:libtinfo.so.6 \
                    > "$here/../build/libracketcs.log" 2>&1 || { rm -f "$out"; return 1; }
            fi
            export AETHER_RACKET_SONAME="$out"
            export AETHER_RACKET_BOOT_DIR="$boot"
            export AETHER_RACKET_COLLECTS="$coll"
            for d in /etc/racket /usr/local/etc/racket; do
                [ -d "$d" ] && { export AETHER_RACKET_CONFIG="$d"; break; }
            done
            ghl_racket_read ;;
    esac
    return 0
}

# The Racket version, for the parity table: from the command if there is one,
# else from the library's directory.
racket_version() {
    v="$(${RACKET:-racket} --version 2>/dev/null | sed -n 's/^Welcome to Racket v\([0-9.]*\).*/\1/p')"
    [ -n "$v" ] && { echo "$v"; return; }
    basename "$(dirname "${AETHER_RACKET_SONAME:-}")" | sed 's/_CS$//'
}

# Whether this Racket has the rhombus collection (rhombus/test_host_rhombus.ae
# runs only if it does).
racket_has_rhombus() {
    ${RACKET:-racket} -e '(collection-file-path "main.rkt" "rhombus")' > /dev/null 2>&1
}

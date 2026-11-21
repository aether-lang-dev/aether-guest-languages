# Sourced: finds the Tcl runtime library the bridge dlopens and exports it as
# AETHER_TCL_SONAME, unless that is set already. The bridge itself tries
# AETHER_TCL_SONAME, then a short list of names (tcl/README.md, "Loading
# libtcl"); this is the orchestrator's probe for a machine where the library
# is somewhere else, or where there are several Tcls and a test run should
# pick one.
#
# TCL_VERSION=8.6 (or 9.0, 8.5) asks for that one; the default order is 8.6,
# 9.0, 8.5. On macOS Homebrew's tcl-tk comes before the system's Tcl 8.5,
# which is in the dyld cache (/usr/lib/libtcl.dylib has no file on disk, but
# dlopen finds it).
tcl_soname() {
    [ -n "${AETHER_TCL_SONAME:-}" ] && return 0
    versions="${TCL_VERSION:-8.6 9.0 8.5}"
    case "$(uname -s 2>/dev/null)" in
        Darwin)
            for v in $versions; do
                for prefix in "$(brew --prefix tcl-tk 2>/dev/null)" "$(brew --prefix tcl-tk@8 2>/dev/null)" \
                              /opt/homebrew/opt/tcl-tk /usr/local/opt/tcl-tk; do
                    f="$prefix/lib/libtcl$v.dylib"
                    [ -f "$f" ] && { export AETHER_TCL_SONAME="$f"; return 0; }
                done
            done
            # The system's Tcl is 8.5, unless the caller asked for another.
            case " $versions " in *" 8.5 "*)
                export AETHER_TCL_SONAME=/usr/lib/libtcl.dylib; return 0 ;;
            esac ;;
        *)
            # Debian, Ubuntu and Fedora ship libtcl8.6.so.0 in the runtime
            # package and libtcl8.6.so only in the dev one; Arch ships
            # libtcl8.6.so; FreeBSD's is libtcl86.so.
            cache="$(/sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null)"
            for v in $versions; do
                nodot="$(echo "$v" | tr -d .)"
                so="$(printf '%s\n' "$cache" | awk '{print $1}' |
                      grep -E "^libtcl($v|$nodot)\.so(\.[0-9.]+)?\$" | sort -r | head -1)"
                [ -n "$so" ] && { export AETHER_TCL_SONAME="$so"; return 0; }
            done
            if [ "$(uname -s)" = FreeBSD ]; then
                for v in $versions; do
                    f="/usr/local/lib/libtcl$(echo "$v" | tr -d .).so"
                    [ -f "$f" ] && { export AETHER_TCL_SONAME="$f"; return 0; }
                done
            fi
            # Last resort: ask a tclsh which library it is.
            so="$(echo 'puts libtcl$tcl_version.so' | "$(tcl_shell)" 2>/dev/null)"
            [ -n "$so" ] && { export AETHER_TCL_SONAME="$so"; return 0; } ;;
    esac
    return 1
}

# A tclsh on PATH: Debian and Ubuntu's tcl8.6 package has only tclsh8.6 (the
# unversioned name comes with the tcl metapackage).
tcl_shell() {
    for c in tclsh tclsh8.6 tclsh9.0 tclsh8.5; do
        p="$(command -v "$c" 2>/dev/null)" && { echo "$p"; return 0; }
    done
    return 1
}

# The tclsh that belongs to the library that was found: the one beside it in
# the same prefix (<prefix>/lib/libtclX.Y.so, <prefix>/bin/tclshX.Y), which is
# how a Tcl built by hand in a private prefix is asked about itself, and which
# is also the system's own for /usr/lib/libtcl8.6.so. A library named by a bare
# soname (libtcl8.6.so.0) has no prefix: the tclsh on PATH.
tcl_shell_for_library() {
    f="$1"
    case "$f" in
        */*)
            prefix="$(dirname "$(dirname "$f")")"
            for c in "$prefix"/bin/tclsh[0-9]*; do [ -x "$c" ] && { echo "$c"; return 0; }; done ;;
    esac
    tcl_shell
}

# The version of the library that was found, from the tclsh that belongs to it
# when there is one, else from the library's file name.
tcl_version() {
    f="$AETHER_TCL_SONAME"
    sh_bin=""
    case "$f" in
        /opt/homebrew/opt/tcl-tk/*|/usr/local/opt/tcl-tk/*|*/tcl-tk*/lib/*) sh_bin="$(dirname "$(dirname "$f")")/bin/tclsh" ;;
        /usr/lib/libtcl.dylib) sh_bin=/usr/bin/tclsh ;;
        *) sh_bin="$(tcl_shell_for_library "$f")" ;;
    esac
    # Homebrew names its shell tclsh9.0 / tclsh8.6 beside the library.
    if [ ! -x "$sh_bin" ]; then
        for c in "$(dirname "$(dirname "$f")")"/bin/tclsh[0-9]*; do [ -x "$c" ] && { sh_bin="$c"; break; }; done
    fi
    if [ -x "$sh_bin" ]; then
        v="$(echo 'puts $tcl_patchLevel' | "$sh_bin" 2>/dev/null)"
        want="$(basename "$f" | sed -n 's/^libtcl\([0-9]\)\.\{0,1\}\([0-9]\).*/\1.\2/p')"
        case "$v" in "$want"*|"") ;; *) v="" ;; esac
        [ -z "$want" ] && [ -n "$v" ] && { echo "$v"; return 0; }
        [ -n "$v" ] && { echo "$v"; return 0; }
    fi
    basename "$f"
}

# The directories Tcl reads when an interpreter is made (Tcl_Init sources
# init.tcl from tcl_library), comma-separated, as GHL_TCL_LIBRARY: what a
# contained child must be granted to read for the worker to start at all
# (tcl/README.md, "In a contained child"). Asked of the tclsh that belongs to
# the library, as tcl_version does. A Tcl 9 with its library in a zip attached
# to libtcl (zipfs) answers //zipfs:/lib/tcl/tcl_library, which is no
# directory and needs no grant: GHL_TCL_LIBRARY is then set and empty, and
# GHL_TCL_ZIPFS_LIB is the file the archive is mounted from (libtcl itself,
# as Tcl resolved it), which is what a contained child has to be granted
# instead ("Tcl 9 and its zipfs" in tcl/README.md).
tcl_library() {
    [ -n "${GHL_TCL_LIBRARY+x}" ] && return 0
    f="$AETHER_TCL_SONAME"
    sh_bin="$(tcl_shell_for_library "$f")"
    case "$f" in
        /opt/homebrew/opt/tcl-tk/*|/usr/local/opt/tcl-tk/*) sh_bin="$(dirname "$(dirname "$f")")/bin/tclsh" ;;
    esac
    [ -x "$sh_bin" ] || return 1
    # TCL_LIBRARY, if the caller has one, is what Tcl reads first.
    lib="$(echo 'puts $tcl_library' | "$sh_bin" 2>/dev/null)"
    [ -n "$lib" ] || return 1
    case "$lib" in
        //zipfs:*)
            lib=""
            GHL_TCL_ZIPFS_LIB="$(echo 'puts [zipfs mount //zipfs:/lib/tcl]' | "$sh_bin" 2>/dev/null)"
            export GHL_TCL_ZIPFS_LIB ;;
    esac
    GHL_TCL_LIBRARY="$lib"
    export GHL_TCL_LIBRARY
}

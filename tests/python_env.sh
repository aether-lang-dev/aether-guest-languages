# Sourced: finds the libpython the bridge dlopens and exports it as
# AETHER_PYTHON_SONAME, unless that is set already. The bridge itself tries
# only AETHER_PYTHON_SONAME and then libpython3.so (python/README.md); this
# is the orchestrator's probe it leaves to the caller.
#
# It asks a python3 for its own library: sysconfig's LIBDIR and INSTSONAME.
# PYTHON names the interpreter to ask (default: python3 on PATH; on macOS,
# Homebrew's newest python3 before Xcode's, whose 3.9 is not a supported
# one). A framework build (macOS) reports INSTSONAME relative to its
# framework prefix, Python.framework/Versions/3.13/Python, which is resolved
# against PYTHONFRAMEWORKPREFIX.
python_interp() {
    py="${PYTHON:-}"
    if [ -z "$py" ] && [ "$(uname -s 2>/dev/null)" = Darwin ]; then
        for v in 3.14 3.13 3.12 3.11 3.10; do
            p="$(brew --prefix "python@$v" 2>/dev/null)/bin/python$v"
            [ -x "$p" ] && { py="$p"; break; }
        done
    fi
    [ -n "$py" ] || py="$(command -v python3 2>/dev/null)"
    [ -n "$py" ] || return 1
    PYTHON_FOR_TESTS="$py"
}

python_soname() {
    python_interp || return 1
    [ -n "${AETHER_PYTHON_SONAME:-}" ] && return 0
    so="$("$py" -c 'import sysconfig as s
v = s.get_config_var
name = v("INSTSONAME") or v("LDLIBRARY") or ""
for d in (v("PYTHONFRAMEWORKPREFIX"), v("LIBDIR")):
    if d and name and not name.startswith("/"):
        import os
        if os.path.exists(os.path.join(d, name)):
            name = os.path.join(d, name)
            break
print(name)' 2>/dev/null)"
    case "$so" in
        /*) [ -f "$so" ] || return 1 ;;
        *.so*|*.dylib) ;;   # a bare soname: the loader's search finds it
        *) return 1 ;;
    esac
    export AETHER_PYTHON_SONAME="$so"
    return 0
}

# The standard library's directories, comma-separated, as GHL_PYTHON_STDLIB:
# what a contained child must be granted to read for CPython to start at all
# (python/README.md, "In a contained child").
python_stdlib() {
    [ -n "${GHL_PYTHON_STDLIB:-}" ] && return 0
    python_interp || return 1
    GHL_PYTHON_STDLIB="$("$PYTHON_FOR_TESTS" -c 'import sysconfig as s
print(",".join(sorted({s.get_paths()["stdlib"], s.get_paths()["platstdlib"]})))' 2>/dev/null)" || return 1
    export GHL_PYTHON_STDLIB
}

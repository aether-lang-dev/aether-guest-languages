# Sourced: finds the embed-api fork of Factor's libfactor and its bootstrapped
# image, and exports them as AETHER_FACTOR_SONAME and AETHER_FACTOR_IMAGE,
# unless both are set already. The bridge itself tries only
# AETHER_FACTOR_SONAME and then libfactor.so, and takes the image only from
# AETHER_FACTOR_IMAGE (factor/README.md); this is the orchestrator's probe it
# leaves to the caller.
#
# A distribution's Factor does not do: it has no factor_embed_eval, the entry
# point the fork adds (factor/README.md, "Caveat 1"). So a candidate counts
# only if it holds that symbol, and the probe says nothing is found otherwise.
#
# Where it looks, in order: $FACTOR_HOME (a directory of the fork, built:
# libfactor.so or libfactor.dylib or libfactor.a, and factor.image beside it),
# then ~/scm/factor-language and ~/factor-language. Factor's own makefile
# names the shared engine libfactor.a on Linux, though it is an ELF shared
# object, which dlopen takes by path.
factor_soname() {
    if [ -n "${AETHER_FACTOR_SONAME:-}" ] && [ -n "${AETHER_FACTOR_IMAGE:-}" ]; then
        [ -n "${FACTOR_HOME:-}" ] || FACTOR_HOME="$(dirname "$AETHER_FACTOR_IMAGE")"
        export FACTOR_HOME
        return 0
    fi
    for d in "${FACTOR_HOME:-}" "$HOME/scm/factor-language" "$HOME/factor-language"; do
        [ -n "$d" ] && [ -f "$d/factor.image" ] || continue
        for f in "$d/libfactor.so" "$d/libfactor.dylib" "$d/libfactor.a"; do
            [ -f "$f" ] || continue
            # A shared object, not an ar archive; and the fork's, not upstream's.
            [ "$(head -c 2 "$f" | od -An -c | tr -d ' ')" = '!<' ] && continue
            grep -q factor_embed_eval "$f" 2>/dev/null || continue
            export AETHER_FACTOR_SONAME="$f" AETHER_FACTOR_IMAGE="$d/factor.image"
            # The tree the image was made from (basis/, extra/), which the
            # contained child's policy reads one file of (child_host.ae).
            export FACTOR_HOME="$d"
            return 0
        done
    done
    return 1
}

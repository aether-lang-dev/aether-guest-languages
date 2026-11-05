# Sourced: finds the libperl the bridge dlopens and exports it as
# AETHER_PERL_SONAME, unless that is set already. The bridge itself tries
# only AETHER_PERL_SONAME and then libperl.so (perl/README.md); this is the
# orchestrator's probe it leaves to the caller.
#
# It asks a perl for its own library: Config's archlibexp and libperl,
# "<archlibexp>/CORE/<libperl>" (libperl.so.5.40 under
# /usr/lib/x86_64-linux-gnu/perl/5.40/CORE on Debian and Ubuntu, libperl.so
# under /usr/lib/perl5/<ver>/core_perl/CORE on Arch, libperl.dylib under
# /System/Library/Perl/<ver>/darwin-thread-multi-2level/CORE on macOS).
# PERL names the interpreter to ask (default: perl on PATH; on macOS,
# Homebrew's before /usr/bin/perl). A perl built without a shared libperl
# (useshrplib off) has none to load: that is a failure here.
perl_interp() {
    pl="${PERL:-}"
    if [ -z "$pl" ] && [ "$(uname -s 2>/dev/null)" = Darwin ]; then
        for p in /opt/homebrew/opt/perl/bin/perl /usr/local/opt/perl/bin/perl; do
            [ -x "$p" ] && { pl="$p"; break; }
        done
    fi
    [ -n "$pl" ] || pl="$(command -v perl 2>/dev/null)"
    [ -n "$pl" ] || return 1
    PERL_FOR_TESTS="$pl"
}

perl_soname() {
    perl_interp || return 1
    [ -n "${AETHER_PERL_SONAME:-}" ] && return 0
    so="$("$pl" -MConfig -e 'print "$Config{archlibexp}/CORE/$Config{libperl}"' 2>/dev/null)"
    [ -f "$so" ] || return 1
    export AETHER_PERL_SONAME="$so"
    return 0
}

# The directories perl reads its modules from, comma-separated, as
# GHL_PERL_LIB: what a contained child must be granted to read for perl to
# start and to load the modules the worker imports (perl/README.md, "In a
# contained child"). Config's own list of the library directories, which exist.
perl_libdirs() {
    [ -n "${GHL_PERL_LIB:-}" ] && return 0
    perl_interp || return 1
    GHL_PERL_LIB="$("$PERL_FOR_TESTS" -MConfig -e 'my %s; print join(",", grep { -d $_ && !$s{$_}++ } map { $Config{$_} } qw(privlibexp archlibexp sitelibexp sitearchexp vendorlibexp vendorarchexp))' 2>/dev/null)" || return 1
    [ -n "$GHL_PERL_LIB" ] || return 1
    export GHL_PERL_LIB
}

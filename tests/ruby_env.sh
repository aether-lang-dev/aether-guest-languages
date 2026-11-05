# Sourced: finds the libruby the bridge dlopens and exports it as
# AETHER_RUBY_SONAME, unless that is set already. The bridge itself tries
# only AETHER_RUBY_SONAME and then libruby.so (ruby/README.md); this is the
# orchestrator's probe it leaves to the caller.
#
# It asks a ruby for its own library: RbConfig's libdir and LIBRUBY_SO. RUBY
# names the interpreter to ask (default: ruby on PATH; on macOS, Homebrew's
# ruby before the system one, whose 2.6 is older than the bridge supports).
# libruby is the shared library of a Ruby built with --enable-shared, which
# Debian, Ubuntu, Arch, Fedora and Homebrew all do.
ruby_interp() {
    rb="${RUBY:-}"
    if [ -z "$rb" ] && [ "$(uname -s 2>/dev/null)" = Darwin ]; then
        for keg in ruby ruby@3.4 ruby@3.3 ruby@3.2; do
            p="$(brew --prefix "$keg" 2>/dev/null)/bin/ruby"
            [ -x "$p" ] && { rb="$p"; break; }
        done
    fi
    [ -n "$rb" ] || rb="$(command -v ruby 2>/dev/null)"
    [ -n "$rb" ] || return 1
    RUBY_FOR_TESTS="$rb"
}

ruby_soname() {
    ruby_interp || return 1
    [ -n "${AETHER_RUBY_SONAME:-}" ] && return 0
    so="$("$rb" -rrbconfig -e 'puts File.join(RbConfig::CONFIG["libdir"], RbConfig::CONFIG["LIBRUBY_SO"])' 2>/dev/null)"
    [ -f "$so" ] || return 1
    export AETHER_RUBY_SONAME="$so"
    # The bridge starts Ruby without RubyGems (ruby/README.md). A library that
    # is a bundled gem there (fiddle, from Ruby 3.5) is found only with it:
    # AETHER_RUBY_GEMS=1 loads it.
    if [ -z "${AETHER_RUBY_GEMS:-}" ] &&
       ! "$rb" --disable-gems -e 'require "fiddle"; require "json"; require "socket"; require "rbconfig"' > /dev/null 2>&1; then
        export AETHER_RUBY_GEMS=1
    fi
    return 0
}

# Ruby's own library directories, comma-separated, as GHL_RUBY_STDLIB: what a
# contained child must be granted to read for Ruby to start (ruby/README.md,
# "In a contained child"). The standard library is Ruby files on disk, read
# the first time a script requires one, and its C extensions (socket, fiddle,
# json) are shared objects beside them.
ruby_stdlib() {
    [ -n "${GHL_RUBY_STDLIB:-}" ] && return 0
    ruby_interp || return 1
    GHL_RUBY_STDLIB="$("$RUBY_FOR_TESTS" -rrbconfig -e 'c = RbConfig::CONFIG
puts [c["rubylibdir"], c["rubyarchdir"], c["sitelibdir"], c["sitearchdir"], c["vendorlibdir"], c["vendorarchdir"]].compact.reject(&:empty?).uniq.sort.join(",")' 2>/dev/null)" || return 1
    export GHL_RUBY_STDLIB
}

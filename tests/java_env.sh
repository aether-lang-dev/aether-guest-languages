# Sourced: finds a JDK (21 or later) and exports what the tests and the
# bridge use:
#
#   JAVA_HOME   the JDK
#   JAVA_BIN    the java command (the bridge's java.run* start it)
#   JAVA_MAJOR  its major version
#   JAVA_FLAGS  flags every program the tests start takes: native access for
#               the Foreign Function API, and --enable-preview on JDK 21,
#               where that API is a preview (java/build.sh compiles for it)
#
# The bridge starts a JVM as a child, so nothing is loaded and there is no
# libjvm to find. Looks at JAVA_HOME, then /usr/libexec/java_home -v 21+ on
# macOS, Homebrew's openjdk@21 / openjdk, and /usr/lib/jvm/* on Linux, then
# javac on the PATH.
java_home_ok() {   # java_home_ok <dir>: a JDK of 21 or later
    [ -x "$1/bin/java" ] && [ -x "$1/bin/javac" ] || return 1
    m="$("$1/bin/javac" -version 2>&1 | sed -n 's/^javac \([0-9]*\).*/\1/p')"
    [ -n "$m" ] && [ "$m" -ge 21 ]
}

java_tool() {
    if [ -n "${JAVA_HOME:-}" ] && java_home_ok "$JAVA_HOME"; then :; else
        JAVA_HOME=""
        cands=""
        if [ -x /usr/libexec/java_home ]; then
            cands="$(/usr/libexec/java_home -v 21+ 2>/dev/null)"
        fi
        for d in /opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home /opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home \
                 /usr/local/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home /opt/homebrew/opt/openjdk@21 /opt/homebrew/opt/openjdk; do
            cands="$cands
$d"
        done
        for d in /usr/lib/jvm/*; do
            cands="$cands
$d"
        done
        if command -v javac > /dev/null 2>&1; then
            cands="$cands
$(dirname "$(dirname "$(readlink -f "$(command -v javac)" 2>/dev/null || command -v javac)")")"
        fi
        for d in $cands; do
            [ -n "$d" ] && java_home_ok "$d" && { JAVA_HOME="$d"; break; }
        done
    fi
    [ -n "$JAVA_HOME" ] || return 1
    # The JDK proper: a Homebrew prefix links bin/ but keeps lib/ elsewhere.
    real="$(readlink -f "$JAVA_HOME/bin/java" 2>/dev/null)"
    [ -n "$real" ] && [ -x "$(dirname "$(dirname "$real")")/bin/javac" ] && JAVA_HOME="$(dirname "$(dirname "$real")")"
    JAVA_BIN="$JAVA_HOME/bin/java"
    JAVA_MAJOR="$("$JAVA_HOME/bin/javac" -version 2>&1 | sed -n 's/^javac \([0-9]*\).*/\1/p')"
    JAVA_FLAGS="--enable-native-access=ALL-UNNAMED"
    [ "$JAVA_MAJOR" -lt 22 ] && JAVA_FLAGS="$JAVA_FLAGS --enable-preview"
    # Distributions link the JDK's configuration and trust store to /etc
    # (conf -> /etc/java-21-openjdk, lib/security/cacerts -> /etc/ssl/certs/java):
    # a JVM that starts a network connection or a TLS one reads the targets, and a
    # grant is for the file as the kernel opens it. Colon-separated.
    GHL_JAVA_READ=""
    for l in "$JAVA_HOME/conf" "$JAVA_HOME/lib/security"; do
        if [ -e "$l" ]; then
            t="$(readlink -f "$l" 2>/dev/null)"
            [ -n "$t" ] && [ "$t" != "$l" ] && GHL_JAVA_READ="$GHL_JAVA_READ:$t"
        fi
    done
    for l in "$JAVA_HOME/lib/security/cacerts"; do
        if [ -L "$l" ]; then
            t="$(dirname "$(readlink -f "$l" 2>/dev/null)")"
            [ -n "$t" ] && GHL_JAVA_READ="$GHL_JAVA_READ:$t"
        fi
    done
    GHL_JAVA_READ="${GHL_JAVA_READ#:}"
    export JAVA_HOME JAVA_BIN JAVA_MAJOR JAVA_FLAGS GHL_JAVA_READ
    return 0
}

# The platform has no libaether_sandbox.so (macOS), so java.run_sandboxed
# starts nothing: the containment hosts then use java.run, which the rows'
# "not held" result is about.
java_uncontained() {
    if [ "$(uname -s 2>/dev/null)" = Darwin ]; then
        export GHL_JAVA_UNCONTAINED=1
    fi
}

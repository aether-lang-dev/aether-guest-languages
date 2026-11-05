#!/bin/sh
# Builds the Java side into <out> (default ../build/java):
#
#   classes/            everything below, compiled
#   aether-sandbox.jar  the agent (src/): -javaagent:aether-sandbox.jar
#
# The programs the tests run (containment/, worker/, examples/) are in
# classes/ and run with -cp. Needs a JDK of 21 or later: with 21 the Foreign
# Function API (src/Native.java) is a preview feature, so the sources are
# compiled with --enable-preview and every program runs with it
# (tests/java_env.sh puts that in JAVA_FLAGS); from 22 it is final and
# neither is needed. javac and jar come from $JAVA_HOME, or the PATH.
#
#   sh java/build.sh [outdir]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/../build/java}"
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
    JAVAC="$JAVA_HOME/bin/javac"; JAR="$JAVA_HOME/bin/jar"
else
    JAVAC=javac; JAR=jar
fi
MAJOR="$("$JAVAC" -version 2>&1 | sed -n 's/^javac \([0-9]*\).*/\1/p')"
[ -n "$MAJOR" ] && [ "$MAJOR" -ge 21 ] || { echo "build.sh: needs a JDK 21 or later (javac says: $("$JAVAC" -version 2>&1))" >&2; exit 1; }
FLAGS=""
[ "$MAJOR" -lt 22 ] && FLAGS="--release 21 --enable-preview"

# Rebuild only when a source is newer than the jar.
if [ -f "$OUT/aether-sandbox.jar" ] && [ -z "$(find "$DIR" -name '*.java' -newer "$OUT/aether-sandbox.jar" 2>/dev/null)" ] &&
   [ "$DIR/MANIFEST.MF" -ot "$OUT/aether-sandbox.jar" ]; then
    exit 0
fi

rm -rf "$OUT/classes"
mkdir -p "$OUT/classes"
# shellcheck disable=SC2046
"$JAVAC" $FLAGS -Xlint:-preview -d "$OUT/classes" $(find "$DIR" -name '*.java' | sort)
"$JAR" cfm "$OUT/aether-sandbox.jar" "$DIR/MANIFEST.MF" -C "$OUT/classes" aether
echo "built: $OUT/aether-sandbox.jar, $OUT/classes (javac $MAJOR)"

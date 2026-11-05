# Java's side of the test harness, sourced by tests/run.sh and
# tests/stunt_lib.sh (with SCRIPT_DIR set to tests/). Java is not embedded:
# the bridge starts a JVM, so the "runtime" is the JDK that builds and runs
# the checks and the worker, and what differs from stunt's expectation is
# mostly what a JVM does for itself: it reads the JDK before it runs a line,
# it makes its threads, and System.getenv is a copy.

. "$SCRIPT_DIR/java_env.sh"

LANG_TITLE=Java
# The bridge's spec and the agent's grant checker (a Java program), and the
# containment tests with java/containment/test_ways.sh (LANG_EXTRA_TESTS).
LANG_SPECS="java/test_host_java.ae"
LANG_RUNTIME_ENV=JAVA_HOME
LANG_NO_RUNTIME="no JDK 21 or later found (set JAVA_HOME)"
lang_runtime() { java_tool && java_uncontained; }
lang_version() { "$JAVA_BIN" -version 2>&1 | sed -n '1s/^[^"]*"\([^"]*\)".*/\1/p'; }
LANG_CHECKS_ENV=JAVA_CLASSES
LANG_CHECKS_FILE=Checks.java
LANG_WORKER_ENV=JAVA_WORKER
LANG_EXTRA_TESTS="$ROOT/java/containment/test_ways.sh"

# Builds the Java side once per run (java/build.sh: the agent jar and the
# classes the checks and the worker are in) and says where the agent is. The
# build hook of stunt_lib.sh. The flags every JVM here takes: native access
# and, on JDK 21, preview (java_env.sh); no performance data file (the JVM
# would create one in /tmp, which no policy grants); a serial collector and
# the first compiler tier, which make a JVM start with fewer threads and less
# work.
lang_build() {
    mkdir -p "$BUILD"
    sh "$ROOT/java/build.sh" "$BUILD/java" > "$TMP/javabuild.log" 2>&1 || die "java/build.sh failed" "$TMP/javabuild.log"
    java_exports "$BUILD"
    LANG_WORKER_BUILT="$BUILD/java/classes"
}

# What the programs and the specs read from the environment, once the Java side
# is built under <build>/java.
java_exports() {
    export JAVA_CLASSES="$1/java/classes"
    export GHL_REAL_JAVA_BIN="$JAVA_BIN"
    export AETHER_JAVA_AGENT="$1/java/aether-sandbox.jar"
    export AETHER_JAVA_FLAGS="$JAVA_FLAGS -XX:-UsePerfData -XX:+UseSerialGC -XX:TieredStopAtLevel=1"
}

# tests/run.sh calls this before the specs, which need the same environment as
# the containment tests: the Java side built and the JDK found.
lang_spec_env() {
    java_tool && java_uncontained || return 0
    mkdir -p "$ROOT/build"
    sh "$ROOT/java/build.sh" "$ROOT/build/java" > "$ROOT/build/javabuild.log" 2>&1 || { cat "$ROOT/build/javabuild.log"; return 1; }
    java_exports "$ROOT/build"
}

# Rows Java cannot express, by name: none. The two raw system call rows need
# the Foreign Function API and a few bytes of machine code (Native.rawSyscall);
# they run where the child does (Linux) and are in the child's list only.
left_out() { return 1; }

# Rows a plain child cannot run, the contained child can: a child process has
# no host functions (the bridge gives it a class and an exit status, and no way
# to call back), and the host cannot stop a child that loops (the bridge waits
# for it).
left_out_inproc() {
    case "$1" in
        binding-*|callback-*) echo "a child process has no host functions: the bridge gives it an exit status, its output and the shared map, and no way to call the host" ;;
        spin) echo "java.run_sandboxed waits for the JVM to exit: there is no instruction limit or cancel to stop a loop (std.guest's deadline is the contained child's)" ;;
        map-*)
            [ "$PLAT" = macos ] || return 1
            echo "macOS has no libaether_sandbox.so, so the _with_map run, the only one that hands the child the map, does not start" ;;
        *) return 1 ;;
    esac
}

# The rows that reach the system from the JVM's own runtime rather than through
# the host: in a plain child, only the preload could hold them. The JDK's
# native code calls libc for files, sockets, processes and libraries, so
# unlike Go the preload sees them; System.getenv is the exception.
libc_row() {
    case "$1" in
        open-*|write-*|getenv-home|env-cached|connect-*|exec-bin-sh|dlopen-*) return 0 ;;
        *) return 1 ;;
    esac
}

# Rows the preload is there to hold but does not, for Java: run expecting
# `succeed`, so the day they are held this goes red and the table in
# java/README.md is brought up to date.
escapes_preload() {
    case "$1" in
        env-cached) echo "System.getenv is the copy of the environment the JDK took out of libc's environ at its first use; no libc getenv is called" ;;
        *) return 1 ;;
    esac
}

# Rows where the preload itself fails: none found for Java.
preload_fails() { return 1; }

# Rows the contained child refuses where the policy would allow them: the
# std.guest worker is started with no environment, so a variable comes only
# through the host's getenv. A JVM cannot enter an Aether sandbox block, so the
# grants on the spawn are the worker's own authority, as for Go: Landlock lets
# it open a granted file itself, seccomp lets it make a socket.
child_stricter() {
    case "$1" in
        getenv-home|env-cached) echo "guest.launch gives the worker no environment: a variable comes through getenv" ;;
        *) return 1 ;;
    esac
}

# Rows the contained child lets through where stunt expects a refusal: see the
# containment table.
child_escapes() {
    case "$1:$POLICY" in
        connect-*:*tcp=*) echo "seccomp lets socket(AF_INET) through for any tcp grant and Landlock filters ports, not addresses; the address is the preload's check on libc's connect" ;;
        *) return 1 ;;
    esac
}

# A JVM does not start under libaether_sandbox.so. The preload refuses every
# mprotect that adds PROT_EXEC and every anonymous executable mmap while a
# sandbox is active (runtime/libaether_sandbox_preload.c, "block PROT_EXEC to
# prevent shellcode"), and HotSpot needs both: it checks at startup that it can
# mark a page executable, and its interpreter and compilers live in executable
# memory it maps. It dies with BLOCK_MARK before it runs a line of Java. The
# preload is part of both the sandbox block (ways 2 and 3) and the contained
# child (way 4: sandbox.spawn_socket always adds it, and keeps it across an
# exec), so no row of the catalogue can run in either. The tests check the
# death, not assume it: the day a wall lets a JVM start the probe passes, the
# rows run, and the table is brought up to date.
BLOCK_MARK="Failed to mark memory page as executable"
wall_blocks() {
    case "$1" in
        inproc) echo "the preload refuses the JVM's executable memory (mprotect and mmap with PROT_EXEC), so java.run_sandboxed starts a JVM that dies at startup" ;;
        child) echo "the contained child is started with the preload, which refuses the JVM's executable memory, so the worker dies at startup" ;;
        *) return 1 ;;
    esac
}

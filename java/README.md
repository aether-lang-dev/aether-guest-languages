# java: Java as a separate JVM process

`import java` lets an Aether program run a Java program. It is the bridge that
was aether's `contrib.host.java` ([PROVENANCE.md](PROVENANCE.md)): a Java
agent (`src/`) that reads the grant list and the shared map from shared
memory, and `java.grant_jvm_runtime()`, the helper that bundles what a JVM
reads before it runs a line. This package adds what aether left to the caller,
the calls that start the JVM: `java.run`, `java.run_sandboxed` and
`java.run_sandboxed_with_map`, the shape of `go.run` (`aether_host_java.c` is
`aether_host_go.c` with `java` for `go`).

The JVM is never in your process. It has its own runtime, threads and signal
handlers and cannot share a process with another runtime, so there is no
`libjvm` to load, no JNI, no `jni.h`: every way below starts a **child**, the
`java` command, on a class you built. Like Go, and unlike the interpreters
here, the bridge loads and links nothing. It needs a JDK 21 or later to build
the Java side (`java/build.sh`: `javac` and `jar`) and a JRE to run it.

JDK 21 or later, because the agent talks to libc through the Foreign Function
API (`java.lang.foreign`): final from 22, a preview in 21. One source builds
on both: `src/Native.java` uses only what the two share, `build.sh` compiles
with `--enable-preview` on 21, and every program takes `--enable-preview` on
21 (`tests/java_env.sh` puts that in `AETHER_JAVA_FLAGS`, which the bridge
reads). aether's agent used JDK 22's `allocateFrom`, and so did not compile
on 21.

## Four ways to run Java from Aether

From least to most contained. All four stay supported: pick by how far you
trust the program, not by which is newest. **Read "The wall that stops a JVM"
below before ways 2, 3 and 4 on Linux: today a JVM does not start inside them.**

| | 1. Child, no containment | 2. Child under a grant list | 3. The same, with the preload (Linux) | 4. Contained child |
|---|---|---|---|---|
| Where Java runs | its own JVM (`java.run`) | its own JVM (`java.run_sandboxed`) | its own JVM (the same call) | its own JVM (`java/worker`, started by `guest.launch`) |
| How the grants reach it | none | `AETHER_SANDBOX_SHM`, read by the agent; nothing on macOS: it does not start | `LD_PRELOAD` and `AETHER_SANDBOX_SHM` in the child's environment | Landlock and seccomp from the grant list, the preload, and a socket |
| Your host functions | none | none | none | checked against the grants; each call is a message the host answers |
| The JDK's own file, socket and process calls | not checked | not checked | would be seen by the preload (libc calls, below) | would be refused beyond what the grants allow |
| **Today, on Linux** | works | **the JVM dies at startup** | **the JVM dies at startup** | **the JVM dies at startup** |
| Platforms | everywhere a JDK runs | Linux (macOS: starts nothing, returns -1) | Linux | Linux with Landlock; macOS refuses to start it |
| Needs | ae 0.875.0 | ae 0.875.0 | ae 0.875.0 | ae 0.878.0 (`std.guest`) |

Ways 2 and 3 are one call, as for Go: `java.run_sandboxed` always starts the
child with `libaether_sandbox.so` in `LD_PRELOAD`, and where the library does
not exist (macOS) it starts nothing.

### 1. A plain child: no containment

```aether
import java

main() {
    // sh java/build.sh build/java
    rc = java.run("build/java/classes", "ghl.examples.Hello")   // the JVM's exit status; -1 if it could not start
    println("exit ${rc}")
}
```

`java.run(classpath, main_class)` forks and executes `$JAVA_BIN` (else `java`
on `PATH`) with `$AETHER_JAVA_FLAGS` (separated by spaces), `-cp <classpath>`
and the class, with the host's environment and standard streams. It returns
the JVM's exit status, or -1 when it cannot start (no such `java`, no
permission) or was killed by a signal: never a 127 that a program which ran
could also give. A class that is not there is `java`'s own exit status, 1.
Nothing contains the child. [examples/Hello.java](examples/Hello.java) is the
program, and [examples/ways.ae](examples/ways.ae) starts it the first three
ways. Set up the environment as the example's header says:

```sh
export JAVA_BIN=$JAVA_HOME/bin/java
export AETHER_JAVA_FLAGS="--enable-native-access=ALL-UNNAMED --enable-preview"   # --enable-preview on JDK 21 only
export AETHER_JAVA_AGENT=build/java/aether-sandbox.jar                           # added to the sandboxed runs
```

### 2. A child under a grant list

```aether
import java
import std.os
import std.sandbox

main() {
    reader = sandbox.new("reader") {
        java.grant_jvm_runtime()                 // what a JVM reads before it runs a line
        grant_fs_read("${os.getenv("JAVA_HOME")}/*")
        grant_fs_read("build/java/classes/*")
    }
    rc = java.run_sandboxed(reader, "build/java/classes", "ghl.examples.Hello")
    println("exit ${rc}")
}
```

What `java.run_sandboxed` does, read from `aether_host_java.c` (it is
`go.run_sandboxed`'s, see [../go/README.md](../go/README.md)):

1. It looks for `libaether_sandbox.so` (`AETHER_SANDBOX_PRELOAD`, beside the
   program, under `AETHER_HOME`). If there is none, which is always the case
   on macOS, it prints why and **returns -1 having started nothing**: it fails
   closed.
2. It writes the grant list, as `category:pattern` lines, to POSIX shared
   memory (`/aether_host_java_<pid>`). An empty list is written too and means
   nothing is granted.
3. It forks, and in the child, calling only `execve`, starts
   `java $AETHER_JAVA_FLAGS -javaagent:$AETHER_JAVA_AGENT -cp ... Main` with
   the host's environment plus `LD_PRELOAD=<the library>` and
   `AETHER_SANDBOX_SHM=<the segment>` (and `AETHER_MAP_SHM` with a map). The
   agent is added when `AETHER_JAVA_AGENT` names its jar.
4. The host waits, reads back what the child wrote to the map, removes the
   segments and returns the exit status.

No grant is checked in the host's process, except that inside a
`sandbox.enforce` block that grants no `exec` the call returns -1 and nothing
starts. The **agent** (`-javaagent`, [src/](src/)) reads the grants at
`premain` (shared memory through `Native`, the file in
`AETHER_SANDBOX_GRANTS` as a fallback) and gives the program a cooperative
check: `AetherSandboxHooks.checkFileRead(path)`, `checkFileWrite`,
`checkConnect(host, port)`, `checkExec` and `checkEnv(name)`, which throw
`SecurityException` (or return null) for what the list does not grant. That is
a check the program makes of itself, not a wall, and it is the only
Java-specific check there is: `System.getenv` is a copy taken at startup and
cannot be scrubbed, so `checkEnv` is how a Java program honours an `env`
grant. The matcher is the one every other place uses (ports, IPv4-mapped
addresses, globs: [PROVENANCE.md](PROVENANCE.md)).

### 3. The same, with the preload: what it would hold in a JVM

`libaether_sandbox.so` interposes libc: `open`, `connect`, `execve`, `fork`,
`posix_spawn`, `getenv`, `dlopen`, `system`. Unlike a Go program, a Java
program **does** call libc for what the catalogue asks: the JDK's native code
(`libnio`, `libnet`, `libjava`, the launcher's `jspawnhelper`) is C, and calls
`open64`, `connect`, `posix_spawn` and `dlopen` through the procedure linkage
table. What a recording interposer, [`containment/libghltrace.c`](containment/libghltrace.c),
sees of each row, run by [`containment/test_ways.sh`](containment/test_ways.sh)
on CachyOS (OpenJDK 25):

- `Files.readAllBytes` and `Files.write`: libc `open` (the JDK's `open64`).
- `SocketChannel` (non-blocking): libc `connect`, `AF_INET` for an `INET`
  channel and `AF_INET6` with the address as `::ffff:192.0.2.1` for an `INET6`
  one. (A `SocketChannel` opened without a family is `AF_INET6` too: the JDK
  is dual-stack, so the IPv4-mapped normalisation is what a Java program
  needs.)
- `ProcessBuilder`: libc `posix_spawn` of `$JAVA_HOME/lib/jspawnhelper`, which
  then `execve`s `/bin/sh`. A grant list therefore needs `exec` for
  `jspawnhelper` (and, from ae 0.884.0, `fork`), besides `/bin/sh`.
- `System.load(path)`: libc `dlopen`.
- `System.getenv`: **not** libc. The JDK copies `environ` at its first use and
  never calls `getenv` again, so nothing the preload does to `getenv` reaches
  it (the `env-cached` row).
- A Foreign Function API call to a libc function: seen **only if the symbol
  is looked up the way a C caller's is**. `Linker.defaultLookup()` finds
  libc's own function in a helper library of the JDK, past anything
  preloaded; `aether.sandbox.Native` looks it up with `dlsym(RTLD_DEFAULT)`,
  which finds the preload's first. A Java program that reaches libc with the
  default lookup escapes the preload.
- The system call instruction itself, which no JDK API issues, is
  `Native.rawSyscall`: a few bytes of machine code in a page of its own. No
  libc is involved, so the preload cannot see it; only the kernel layer of way
  4 holds it.

So the preload would be a real, if partial, library interposer for a JVM. It is
not one that can be tested today, because of the next section.

### The wall that stops a JVM

`libaether_sandbox.so` refuses every `mprotect` that adds `PROT_EXEC` and every
anonymous `mmap` with `PROT_EXEC` while a sandbox is active (its "block
PROT_EXEC to prevent shellcode execution" rule, with no exception for a JIT).
HotSpot needs both: at startup it checks that it can mark a page executable,
and its interpreter and compilers live in executable memory it maps. So a JVM
under the preload dies before it runs a line of Java:

```
$ printf 'fs_read:/usr/lib/jvm/*\nfs_read:/proc/*\nfs_read:/sys/*\n' > grants
$ LD_PRELOAD=~/.aether/lib/libaether_sandbox.so AETHER_SANDBOX_GRANTS=grants java -version
Error occurred during initialization of VM
Failed to mark memory page as executable - check if grsecurity/PaX is enabled
```

(OpenJDK 25.0.5 on CachyOS, ae 0.889.0 and 0.878.0; with no sandbox source set
the preload is inert and the JVM starts. `-Xint` does not help: the template
interpreter is generated machine code.) This is not a fault of this
package. It is how `java.run_sandboxed` and `guest.launch` behave for any
runtime that generates code, and it means the README of aether's bridge ("LD_PRELOAD
enforces file/network/exec at the libc level") was not true of a running JVM.
The preload is not optional where it matters: `sandbox.spawn_socket` always
adds it, and it puts `LD_PRELOAD` back across an `exec` (a worker that does
`unset LD_PRELOAD` and execs `java` still gets it). So no JVM runs in ways 2, 3
or 4 on Linux. What would unblock it is Aether's: a grant (or an exception for
W^X-compliant JIT mappings) that lets a runtime keep executable memory, with the
kernel layer unchanged.

Observed once, not tested by the suite: with a local build of the preload that
has those three `PROT_EXEC` conditions disabled, the JVM starts under it and in
the contained child, and most rows of the catalogue come out as the table of
another language would show them (files by Landlock, the connect by seccomp,
`ProcessBuilder` by the exec and fork grants). It is not clean yet: the
preload's catch-all refusal of libc's `syscall()` (EPERM for every number it
does not name) also reaches HotSpot, which calls `syscall(SYS_futex)` for its
wait barrier, so the JVM prints a fatal-error report at exit
(`waitBarrier_linux.cpp: futex FUTEX_WAIT failed; error='Operation not
permitted'`); and a `SocketChannel` connect failed with `Error loading
java.security file` there, which was not investigated. So lifting the
`PROT_EXEC` rule is necessary, and not on its own enough.

The tests check the death, not assume it ([`containment/lang.sh`](containment/lang.sh),
`wall_blocks`, and `tests/stunt_lib.sh`): every test script probes the JVM under
the preload and in the contained child, which must exit with that message,
and records one **blocked** cell per mode in the parity table below. The day a
wall lets a JVM start, the probe passes, the catalogue's rows run in those
modes, and the table is brought up to date by `--write-parity`. Everything
that does not need the wall is tested: the rows with no wall around them (macOS,
and Linux through `java.run`), the worker and its protocol with
`guest.launch_uncontained`, the agent's two shared-memory readers, the grant
checker, and what libc sees.

### 4. A contained child, over `std.guest`

```aether
import std.guest
import std.list
import std.os
import std.sandbox

main() {
    // sh java/build.sh build/java
    jh = os.getenv("JAVA_HOME")
    perms = sandbox.new("java") {                // a JVM reads the JDK before it runs a line
        grant_fs_read("${jh}/*")
        grant_fs_read("build/java/classes/*")
        grant_fs_read("/proc/*")
        grant_fs_read("/sys/*")
    }
    args = list.new()
    list.add(args, "--enable-native-access=ALL-UNNAMED")
    list.add(args, "-cp")
    list.add(args, "build/java/classes")
    list.add(args, "ghl.worker.Worker")
    g, err = guest.launch(perms, "${jh}/bin/java", args, "java", "")
    if err != "" { println("not started: ${err}") return }     // macOS: refused
    guest.mount(&g, "greet", 1, 1)
    guest.start(&g, "say hello\ncall greet [\"Java\"]")
    while true {
        e = guest.next(&g)
        if e.kind == "SAY" {
            println(guest.printable(e.text))
        } else if e.kind == "CALL" {
            guest.answer(&g, guest.json_string("hello, Java"))
        } else {
            println("${e.kind} ${e.code} ${guest.printable(e.text)}")
            break
        }
    }
}
```

([examples/contained.ae](examples/contained.ae) is the host.) On macOS this
prints `not started: ... cannot contain a child on macOS`. On Linux the worker
dies at startup today (above) and the host sees `KILLED ... Connection reset`
or `ERROR`.

The worker is a **JVM program that speaks `std.guest`'s protocol itself**, as
`go/worker` does in Go. That is the natural shape here: the other shape, an
Aether worker that starts the JVM inside its own process before it locks down,
needs `JNI_CreateJavaVM` (a `libjvm` in the Aether process), and the bridge
does not embed. Package [`ghl.guestchild`](guestchild/GuestChild.java) is the
protocol, the counterpart of `std.guest.child` for a worker in Java: `std.ipc`
frames of `VERB text` on the socket named by `AETHER_IPC_FD`, with `readScript`
(MOUNT, PROBE, LOGLEVEL, SCRIPT), `say`, `audit`, `log`, `callHost`, `poll` (a
CANCEL), `done` and `error`. Java has no way to wrap an inherited descriptor in a
stream or a channel, so the reads, the writes and the receipt of a granted
file's descriptor (`recvmsg`, `SCM_RIGHTS`, Linux's layout) are libc's through
`aether.sandbox.Native`. [`worker/Worker.java`](worker/Worker.java) is a
complete worker for the tests, with the catalogue's rows compiled in: its
script is a command list (`say`, `call`, `fact`, `row`, and the four
host-checked rows), as `go-worker`'s.

As for Go, **the worker's own lockdown is missing**: `std.guest.child.run`
enters `sandbox.enforce` with no grants before the script's first step, and a
JVM cannot enter an Aether sandbox block. The grants on the spawn are the
worker's own authority, so **launch it with the smallest list that lets it
work** and let it ask the host for the rest (`callHost`, a host's `grant_file`).
A JVM's smallest list is not small: the JDK's directory (the modules image, the
class data archive, the libraries), `/proc` and `/sys` (container and CPU
probes), the classes, and the exec of `jspawnhelper` if the worker starts a
process. Those reads are in `containment/child_host.ae`'s `add_jvm`. The
JVM's threads (the garbage collector's, the compiler's, the signal and VM
threads) are `clone` calls that seccomp's process-creation fence lets through
as threads: the tests start the worker with `-XX:+UseSerialGC
-XX:TieredStopAtLevel=1` to keep them few.

The host can cancel at a deadline and kills a worker that stops answering
(`spin`, `silent`, `oversize` and `half-frame` are the rows that hold it);
the cancel reaches a Java worker only if it calls `poll`, or is waiting in
`callHost`.

## Stunt parity

Each of stunt's tests, as Java comes out of it in each way that runs the
checks: "matches" when every row of the test came out as stunt expects, else
the rows that did not and how. Column 2 is macOS, where the bridge's
sandboxed run starts nothing and the rows run through `java.run`, to show what
is not held. Columns 3 and 4 are Linux, where the JVM does not start inside
the preload or the contained child: each cell is the one **blocked** line, and
the day that changes the rows come back.

<!-- stunt-parity:begin: generated by sh tests/run.sh --write-parity, from build/parity-*.tsv -->
| stunt test | policy | 2. Sandbox block, no preload | 3. Under `LD_PRELOAD` | 4. Contained child |
|---|---|---|---|---|
| `test_stunt_exec` | `exec=/bin/sh,fork` | differs: open-etc-passwd not held | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_file_granted` | `fs_read=$WORK/stunt-fixture.txt,env=HOME` | differs: binding-read-granted-file left out, open-etc-passwd not held, binding-read-etc-passwd left out, write-granted-file not held, binding-getenv-home left out | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_fork` | `fork` | differs: exec-bin-sh not held, open-etc-passwd not held | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_host_checked` | `-` | differs: spin left out | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_native` | `native=*` | differs: open-etc-passwd not held | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_nested` | `fs_read=$WORK/*\|fs_read=/etc/passwd` then `fs_read=$WORK/*,fs_read=/etc/passwd\|fs_read=$WORK/*` | differs: open-etc-passwd not held, binding-read-etc-passwd left out, open-granted-file not held, binding-read-granted-file left out | differs: (the runtime does not start) blocked | (no row of it runs this way) |
| `test_stunt_net_allowed` | `tcp=192.0.2.1:443` | differs: open-etc-passwd not held | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_net_other` | `tcp=198.51.100.1:443` | differs: connect-192.0.2.1 not held, connect-ipv4-mapped not held | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_no_grants` | `-` | differs: open-etc-passwd not held, open-granted-file not held, write-granted-file not held, getenv-home not held, env-cached not held, connect-192.0.2.1 not held, connect-ipv4-mapped not held, exec-bin-sh not held, binding-read-etc-passwd left out, binding-read-granted-file left out, binding-getenv-home left out, callback-greet left out, callback-add left out, callback-unregistered left out, map-get left out, map-set left out, map-overwrite-input left out | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |
| `test_stunt_write_granted` | `fs_write=$WORK/*` | differs: open-granted-file not held, binding-read-granted-file left out | differs: (the runtime does not start) blocked | differs: (the runtime does not start) blocked |

- Column 2, ae from 0.884.0: macOS 27.2 arm64, ae 0.889.0, Java 21.0.13.0.3
- Column 3, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Java 25.0.5.0.3
- Column 3, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Java 25.0.5.0.3
- Column 4, ae from 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.889.0, Java 25.0.5.0.3
- Column 4, ae before 0.884.0: CachyOS, kernel 7.3.1 x86_64, ae 0.878.0, Java 25.0.5.0.3
<!-- stunt-parity:end -->

The test names are stunt's, aether's `tests/integration/contrib_host_stunt`
at `AETHER_STUNT_REF` (`../ci/versions.env`), mirrored in
`../tests/test_stunt_*.sh` with the same policies. The table is generated
from a run (`sh tests/run.sh --write-parity`) and checked against every run
(`tests/parity.sh`), in the columns the host can run; the lines under it say
which host produced which column. Java reaches the system the same way before
and after ae 0.884.0 (`AE_SPAWN_PIN`), so no cell has two sides.

## Containment table

What a Java program gets when it tries each row of stunt's catalogue (aether
`contrib/host/stunt`, at the ref in `../ci/versions.env`), done the Java way
([rows](rows/Rows.java), run by [containment/Checks.java](containment/Checks.java)
for ways 1 to 3 and by [worker/Worker.java](worker/Worker.java) for way 4),
under the policies of aether's `tests/integration/contrib_host_stunt`
(`../tests/test_stunt_*.sh`). The first column is the call, the second what
libc would see of it on Linux (a test outcome of
[containment/test_ways.sh](containment/test_ways.sh), with
[libghltrace.c](containment/libghltrace.c)), the third the result with **no
wall** (`java.run` on macOS; the same rows on CachyOS with the worker started
by `guest.launch_uncontained`), and the fourth what the walls do today. Every
cell is a test outcome (`../tests/run.sh`): checked on macOS arm64 (JDK 21,
ae 0.889.0) and CachyOS x86_64 (OpenJDK 25, kernel with Landlock; ae 0.889.0
and 0.878.0), and in CI (OpenJDK 21).

| Row | Java's call, and what libc sees | No wall | Preload and contained child, Linux |
|---|---|---|---|
| `open-etc-passwd`, `open-granted-file`, `write-granted-file` | `Files.readAllBytes`, `Files.write`: libc `open` (the JDK's `open64`) | succeed (**not held** on macOS) | the JVM does not start |
| `getenv-home` | libc `getenv` through `Native` (looked up with `dlsym(RTLD_DEFAULT)`) | succeeds | the JVM does not start. With `Linker.defaultLookup()` the call is **not seen** by an interposer |
| `env-cached` | `System.getenv`: **not libc**; a copy taken at start | succeeds | the JVM does not start; would **escape** the preload, and the contained child has no environment to copy |
| `connect-192.0.2.1` | `SocketChannel.open(INET)`, non-blocking: libc `connect`, `AF_INET` | let through | the JVM does not start |
| `connect-ipv4-mapped` | `SocketChannel.open(INET6)`: libc `connect`, `AF_INET6`, `::ffff:192.0.2.1` | let through | the JVM does not start |
| `exec-bin-sh` | `ProcessBuilder`: libc `posix_spawn` of `jspawnhelper`, then its `execve` of `/bin/sh` | succeeds | the JVM does not start |
| `fork` | `Native` `fork` then `_exit` in the child (child-only row) | succeeds | the JVM does not start |
| `dlopen-libm`, `dlopen-libc-by-name`, `dlopen-libc-by-path` | `System.load` for a file that is there, `dlopen` through `Native` for a name: libc `dlopen` | load | the JVM does not start |
| `syscall-fn-openat` | libc `syscall()` through `Native` (variadic) | succeeds | the JVM does not start |
| `syscall-openat`, `syscall-socket` | the instruction itself, from a page of machine code (`Native.rawSyscall`): no libc | succeed (no wall) | the JVM does not start |
| `binding-read-etc-passwd`, `binding-read-granted-file`, `binding-getenv-home`, `callback-greet`, `callback-add`, `callback-unregistered` (host functions) | `GuestChild.callHost` | **left out** in a plain child (it has none); work in the worker: refused or answered as the host decides, a granted file arriving as a descriptor | the JVM does not start |
| `map-get`, `map-set`, `map-overwrite-input` | `AetherMap` over `AETHER_MAP_SHM` (plain child); `callHost` (worker) | work on Linux; **left out** on macOS (`java.run_sandboxed_with_map` starts nothing and a macOS shared memory object cannot grow to take an output) | the JVM does not start |
| `spin` | the worker polls for a CANCEL | **left out** in a plain child (the bridge waits for the child); cancelled at the deadline in the worker | the JVM does not start |
| `silent`, `oversize`, `half-frame` | the worker never looks, says 1 MiB, writes half a frame | (worker rows) killed by the host | the JVM does not start |

What the table says:

- **Nothing about a JVM's containment is shown by the walls today**, because no
  JVM runs inside them: "The wall that stops a JVM". What is shown is the
  rest of the picture: with no wall, every row does what it should; and the
  libc column says which rows the preload could hold if a JVM could run under
  it, which is more than for Go (files, sockets, processes and libraries are
  libc's for the JDK) and not all (`System.getenv`, a default-lookup FFM call,
  the system call instruction).
- **The contained child, when it can start a JVM, will not narrow the grants.**
  A JVM cannot enter an Aether sandbox block, so the spawn's grants are the
  worker's own authority (the worker section above), and they include the
  whole JDK directory.
- **The nested test** (`test_stunt_nested`) is only partly the other languages'
  test, as for Go: the bridge hands the child the inner grants and ignores the
  enclosing level, except that a block with no `exec` grant stops the child
  starting at all.

## Gaps

What this move did not do, and what stays open:

- **The preload and a JVM** (above). Upstream's: a way for a runtime that
  generates code to keep executable memory under the preload, with the
  kernel layer unchanged. Until then, ways 2 to 4 hold no JVM on Linux and the
  tables say so.
- **The worker on macOS.** `guest.launch` refuses there, so nothing of way 4
  runs; and `GuestChild.takeFile` (the descriptor of a granted file) uses
  Linux's `struct msghdr`, which differs on macOS.
- **Nested grants.** `java.run_sandboxed` inside `sandbox.enforce` gives the
  child the inner list and ignores the outer, as `go.run_sandboxed` does.
- **Arguments and output.** The bridge passes the JVM a class and no program
  arguments, and does not capture its output; both go through the
  environment, the shared map and the inherited standard streams.
- **A map output on macOS.** As for Go.
- **Windows.** The bridge's stubs return -1 with a message; untested here.
- **JDK 17 and earlier** are not supported (no Foreign Function API).

## Finding the JDK

The bridge runs `$JAVA_BIN`, else `java` on `PATH`. The tests find a JDK in
[`../tests/java_env.sh`](../tests/java_env.sh): `JAVA_HOME` if it is a JDK of 21
or later, then `/usr/libexec/java_home -v 21+`, Homebrew's `openjdk@21` and
`openjdk`, `/usr/lib/jvm/*`, and `javac` on the `PATH`; they follow symlinks to
the JDK proper (Homebrew's prefix keeps `lib/` elsewhere) and skip cleanly
without one.

```sh
GUEST_LANG=java sh tests/run.sh                  # specs, rows, parity
JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 GUEST_LANG=java sh tests/run.sh
sh java/build.sh [outdir]                        # the agent jar and the classes, by hand
```

## Implementation notes

- The bridge is `aether_host_java.c`: `aether_host_go.c` with the JVM's command
  line built from `JAVA_BIN`, `AETHER_JAVA_FLAGS` and `AETHER_JAVA_AGENT`
  ([PROVENANCE.md](PROVENANCE.md)). Everything the child execs with is made
  before the fork and the child calls only `execve`.
- `src/Native.java` is the libc the Java side uses: one call that returns the
  result and the `errno` (`captureCallState`), `shmOpen` (a variadic mode on
  macOS), `readShm` (macOS refuses `read` on shared memory: it is `fstat` and
  `mmap` there), and `rawSyscall`.
- `rows/Rows.java` is the catalogue and the judge, shared by `Checks`
  (plain child) and `Worker` (contained). The errors that reach it are
  exceptions carrying libc's message, so a refusal is "Permission denied" or
  "Operation not permitted" where the Go and C rows read `EACCES` and
  `EPERM`.
- No JNI and no `jni.h`: nothing in the bridge's C touches the JVM's ABI, so
  there is no `java_abi.h` to keep.

## Tests

- [`test_host_java.ae`](test_host_java.ae): the bridge's calls: exit statuses,
  a class that is not there, a JVM that cannot start (-1), `run_sandboxed` with
  grants, with `grant_jvm_runtime()` and with an empty list on Linux (started,
  and dead) and -1 on macOS.
- `../tests/test_stunt_*.sh`, `../tests/test_catalogue.sh` and
  `containment/test_ways.sh`: the containment table and the parity table above,
  the agent (`GrantCheckerTest`: the one matcher; `AgentTest`: the grant list
  and the map through real shared memory), the worker without a wall, what libc
  sees, and the programs in this README.

`../tests/run.sh` runs everything.

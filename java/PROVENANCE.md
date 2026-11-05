# Provenance

The Java bridge was copied from aether's `contrib/host/java/` at commit
`2609ec841` (aether-lang-dev/aether, `main`, 2026-11-05; v0.889.0's merge;
the directory's last change is `d11dd433f`), with what aether has of its
tests: `SandboxTest.java` (a program that tries to escape, kept), the shared
map case of `tests/sandbox/test_shared_map_all.sh` (a C parent that freezes a
map into shm and runs the agent against it, not copied: it needs aether's
tree) and the namespace and trading examples (`tests/integration/
namespace_java`, `embedded_java_trading_e2e`, which test aether's
`ae build --namespace` SDK generator, not this bridge). aether is MIT
licensed, copyright "Aether Programming Language Contributors"; so is this
repository (`../LICENSE`).

| Here | From aether | Changed |
|---|---|---|
| `module.ae` | `contrib/host/java/module.ae` | `grant_jvm_runtime` unchanged; the three `run` calls and their `@source` are new; header |
| `src/AetherGrantChecker.java` | `.../src/AetherGrantChecker.java` | see below |
| `src/AetherMap.java` | `.../src/AetherMap.java` | the Foreign Function calls go through `Native` |
| `src/AetherSandboxHooks.java` | `.../src/AetherSandboxHooks.java` | `checkConnect(host, port)` added |
| `src/AetherSandboxAgent.java`, `src/SandboxTest.java` | the same names | unchanged |
| `MANIFEST.MF` | `.../MANIFEST.MF` | unchanged |
| `build.sh` | `.../build.sh` | rewritten: finds the JDK, builds every Java file here, JDK 21 or later |
| `src/Native.java` | (new) | the libc the Java side calls, through the Foreign Function API |
| `aether_host_java.c`, `aether_host_java.h` | (new) | `../go/aether_host_go.c` with `java` for `go` |
| `rows/`, `guestchild/`, `worker/`, `containment/`, `examples/` | (new) | Java, and Aether hosts for it, modelled on `../go/` |
| `../tests/java_env.sh` | (new) | finds a JDK |

Not brought over: `README.md` (rewritten for this repository; its "Testing"
section points at aether's integration tests, and its "Prerequisites" asked
for JDK 22 and Amazon Corretto 24).

## What the move changed

- **It builds on JDK 21.** aether's `AetherGrantChecker` and `AetherMap`
  used `Arena.allocateFrom(String)` and `MemorySegment.getString`, which exist
  from JDK 22; and the README said JDK 22 or later. All their Foreign Function
  calls now go through `src/Native.java`, which uses only what JDK 21's
  preview API and 22's final one share, so one source builds on both
  (`build.sh` adds `--enable-preview --release 21` on 21). Two differences it
  works around: a layout's `VarHandle` takes one more coordinate from JDK 22
  (the `errno` capture reads at the layout's byte offset instead), and nothing
  here calls the 22-only string methods.
- **`shm_open` is called with its mode as a variadic argument.** On macOS the
  mode is a variadic argument and Apple's arm64 ABI puts those on the stack; a
  call that does not say so creates the object with a garbage mode. On Linux it
  changes nothing. (`Native.shmOpen`.)
- **A shared memory object is read the way the platform allows.** aether's code
  `read`s it, which works on Linux and fails on macOS (ENODEV); on macOS it is
  now `fstat` and `mmap` (`Native.readShm`). Nothing in aether ran the agent on
  macOS, where there is no preload, so this was not found there.
- **The grant matcher is the one every other place uses.** The agent's
  `patternMatch` normalised IPv4-mapped IPv6 addresses (the TODO's "applied in
  `AetherGrantChecker.patternMatch`") but not the rest of
  `aether_sandbox_match.h`, which since then compares a tcp or udp pattern with a
  port: `192.0.2.1:443`, `[::1]:8080`, the resource being `host:port`. A Java
  program that checked `checkConnect(host)` against a grant that names a port
  would be refused, or one that checked a mapped address against a bracketed
  pattern would not match. `AetherGrantChecker` now follows the header's rules
  (`checkNet`, `AetherSandboxHooks.checkConnect(host, port)`;
  `checkConnect(host)` is kept and means "any port"), and
  `containment/GrantCheckerTest.java` holds it to them, IPv4-mapped cases
  included.
- **The bridge starts the JVM.** aether's module carried no C: the caller
  started the JVM with `spawn_sandboxed` and passed the agent. `java.run`,
  `java.run_sandboxed` and `java.run_sandboxed_with_map` do it, with
  `aether_host_java.c`. It is `aether_host_go.c` (itself aether's, see
  `../go/PROVENANCE.md`) with these changes: the command line is built from
  `JAVA_BIN`, `AETHER_JAVA_FLAGS` (split on spaces) and, for sandboxed runs,
  `AETHER_JAVA_AGENT`, then `-cp <classpath> <main class>`; the segment is
  `/aether_host_java_<pid>`; messages say `[java-host]`.

## Found, not changed

- **A JVM does not start under the preload.** `libaether_sandbox.so` refuses
  `mprotect` with `PROT_EXEC` and anonymous executable `mmap` while a sandbox is
  active, and HotSpot needs both at startup (`README.md`, "The wall that stops a
  JVM", with the three-line repro). Not changed: it is the preload's rule, in
  aether. It makes `java.run_sandboxed`, and the contained child, unusable for a
  JVM today. aether's README for the bridge, whose example spawns a `java` under
  `spawn_sandboxed`, described a combination that does not run today: the same
  refusal, with the preload active.
- **The agent has never held anything.** `AetherSandboxHooks.install` scrubs
  nothing ("Nothing to scrub in Java"); its checks are calls a program makes of
  itself. That is kept and documented.
- **`SandboxTest.java`** is aether's demonstration program and goes to
  `example.com` and `httpbin.org` over the network; it is compiled with the
  rest and not run by the tests.

aether keeps its copy for a transition period; see the top-level README.

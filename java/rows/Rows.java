// Rows is stunt's catalogue (aether contrib/host/stunt), each row done the
// way a Java program would do it, and the judge that turns what happened into
// stunt's one line:
//
//   <check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
//
// The checks program (containment/Checks.java, ways 1 to 3: a plain child)
// and the worker (worker/Worker.java, way 4: a contained child) both run it,
// so a row is the same Java code whichever wall is around it. A refusal is
// what a sandbox gives: EPERM or EACCES from libc, the preload library or the
// kernel, or a host function's failure that starts "refused:". Anything else
// that goes wrong is an error, never a refusal, so it cannot pass for a
// sandbox at work.
//
// Java reaches the system through the JDK, and the JDK's native code calls
// libc (open64, socket, connect, posix_spawn, dlopen), so the preload sees
// what a Java program does with files, sockets and processes. What it does
// not see: System.getenv (the environment, copied once at the first use),
// and anything the Foreign Function API does by the instruction itself.
// Errors come back as exceptions that carry libc's message (strerror), not
// the number, so a row reads "Permission denied" and "Operation not
// permitted" where the Go and C rows read EACCES and EPERM.
package ghl.rows;

import aether.sandbox.AetherMap;
import aether.sandbox.Native;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.net.Inet6Address;
import java.net.InetSocketAddress;
import java.net.StandardProtocolFamily;
import java.nio.channels.SocketChannel;
import java.nio.file.AccessDeniedException;
import java.nio.file.FileSystemException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

public final class Rows {
    private Rows() {}

    /** What the host knew before the sandbox was in force. */
    public record Facts(boolean homeSet, boolean sh, String libm, String libcName, String libcPath) {}

    /** Calls a function the host mounted (std.guest's CALL): kind VALUE (text is the JSON value), FAIL (the message) or CANCEL. */
    public interface Caller { String[] call(String name, String argsJson); }

    /** What a row runs with: the facts, and the host's functions (null for a plain child, which has none). */
    public record Env(Facts facts, Caller call) {}

    private static final String FIXTURE = "./stunt-fixture.txt";
    private static final String WRITTEN = "./stunt-written.txt";

    private record Result(String kind, String detail) {}

    private static Result succeeded(String d) { return new Result("succeeded", d); }
    private static Result refused(String d) { return new Result("refused", d); }
    private static Result failed(String d) { return new Result("error", d); }

    private static boolean deniedText(String m) {
        return m != null && (m.contains("Permission denied") || m.contains("Operation not permitted"));
    }

    private static Result byErr(Throwable e, String what) {
        if (e instanceof AccessDeniedException) return refused(what);
        return deniedText(e.getMessage()) ? refused(what) : failed(what);
    }

    private static Result byErrno(int errno, String what) {
        return errno == Native.EPERM || errno == Native.EACCES ? refused(what) : failed(what);
    }

    private static String firstLine(String s) {
        int i = s.indexOf('\n');
        if (i >= 0) s = s.substring(0, i);
        return s.length() > 40 ? s.substring(0, 40) : s;
    }

    private static String quote(String s) { return Json.string(s); }

    // Files.readAllBytes: Java's way to a file. The JDK's native code opens it
    // with libc's open64.
    private static Result openPath(String path) {
        try {
            byte[] b = Files.readAllBytes(Path.of(path));
            return succeeded("read " + quote(firstLine(new String(b, java.nio.charset.StandardCharsets.UTF_8))));
        } catch (IOException e) {
            return byErr(e, "open: " + e);
        }
    }

    private static Result writeFile() {
        try {
            Files.write(Path.of(WRITTEN), "written by java\n".getBytes());
            return succeeded("wrote " + WRITTEN);
        } catch (IOException e) {
            return byErr(e, "open: " + e);
        }
    }

    // getenv(3), the libc call, through the Foreign Function API: what stunt's
    // getenv-home row is.
    private static Result libcGetenv(Facts f) {
        try (Arena arena = Arena.ofConfined()) {
            long v = Native.call("getenv", "p", "p", Native.cstr(arena, "HOME")).value();
            if (v != 0) return succeeded("HOME=" + Native.stringAt(v, 4096));
        }
        if (f.homeSet()) return refused("libc getenv gave NULL; HOME is in the environment");
        return failed("HOME is unset");
    }

    // System.getenv: the JDK copies the environment out of libc's environ the
    // first time it is asked, and never calls getenv again. stunt's env-cached
    // row. A missing HOME is a refusal only when the host had one: the
    // contained child is given no environment.
    private static Result envCached(Facts f) {
        String v = System.getenv("HOME");
        if (v != null && !v.isEmpty()) return succeeded("HOME=" + v + " from System.getenv");
        if (f.homeSet()) return refused("HOME is not in the environment; the host has it");
        return failed("HOME was never in the environment");
    }

    // A non-blocking connect, judged as stunt judges it: "Permission denied"
    // at connect (EACCES: the preload, Landlock) or either message at socket
    // (seccomp) is a refusal; "Operation not permitted" at connect is a
    // firewall's answer, an error; anything else (in progress, a network
    // error) means the call was let through.
    private static Result connectTo(StandardProtocolFamily family, java.net.InetAddress addr) {
        SocketChannel ch;
        try {
            ch = SocketChannel.open(family);
        } catch (IOException e) {
            return byErr(e, "socket: " + e);
        }
        try (ch) {
            ch.configureBlocking(false);
            try {
                if (ch.connect(new InetSocketAddress(addr, 443))) return succeeded("connected");
                return succeeded("let through: connect in progress");
            } catch (IOException e) {
                String m = e.getMessage();
                if (m != null && m.contains("Permission denied")) return refused("connect: " + e);
                if (m != null && m.contains("Operation not permitted")) {
                    return failed("connect: " + e + " (a firewall's answer, not the sandbox's)");
                }
                return succeeded("let through: " + e);
            }
        } catch (IOException e) {
            return failed("close: " + e);
        }
    }

    private static Result connectV4() {
        try {
            return connectTo(StandardProtocolFamily.INET, java.net.InetAddress.getByAddress(new byte[] { (byte) 192, 0, 2, 1 }));
        } catch (IOException e) {
            return failed("address: " + e);
        }
    }

    // An AF_INET6 socket and ::ffff:192.0.2.1.
    private static Result connectMapped() {
        byte[] a = new byte[16];
        a[10] = (byte) 0xff; a[11] = (byte) 0xff;
        a[12] = (byte) 192; a[13] = 0; a[14] = 2; a[15] = 1;
        try {
            return connectTo(StandardProtocolFamily.INET6, Inet6Address.getByAddress(null, a, -1));
        } catch (IOException e) {
            return failed("address: " + e);
        }
    }

    // ProcessBuilder: the JDK starts a process through jspawnhelper (a
    // posix_spawn of it, then its exec of the program), all libc.
    private static Result execSh(Facts f) {
        Process p;
        try {
            p = new ProcessBuilder("/bin/sh", "-c", "exit 0").redirectErrorStream(true).start();
        } catch (IOException e) {
            return byErr(e, "ProcessBuilder: " + e.getMessage());
        }
        try {
            if (!p.waitFor(10, TimeUnit.SECONDS)) {
                p.destroyForcibly();
                return failed("ProcessBuilder: /bin/sh did not exit");
            }
        } catch (InterruptedException e) {
            return failed("interrupted");
        }
        int rc = p.exitValue();
        if (rc == 0) return succeeded("ProcessBuilder: exit 0");
        if (rc == 127 && f.sh()) return refused("ProcessBuilder: the shell could not be executed (exit 127)");
        return failed("ProcessBuilder: exit " + rc);
    }

    // libc's fork through the Foreign Function API, the child exiting at once.
    // (A JVM does not fork: the child has this thread only. It calls _exit
    // as its first act, and the parent gives it five seconds.)
    private static Result forkOnce() {
        Native.prepare("_exit", "v", "i");
        Native.call("srandom", "v", "i", 0);   // runs a void(int) call once, so the child's call spins no new code
        Native.Result r = Native.call("fork", "i", "");
        if (r.value() == 0) {
            Native.call("_exit", "v", "i", 0);
            return failed("_exit returned");
        }
        if (r.value() < 0) return byErrno(r.errno(), "fork: errno " + r.errno());
        int pid = (int) r.value();
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment st = arena.allocate(4);
            for (int i = 0; i < 500; i++) {
                Native.Result w = Native.call("waitpid", "i", "ipi", pid, st, 1 /* WNOHANG */);
                if (w.value() == pid) {
                    int status = st.get(java.lang.foreign.ValueLayout.JAVA_INT, 0);
                    if ((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0) return succeeded("the forked child ran");
                    return failed("the forked child: status " + status);
                }
                if (w.value() < 0 && w.errno() != Native.EINTR) return failed("waitpid: errno " + w.errno());
                try { Thread.sleep(10); } catch (InterruptedException e) { break; }
            }
            Native.call("kill", "i", "ii", pid, 9);
            return failed("the forked child did not exit");
        }
    }

    // dlopen. System.load is Java's way to dlopen a file by its path (the JVM
    // calls dlopen, and says dlerror's message); a name, or a path with no
    // file (macOS's libraries live in the dyld cache), goes to dlopen itself.
    // A dlopen that fails on its own leaves a message; the preload's refusal
    // returns NULL with none, or "Permission denied".
    private static Result load(String path) {
        if (path == null || path.isEmpty()) return failed("the host found no library to load");
        if (path.startsWith("/") && Files.exists(Path.of(path))) {
            try {
                System.load(path);
                return succeeded("loaded " + path + " (System.load)");
            } catch (UnsatisfiedLinkError e) {
                String m = e.getMessage() == null ? "" : e.getMessage();
                int c = m.lastIndexOf(": ");
                String msg = c >= 0 ? m.substring(c + 2) : m;
                if (m.endsWith(":") || m.endsWith(": ") || deniedText(m)) {
                    return refused("System.load: " + (msg.isEmpty() ? "no dlerror() message" : msg) + " (the preload's refusal)");
                }
                return failed("System.load: " + m);
            }
        }
        try (Arena arena = Arena.ofConfined()) {
            long h = Native.call("dlopen", "p", "pi", Native.cstr(arena, path), 2 /* RTLD_NOW */).value();
            if (h != 0) {
                Native.call("dlclose", "i", "p", MemorySegment.ofAddress(h));
                return succeeded("loaded " + path + " (dlopen)");
            }
            String m = Native.stringAt(Native.call("dlerror", "p", "").value(), 1024);
            if (m == null || m.isEmpty() || deniedText(m)) {
                return refused("dlopen: " + (m == null ? "no dlerror() message" : m) + " (the preload's refusal)");
            }
            return failed("dlopen: " + m);
        }
    }

    // libc's syscall(), through the Foreign Function API: what stunt calls
    // syscall-fn. Only EPERM or EACCES is a refusal (ENOSYS is the preload's
    // syscall() with no grants in force, which is an error).
    private static Result syscallFnOpenat() {
        try (Arena arena = Arena.ofConfined()) {
            // syscall(long nr, ...): the variadic arguments start at 1.
            Native.Result r = Native.callVariadic("syscall", "l", "lipi", 1,
                Native.sysOpenat(), -100 /* AT_FDCWD */, Native.cstr(arena, "/etc/passwd"), 0 /* O_RDONLY */);
            if (r.value() >= 0) {
                Native.close((int) r.value());
                return succeeded("opened /etc/passwd");
            }
            return byErrno(r.errno(), "syscall(): errno " + r.errno());
        }
    }

    // The instruction itself (Native.rawSyscall), past libc and any preload:
    // only the kernel (Landlock, seccomp) is left to hold what it does.
    private static Result raw(long r, String what) {
        if (r >= 0) {
            Native.close((int) r);
            return succeeded(what);
        }
        return byErrno((int) -r, "the syscall instruction: errno " + (-r));
    }

    private static Result syscallOpenat() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment p = Native.cstr(arena, "/etc/passwd");
            return raw(Native.rawSyscall(Native.sysOpenat(), -100, p.address(), 0),
                       "the syscall instruction opened /etc/passwd");
        } catch (IllegalStateException e) {
            return failed("no system call stub: " + e.getMessage());
        }
    }

    private static Result syscallSocket() {
        try {
            return raw(Native.rawSyscall(Native.sysSocket(), 2 /* AF_INET */, 1 /* SOCK_STREAM */, 0),
                       "the syscall instruction made an AF_INET socket");
        } catch (IllegalStateException e) {
            return failed("no system call stub: " + e.getMessage());
        }
    }

    // A host function's answer: in a contained child a file comes as std.guest's
    // answer to the call, {"granted": true, "text": ...} or {"granted": false,
    // "why": ...}, and a FAIL is a failure. Without a host (a plain child) there
    // is no function to call.
    private static Result hostStr(Env e, String fn, String arg) {
        if (e.call() == null) return failed("no host function " + fn + " (a plain child has none)");
        String[] a = e.call().call(fn, Json.array(arg));
        String kind = a[0], text = a[1];
        if (kind.equals("FAIL")) return text.startsWith("refused:") ? refused(text) : failed(text);
        if (kind.equals("CANCEL")) return failed("cancelled by host: " + text);
        Object v;
        try {
            v = Json.parse(text);
        } catch (IllegalArgumentException x) {
            return failed(fn + " answered " + text);
        }
        if (v instanceof Map<?, ?> m && m.get("granted") instanceof Boolean g) {
            if (!g) return refused("refused: " + m.get("why"));
            return succeeded(fn + "(" + arg + ") = " + quote(firstLine(String.valueOf(m.get("text")))));
        }
        if (!(v instanceof String s)) return failed(fn + " answered " + text);
        return succeeded(fn + "(" + arg + ") = " + quote(firstLine(s)));
    }

    private static Result callbackGreet(Env e) {
        if (e.call() == null) return failed("no host function greet (a plain child has none)");
        String[] a = e.call().call("greet", "[\"stunt\"]");
        try {
            if (a[0].equals("VALUE") && "hello, stunt".equals(Json.parse(a[1]))) {
                return succeeded("greet(\"stunt\") = \"hello, stunt\"");
            }
        } catch (IllegalArgumentException x) { /* fall through */ }
        return failed("greet(\"stunt\") = " + a[0] + " " + a[1]);
    }

    private static Result callbackAdd(Env e) {
        if (e.call() == null) return failed("no host function add (a plain child has none)");
        String[] a = e.call().call("add", "[2,40]");
        if (!a[0].equals("VALUE") || !a[1].trim().equals("42")) return failed("add(2, 40) = " + a[0] + " " + a[1]);
        return succeeded("add(2, 40) = 42");
    }

    // A host function nobody registered: asked by name, the host's answer is
    // an error, not a crash.
    private static Result callbackUnregistered(Env e) {
        if (e.call() == null) return failed("no host function nope (a plain child has none)");
        String[] a = e.call().call("nope", "[]");
        if (a[0].equals("FAIL") || a[0].equals("CANCEL")) return refused("no host function nope (" + a[1] + ")");
        return succeeded("nope() answered " + a[1]);
    }

    // ---- the shared map ------------------------------------------------------
    //
    // In a plain child it is AETHER_MAP_SHM, read and written with AetherMap
    // (the agent's own class: frozen inputs, outputs written back when the
    // child exits). In a contained child it is the host's, over CallHost.

    private static AetherMap shm() {
        String name = System.getenv("AETHER_MAP_SHM");
        if (name == null) throw new IllegalStateException("AETHER_MAP_SHM is not set");
        return AetherMap.fromSharedMemory(name);
    }

    private static String mapGetValue(Env e, String key) {
        if (e.call() != null) {
            String[] a = e.call().call("aether_map_get", Json.array(key));
            if (!a[0].equals("VALUE")) throw new IllegalStateException(a[0] + ": " + a[1]);
            Object v = Json.parse(a[1]);
            return v == null ? null : (String) v;
        }
        return shm().get(key);
    }

    private static void mapPutValue(Env e, String key, String value) {
        if (e.call() != null) {
            String[] a = e.call().call("aether_map_put", Json.array(key, value));
            if (!a[0].equals("VALUE")) throw new IllegalStateException(a[0] + ": " + a[1]);
            return;
        }
        AetherMap m = shm();
        m.put(key, value);
        m.writeBack();
    }

    private static Result mapGet(Env e) {
        try {
            String v = mapGetValue(e, "input");
            if (v == null) return failed("input is not in the map");
            if (!v.equals("stunt-in")) return failed("input = " + quote(v));
            return succeeded("input = \"stunt-in\"");
        } catch (RuntimeException x) {
            return failed("input: " + x.getMessage());
        }
    }

    private static Result mapSet(Env e) {
        try {
            mapPutValue(e, "output", "from-java");
            String v = mapGetValue(e, "output");
            if (!"from-java".equals(v)) return failed("output read back as " + (v == null ? "nothing" : quote(v)));
            return succeeded("output = \"from-java\"");
        } catch (RuntimeException x) {
            return failed("output: " + x.getMessage());
        }
    }

    // The write is the host's to refuse: in a contained child its answer to
    // aether_map_put is false; in a plain child AetherMap.put ignores a frozen
    // input, and the host checks its own map afterwards (containment/host.ae).
    private static Result mapOverwriteInput(Env e) {
        try {
            mapPutValue(e, "input", "overwritten");
            String v = mapGetValue(e, "input");
            if ("stunt-in".equals(v)) return refused("input is still \"stunt-in\" (read back)");
            return succeeded("input read back as " + (v == null ? "nothing" : quote(v)));
        } catch (RuntimeException x) {
            return failed("input: " + x.getMessage());
        }
    }

    /** Runs the row {@code name} and returns stunt's line for it, judged against {@code expect} (succeed, refused or error). */
    public static String line(String name, String expect, Env e) {
        Result r;
        try {
            r = switch (name) {
                case "open-etc-passwd" -> openPath("/etc/passwd");
                case "open-granted-file" -> openPath(FIXTURE);
                case "write-granted-file" -> writeFile();
                case "getenv-home" -> libcGetenv(e.facts());
                case "env-cached" -> envCached(e.facts());
                case "connect-192.0.2.1" -> connectV4();
                case "connect-ipv4-mapped" -> connectMapped();
                case "exec-bin-sh" -> execSh(e.facts());
                case "fork" -> forkOnce();
                case "dlopen-libm" -> load(e.facts().libm());
                case "dlopen-libc-by-name" -> load(e.facts().libcName());
                case "dlopen-libc-by-path" -> load(e.facts().libcPath());
                case "syscall-fn-openat" -> syscallFnOpenat();
                case "syscall-openat" -> syscallOpenat();
                case "syscall-socket" -> syscallSocket();
                case "binding-read-etc-passwd" -> hostStr(e, "read_file", "/etc/passwd");
                case "binding-read-granted-file" -> hostStr(e, "read_file", FIXTURE);
                case "binding-getenv-home" -> hostStr(e, "getenv", "HOME");
                case "callback-greet" -> callbackGreet(e);
                case "callback-add" -> callbackAdd(e);
                case "callback-unregistered" -> callbackUnregistered(e);
                case "map-get" -> mapGet(e);
                case "map-set" -> mapSet(e);
                case "map-overwrite-input" -> mapOverwriteInput(e);
                default -> null;
            };
        } catch (Throwable t) {
            r = failed("unexpected " + t);
        }
        if (r == null) return name + ": not expressed in Java - expected " + expect + ": FAIL";
        boolean pass = (expect.equals("succeed") && r.kind().equals("succeeded"))
                    || (expect.equals("refused") && r.kind().equals("refused"))
                    || (expect.equals("error") && r.kind().equals("error"));
        return name + ": " + r.kind() + " (" + r.detail().replace('\n', ' ') + ") - expected " + expect + ": " + (pass ? "PASS" : "FAIL");
    }
}

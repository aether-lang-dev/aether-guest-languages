package aether.sandbox;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * The few libc calls this package makes through the Foreign Function &amp;
 * Memory API, in one place. New in aether-guest-languages: aether's copy of
 * the agent made the same calls inline, in AetherGrantChecker and AetherMap,
 * with {@code arena.allocateFrom} and {@code MemorySegment.getString}, which
 * only exist from JDK 22. This class uses the part of the API that is the
 * same in JDK 21 (a preview there, so build.sh compiles with
 * {@code --enable-preview} and the programs run with it) and in 22 and later
 * (final), so one source builds on both.
 *
 * A call returns its result and the C {@code errno} it left
 * ({@link #call}); strings go in as NUL-terminated bytes.
 */
public final class Native {
    private Native() {}

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup LOOKUP = LINKER.defaultLookup();
    private static final StructLayout CAPTURE = Linker.Option.captureStateLayout();
    // (Not a VarHandle: the ones a layout makes take one more coordinate from
    // JDK 22 than in 21, and the offset is the same in both.)
    private static final long ERRNO_OFFSET = CAPTURE.byteOffset(MemoryLayout.PathElement.groupElement("errno"));
    private static final Map<String, MethodHandle> HANDLES = new HashMap<>();

    private static final boolean MAC = System.getProperty("os.name").startsWith("Mac");

    /** errno values this package looks at: the same on Linux and macOS but EAGAIN. */
    public static final int EPERM = 1, EINTR = 4, EACCES = 13, EAGAIN = MAC ? 35 : 11;

    /** recv's flags, which differ: MSG_PEEK is 2 on both, MSG_DONTWAIT 0x40 on Linux and 0x80 on macOS. */
    public static final int MSG_PEEK = 0x2, MSG_DONTWAIT = MAC ? 0x80 : 0x40;

    /** What a call returned, and the errno it left. */
    public record Result(long value, int errno) {}

    private static ValueLayout layout(char c) {
        switch (c) {
            case 'i': return ValueLayout.JAVA_INT;
            case 'l': return ValueLayout.JAVA_LONG;
            case 'p': return ValueLayout.ADDRESS;
            default: throw new IllegalArgumentException("type " + c);
        }
    }

    // Where a symbol is looked up matters to a library interposed on libc (the
    // sandbox's preload): Linker.defaultLookup() finds libc's own function in the
    // JDK's helper library, past anything preloaded; dlsym(RTLD_DEFAULT) finds
    // what a C caller in this process would call, the interposer's first.
    // This class does the second, so its calls are the ones such a library
    // sees. ghl.containment.LookupTrace shows the difference.
    private static MemorySegment global(String name) {
        try (Arena arena = Arena.ofConfined()) {
            MethodHandle dlsym = LINKER.downcallHandle(
                LOOKUP.find("dlsym").orElseThrow(),
                FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
            // RTLD_DEFAULT: 0 on Linux, -2 on macOS.
            MemorySegment dflt = MemorySegment.ofAddress(System.getProperty("os.name").startsWith("Mac") ? -2L : 0L);
            MemorySegment p = (MemorySegment) dlsym.invoke(dflt, cstr(arena, name));
            if (p.address() != 0) return p.reinterpret(0);
        } catch (Throwable t) {
            // fall through to the default lookup
        }
        return LOOKUP.find(name).orElseThrow(() -> new UnsatisfiedLinkError("no libc symbol " + name));
    }

    private static synchronized MethodHandle handle(String name, String ret, String params, int variadicFrom) {
        String key = name + ":" + ret + ":" + params + ":" + variadicFrom;
        MethodHandle h = HANDLES.get(key);
        if (h != null) return h;
        MemorySegment sym = global(name);
        MemoryLayout[] ps = new MemoryLayout[params.length()];
        for (int i = 0; i < ps.length; i++) ps[i] = layout(params.charAt(i));
        FunctionDescriptor fd = ret.equals("v") ? FunctionDescriptor.ofVoid(ps)
                                                : FunctionDescriptor.of(layout(ret.charAt(0)), ps);
        if (variadicFrom >= 0) {
            h = LINKER.downcallHandle(sym, fd, Linker.Option.captureCallState("errno"),
                                      Linker.Option.firstVariadicArg(variadicFrom));
        } else {
            h = LINKER.downcallHandle(sym, fd, Linker.Option.captureCallState("errno"));
        }
        HANDLES.put(key, h);
        return h;
    }

    /**
     * Calls libc function {@code name}. {@code ret} is one of i (int), l
     * (long), p (pointer, returned as its address) or v (void); {@code params}
     * one letter each, with MemorySegment for p, Integer for i, Long for l.
     */
    public static Result call(String name, String ret, String params, Object... args) {
        return callVariadic(name, ret, params, -1, args);
    }

    /** As {@link #call}, for a variadic function whose first variadic argument is {@code from}. */
    public static Result callVariadic(String name, String ret, String params, int from, Object... args) {
        try (Arena arena = Arena.ofConfined()) {
            MethodHandle h = handle(name, ret, params, from);
            MemorySegment cap = arena.allocate(CAPTURE);
            Object[] all = new Object[args.length + 1];
            all[0] = cap;
            System.arraycopy(args, 0, all, 1, args.length);
            Object r = h.invokeWithArguments(all);
            long v = 0;
            if (r instanceof Integer i) v = i;
            else if (r instanceof Long l) v = l;
            else if (r instanceof MemorySegment s) v = s.address();
            return new Result(v, cap.get(ValueLayout.JAVA_INT, ERRNO_OFFSET));
        } catch (RuntimeException | Error e) {
            throw e;
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    /**
     * shm_open(name, oflag, mode). The mode is a variadic argument on macOS (a
     * non-variadic call puts it where the callee does not look, and an object
     * is created with a garbage mode), and an ordinary one on Linux, where
     * declaring it variadic changes nothing.
     */
    public static int shmOpen(Arena arena, String name, int oflag, int mode) {
        return (int) callVariadic("shm_open", "i", "pii", 2, cstr(arena, name), oflag, mode).value();
    }

    /** Makes the downcall handle for a function ready, so that using it later loads no more classes. */
    public static void prepare(String name, String ret, String params) {
        handle(name, ret, params, -1);
    }

    /** A NUL-terminated copy of {@code s} in {@code arena}. */
    public static MemorySegment cstr(Arena arena, String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        MemorySegment seg = arena.allocate(b.length + 1L);
        MemorySegment.copy(MemorySegment.ofArray(b), 0, seg, 0, b.length);
        seg.set(ValueLayout.JAVA_BYTE, b.length, (byte) 0);
        return seg;
    }

    /** The NUL-terminated string at {@code seg} (at most {@code max} bytes). */
    public static String string(MemorySegment seg, int max) {
        int n = 0;
        while (n < max && seg.get(ValueLayout.JAVA_BYTE, n) != 0) n++;
        byte[] b = new byte[n];
        MemorySegment.copy(seg, 0, MemorySegment.ofArray(b), 0, n);
        return new String(b, StandardCharsets.UTF_8);
    }

    /** The string at the C address {@code addr}, or null for 0. */
    public static String stringAt(long addr, int max) {
        if (addr == 0) return null;
        return string(MemorySegment.ofAddress(addr).reinterpret(max), max);
    }

    /** A segment of {@code n} bytes at the C address {@code addr}. */
    public static MemorySegment at(long addr, long n) {
        return MemorySegment.ofAddress(addr).reinterpret(n);
    }

    /** libc {@code read}, retrying EINTR: bytes read, 0 at EOF, or -errno. */
    public static int read(int fd, byte[] dst, int off, int len) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment buf = arena.allocate(len);
            while (true) {
                Result r = call("read", "l", "ipl", fd, buf, (long) len);
                if (r.value() < 0) {
                    if (r.errno() == EINTR) continue;
                    return -r.errno();
                }
                MemorySegment.copy(buf, 0, MemorySegment.ofArray(dst), off, r.value());
                return (int) r.value();
            }
        }
    }

    /** libc {@code write} of all of {@code src}: 0, or -errno. */
    public static int writeAll(int fd, byte[] src, int off, int len) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment buf = arena.allocate(len);
            MemorySegment.copy(MemorySegment.ofArray(src), off, buf, 0, len);
            long done = 0;
            while (done < len) {
                Result r = call("write", "l", "ipl", fd, buf.asSlice(done), (long) (len - done));
                if (r.value() < 0) {
                    if (r.errno() == EINTR) continue;
                    return -r.errno();
                }
                done += r.value();
            }
            return 0;
        }
    }

    /**
     * The contents of a POSIX shared memory object, up to {@code max} bytes. On
     * Linux a plain {@code read} does it (the object is a file under
     * /dev/shm); macOS refuses {@code read} on one (ENODEV), so there it is
     * fstat for the size and an mmap.
     */
    public static byte[] readShm(int fd, int max) {
        if (!System.getProperty("os.name").startsWith("Mac")) {
            byte[] buf = new byte[max];
            int n = read(fd, buf, 0, max);
            return n < 0 ? null : java.util.Arrays.copyOf(buf, n);
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment st = arena.allocate(256);
            if (call("fstat", "i", "ip", fd, st).value() != 0) return null;
            long size = Math.min(st.get(ValueLayout.JAVA_LONG, 96), (long) max);   // st_size, macOS arm64 and x86-64
            if (size <= 0) return new byte[0];
            Result m = call("mmap", "p", "pliiil", MemorySegment.NULL, size, 1 /* PROT_READ */, 1 /* MAP_SHARED */, fd, 0L);
            if (m.value() == -1L) return null;
            byte[] out = new byte[(int) size];
            MemorySegment.copy(at(m.value(), size), 0, MemorySegment.ofArray(out), 0, size);
            call("munmap", "i", "pl", at(m.value(), size), size);
            return out;
        }
    }

    public static int close(int fd) {
        return (int) call("close", "i", "i", fd).value();
    }

    // ---- the system call instruction itself ---------------------------------

    private static MethodHandle rawHandle;
    private static String rawWhy;

    /**
     * A raw system call: the {@code syscall} instruction, past libc and so past
     * any library interposed on libc, made from a few bytes of machine code
     * in a page of its own (x86-64 and aarch64 Linux). Returns the kernel's
     * result: the value, or -errno. Nothing in the JDK does this; the
     * contained child's kernel layer (Landlock, seccomp) is what holds it.
     */
    public static synchronized long rawSyscall(long nr, long a, long b, long c) {
        if (rawHandle == null && rawWhy == null) {
            try {
                String arch = System.getProperty("os.arch");
                byte[] code;
                if (arch.equals("amd64") || arch.equals("x86_64")) {
                    code = new byte[] { 0x48, (byte) 0x89, (byte) 0xf8,   // mov rax, rdi
                                        0x48, (byte) 0x89, (byte) 0xf7,   // mov rdi, rsi
                                        0x48, (byte) 0x89, (byte) 0xd6,   // mov rsi, rdx
                                        0x48, (byte) 0x89, (byte) 0xca,   // mov rdx, rcx
                                        0x0f, 0x05,                       // syscall
                                        (byte) 0xc3 };                    // ret
                } else if (arch.equals("aarch64")) {
                    int[] w = { 0xaa0003e8, 0xaa0103e0, 0xaa0203e1, 0xaa0303e2, 0xd4000001, 0xd65f03c0 };
                    code = new byte[w.length * 4];
                    for (int i = 0; i < w.length; i++) {
                        for (int j = 0; j < 4; j++) code[i * 4 + j] = (byte) (w[i] >>> (8 * j));
                    }
                } else {
                    throw new IllegalStateException("no system call stub for " + arch);
                }
                // MAP_PRIVATE | MAP_ANONYMOUS = 0x22 on Linux; PROT_READ | PROT_WRITE, then read and execute.
                Result m = call("mmap", "p", "pliiil", MemorySegment.NULL, 4096L, 3, 0x22, -1, 0L);
                if (m.value() == -1L || m.value() == 0) throw new IllegalStateException("mmap: errno " + m.errno());
                MemorySegment page = at(m.value(), 4096);
                MemorySegment.copy(MemorySegment.ofArray(code), 0, page, 0, code.length);
                Result p = call("mprotect", "i", "pli", page, 4096L, 5);
                if (p.value() != 0) throw new IllegalStateException("mprotect: errno " + p.errno());
                rawHandle = LINKER.downcallHandle(page, FunctionDescriptor.of(ValueLayout.JAVA_LONG,
                    ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));
            } catch (RuntimeException e) {
                rawWhy = e.getMessage();
            }
        }
        if (rawHandle == null) throw new IllegalStateException(rawWhy);
        try {
            return (long) rawHandle.invokeExact(nr, a, b, c);
        } catch (Throwable t) {
            throw new RuntimeException(t);
        }
    }

    /** The syscall numbers of the two rows that use it, for this machine. */
    public static long sysOpenat() { return System.getProperty("os.arch").equals("aarch64") ? 56 : 257; }
    public static long sysSocket() { return System.getProperty("os.arch").equals("aarch64") ? 198 : 41; }
}

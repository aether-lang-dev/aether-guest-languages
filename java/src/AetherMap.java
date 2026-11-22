package aether.sandbox;

import java.lang.foreign.*;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Reads and writes the Aether shared map through POSIX shared memory (Panama
 * FFI), in Aether's layout 2 (runtime/aether_shared_map.h, Aether 0.935.0).
 * Every integer is a little-endian uint32: the magic "AESM", a word that is
 * always 0, the version 2, frozen_count, entry_count, then per entry key_len,
 * value_len, the key's bytes and the value's (UTF-8). The first frozen_count
 * entries are the host's inputs; the rest are outputs. Keys and values are
 * bytes by their length, so a NUL (U+0000, one zero byte in UTF-8) in either
 * is carried. A block in any other shape is refused whole, as Aether's reader
 * refuses one: {@link #fromSharedMemory} throws and nothing of it is read.
 */
public class AetherMap {
    static final byte[] MAGIC = {'A', 'E', 'S', 'M'};
    static final int VERSION = 2;
    static final int HEADER = 20;
    static final int MAX_ENTRIES = 256;
    /** The largest block read, as Aether's reader (AETHER_SHARED_MAP_SHM_MAX_READ). */
    static final int MAX_READ = 64 * 1024 * 1024;

    private final Map<String, String> inputs = new LinkedHashMap<>();
    private final Map<String, String> outputs = new LinkedHashMap<>();
    private final String shmName;

    private AetherMap(String shmName) {
        this.shmName = shmName;
    }

    /**
     * The map in the shared memory object {@code shmName}: empty when there is
     * no such object; an IllegalStateException (nothing read) when its bytes
     * are not a layout-2 block.
     */
    public static AetherMap fromSharedMemory(String shmName) {
        AetherMap map = new AetherMap(shmName);
        byte[] block = map.readShm();
        if (block != null) map.decode(block);
        return map;
    }

    /** An input value, else an output, else null. */
    public String get(String key) {
        String v = inputs.get(key);
        return v != null ? v : outputs.get(key);
    }

    /** Put an output value (an input is frozen: ignored). */
    public void put(String key, String value) {
        if (inputs.containsKey(key)) return;
        outputs.put(key, value);
    }

    /** Write the inputs and outputs back to shared memory. */
    public void writeBack() {
        if (outputs.isEmpty()) return;
        writeShm(encode());
    }

    private static int le32(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8 | (b[at + 2] & 0xff) << 16 | (b[at + 3] & 0xff) << 24;
    }

    private static void put32(ByteArrayOutputStream o, int v) {
        o.write(v & 0xff); o.write((v >>> 8) & 0xff); o.write((v >>> 16) & 0xff); o.write((v >>> 24) & 0xff);
    }

    private static IllegalStateException refused(String why) {
        return new IllegalStateException("the shared map block " + why + ": refused");
    }

    /** Reads a layout-2 block into this map, or throws with nothing read. */
    void decode(byte[] d) {
        int n = d.length;
        if (n < HEADER || d[0] != MAGIC[0] || d[1] != MAGIC[1] || d[2] != MAGIC[2] || d[3] != MAGIC[3]
                || le32(d, 4) != 0 || le32(d, 8) != VERSION)
            throw refused("is not layout 2 (no AESM magic, zero word and version 2)");
        long frozen = le32(d, 12) & 0xffffffffL, count = le32(d, 16) & 0xffffffffL;
        if (count > MAX_ENTRIES || frozen > count) throw refused("counts more entries than it may");
        Map<String, String> in = new LinkedHashMap<>(), out = new LinkedHashMap<>();
        int pos = HEADER;
        for (int i = 0; i < count; i++) {
            if (n - pos < 8) throw refused("ends inside an entry");
            long kl = le32(d, pos) & 0xffffffffL, vl = le32(d, pos + 4) & 0xffffffffL;
            pos += 8;
            if (kl > n - pos || vl > n - pos - kl) throw refused("has an entry running past its end");
            String k = new String(d, pos, (int) kl, StandardCharsets.UTF_8);
            String v = new String(d, pos + (int) kl, (int) vl, StandardCharsets.UTF_8);
            pos += (int) (kl + vl);
            (i < frozen ? in : out).put(k, v);
        }
        // Zero bytes may follow (macOS rounds a shm object up to a page); nothing else.
        for (int z = pos; z < n; z++) if (d[z] != 0) throw refused("has bytes after its last entry");
        inputs.putAll(in);
        outputs.putAll(out);
    }

    /** This map as a layout-2 block: the inputs (frozen), then the outputs. */
    byte[] encode() {
        if (inputs.size() + outputs.size() > MAX_ENTRIES) throw new IllegalStateException("the shared map holds at most 256 keys");
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.writeBytes(MAGIC);
        put32(o, 0);
        put32(o, VERSION);
        put32(o, inputs.size());
        put32(o, inputs.size() + outputs.size());
        for (var m : java.util.List.of(inputs, outputs)) {
            for (var e : m.entrySet()) {
                byte[] kb = e.getKey().getBytes(StandardCharsets.UTF_8);
                byte[] vb = e.getValue().getBytes(StandardCharsets.UTF_8);
                put32(o, kb.length);
                put32(o, vb.length);
                o.writeBytes(kb);
                o.writeBytes(vb);
            }
        }
        return o.toByteArray();
    }

    // The object's size: fstat's st_size (a shared memory object cannot be
    // lseek'd on macOS). Its offset in struct stat: 96 on macOS (arm64 and
    // x86-64), 48 on Linux x86-64 and arm64.
    private static long size(int fd) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment st = arena.allocate(256);
            if (Native.call("fstat", "i", "ip", fd, st).value() != 0) return -1;
            boolean mac = System.getProperty("os.name").startsWith("Mac");
            return st.get(ValueLayout.JAVA_LONG, mac ? 96 : 48);
        }
    }

    /** The object's bytes, null when there is no such object. */
    private byte[] readShm() {
        try (Arena arena = Arena.ofConfined()) {
            int fd = Native.shmOpen(arena, shmName, 0 /* O_RDONLY */, 0);
            if (fd < 0) return null;
            try {
                long size = size(fd);
                if (size < 0 || size > MAX_READ) throw refused("is larger than a block may be");
                byte[] buf = Native.readShm(fd, (int) size);
                if (buf == null) throw refused("could not be read");
                return buf;
            } finally {
                Native.close(fd);
            }
        }
    }

    // The block, then zeros to the end of the object: macOS sizes a shared
    // memory object once (a later ftruncate fails), so a block shorter than the
    // object is followed by zero bytes, which a layout-2 reader allows.
    private void writeShm(byte[] data) {
        try (Arena arena = Arena.ofConfined()) {
            int fd = Native.shmOpen(arena, shmName, 2 /* O_RDWR */, 0600);
            if (fd < 0) throw new IllegalStateException("shm_open " + shmName + " for the write-back failed");
            try {
                Native.call("ftruncate", "i", "il", fd, (long) data.length);
                long size = size(fd);
                if (size < data.length) throw new IllegalStateException("the shared map block does not fit its object (" + size + " bytes)");
                Native.Result m = Native.call("mmap", "p", "pliiil",
                    MemorySegment.NULL, size, 3 /* PROT_READ|PROT_WRITE */, 1 /* MAP_SHARED */, fd, 0L);
                if (m.value() == -1L) throw new IllegalStateException("mmap " + shmName + ": errno " + m.errno());
                MemorySegment mem = Native.at(m.value(), size);
                MemorySegment.copy(MemorySegment.ofArray(data), 0, mem, 0, data.length);
                mem.asSlice(data.length).fill((byte) 0);
                Native.call("munmap", "i", "pl", mem, size);
            } finally {
                Native.close(fd);
            }
        }
    }
}

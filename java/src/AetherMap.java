package aether.sandbox;

import java.lang.foreign.*;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Reads/writes the Aether shared map via POSIX shared memory (Panama FFI).
 * Format: frozen_count(4 bytes) + key\0value\0...key\0value\0\0
 */
public class AetherMap {
    private final Map<String, String> inputs = new LinkedHashMap<>();
    private final Map<String, String> outputs = new LinkedHashMap<>();
    private final String shmName;
    private int frozenCount;

    private AetherMap(String shmName) {
        this.shmName = shmName;
    }

    /** Load map from shared memory */
    public static AetherMap fromSharedMemory(String shmName) {
        AetherMap map = new AetherMap(shmName);
        map.readFromShm();
        return map;
    }

    /** Get an input value */
    public String get(String key) {
        return inputs.get(key);
    }

    /** Put an output value (cannot overwrite inputs) */
    public void put(String key, String value) {
        if (inputs.containsKey(key)) return;  // frozen input
        outputs.put(key, value);
    }

    /** Write outputs back to shared memory */
    public void writeBack() {
        writeToShm();
    }

    private void readFromShm() {
        try (Arena arena = Arena.ofConfined()) {
            int fd = Native.shmOpen(arena, shmName, 0 /* O_RDONLY */, 0);
            if (fd < 0) return;

            byte[] buf = Native.readShm(fd, 65536);
            Native.close(fd);
            int bytesRead = buf == null ? 0 : buf.length;

            if (bytesRead <= 4) return;

            // Parse frozen_count
            frozenCount = (buf[0] & 0xff) | (buf[1] & 0xff) << 8
                        | (buf[2] & 0xff) << 16 | (buf[3] & 0xff) << 24;

            // Parse key\0value\0 pairs
            int pos = 4;
            while (pos < bytesRead - 1) {
                if (buf[pos] == 0) break;  // double-null terminator

                int keyStart = pos;
                while (pos < bytesRead && buf[pos] != 0) pos++;
                String key = new String(buf, keyStart, pos - keyStart, StandardCharsets.UTF_8);
                pos++; // skip null

                int valStart = pos;
                while (pos < bytesRead && buf[pos] != 0) pos++;
                String value = new String(buf, valStart, pos - valStart, StandardCharsets.UTF_8);
                pos++; // skip null

                inputs.put(key, value);
            }
        } catch (Throwable e) {
            System.err.println("[aether-map-java] read error: " + e.getMessage());
        }
    }

    private void writeToShm() {
        if (outputs.isEmpty()) return;
        try (Arena arena = Arena.ofConfined()) {
            // Build buffer: frozen_count + inputs + outputs
            byte[] data = new byte[65536];
            int pos = 0;
            data[pos++] = (byte)(frozenCount & 0xff);
            data[pos++] = (byte)((frozenCount >> 8) & 0xff);
            data[pos++] = (byte)((frozenCount >> 16) & 0xff);
            data[pos++] = (byte)((frozenCount >> 24) & 0xff);

            // Write inputs (frozen), then outputs
            for (var map : java.util.List.of(inputs, outputs)) {
                for (var e : map.entrySet()) {
                    byte[] kb = e.getKey().getBytes(StandardCharsets.UTF_8);
                    byte[] vb = e.getValue().getBytes(StandardCharsets.UTF_8);
                    System.arraycopy(kb, 0, data, pos, kb.length); pos += kb.length;
                    data[pos++] = 0;
                    System.arraycopy(vb, 0, data, pos, vb.length); pos += vb.length;
                    data[pos++] = 0;
                }
            }
            data[pos++] = 0; // double-null

            // Write to shm (O_RDWR=2)
            int fd = Native.shmOpen(arena, shmName, 2 /* O_RDWR */, 0600);
            if (fd < 0) return;

            Native.call("ftruncate", "i", "il", fd, (long) pos);
            Native.Result m = Native.call("mmap", "p", "pliiil",
                MemorySegment.NULL, (long) pos, 3 /* PROT_READ|PROT_WRITE */,
                1 /* MAP_SHARED */, fd, 0L);
            if (m.value() == -1L) { Native.close(fd); return; }  // MAP_FAILED
            MemorySegment mem = Native.at(m.value(), pos);
            MemorySegment.copy(MemorySegment.ofArray(data), 0, mem, 0, pos);
            Native.call("munmap", "i", "pl", mem, (long) pos);
            Native.close(fd);
        } catch (Throwable e) {
            System.err.println("[aether-map-java] write error: " + e.getMessage());
        }
    }

}

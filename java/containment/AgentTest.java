// The agent's two shared-memory readers, held to a real POSIX shared memory
// object through the Foreign Function API (aether.sandbox.Native): the grant
// list (AetherGrantChecker.fromSharedMemory, what the agent does at premain)
// and the shared map (AetherMap, the format the host's aether_shared_map
// writes). Platform-neutral, with no sandbox in the way: the test makes the
// objects itself. Run by containment/test_ways.sh.
package ghl.containment;

import aether.sandbox.AetherGrantChecker;
import aether.sandbox.AetherMap;
import aether.sandbox.Native;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;

public final class AgentTest {
    private static int failures;

    private static void expect(boolean ok, String what) {
        if (!ok) { failures++; System.out.println("  [FAIL] " + what); }
    }

    // shm_open(name, O_CREAT | O_RDWR), ftruncate to a page, write the bytes. (The
    // size is a page because macOS sizes a shared memory object once, when it
    // is first truncated: a map written back later must fit in what is there.)
    private static void put(String name, byte[] data) {
        try (Arena arena = Arena.ofConfined()) {
            int oflags = System.getProperty("os.name").startsWith("Mac") ? 0x202 : 0x42;   // O_CREAT | O_RDWR
            int fd = Native.shmOpen(arena, name, oflags, 0600);
            if (fd < 0) throw new IllegalStateException("shm_open " + name);
            Native.call("ftruncate", "i", "il", fd, 4096L);
            // (macOS does not write() a shared memory object: it is mmapped.)
            Native.Result m = Native.call("mmap", "p", "pliiil", MemorySegment.NULL, 4096L, 3, 1, fd, 0L);
            if (m.value() == -1L) throw new IllegalStateException("mmap " + name + ": errno " + m.errno());
            MemorySegment mem = Native.at(m.value(), 4096);
            MemorySegment.copy(MemorySegment.ofArray(data), 0, mem, 0, data.length);
            Native.call("munmap", "i", "pl", mem, 4096L);
            Native.close(fd);
        }
    }

    private static void unlink(String name) {
        try (Arena arena = Arena.ofConfined()) {
            Native.call("shm_unlink", "i", "p", Native.cstr(arena, name));
        }
    }

    public static void main(String[] args) {
        String base = "/ghl_java_" + ProcessHandle.current().pid();
        String grants = base + "_g", map = base + "_m";
        try {
            // The grant list, as the host's serialiser writes it: lines, one NUL.
            put(grants, "fs_read:/etc/*\ntcp:192.0.2.1:443\nenv:HOME\n\u0000".getBytes(StandardCharsets.UTF_8));
            AetherGrantChecker g = AetherGrantChecker.fromSharedMemory(grants);
            expect(g.check("fs_read", "/etc/hosts"), "grant read from shared memory: fs_read /etc/hosts");
            expect(!g.check("fs_read", "/root/x"), "grant read from shared memory: fs_read /root/x refused");
            expect(g.checkNet("tcp", "::ffff:192.0.2.1", 443), "grant read from shared memory: tcp 192.0.2.1:443 as ipv4-mapped");
            expect(!g.checkNet("tcp", "192.0.2.1", 80), "grant read from shared memory: another port refused");
            expect(g.check("env", "HOME") && !g.check("env", "PATH"), "grant read from shared memory: env");
            expect(!AetherGrantChecker.fromSharedMemory(grants + "_none").check("fs_read", "/etc/hosts"),
                   "a segment that is not there grants nothing");

            // The shared map: frozen count (LE int), key\0value\0 pairs, a final \0.
            byte[] m = ("\u0001\u0000\u0000\u0000" + "input\u0000stunt-in\u0000" + "\u0000").getBytes(StandardCharsets.UTF_8);
            put(map, m);
            AetherMap a = AetherMap.fromSharedMemory(map);
            expect("stunt-in".equals(a.get("input")), "map: the host's input is read");
            expect(a.get("nope") == null, "map: an absent key is null");
            a.put("input", "TAMPERED");     // a frozen input: ignored
            a.put("output", "from-java");
            a.writeBack();
            AetherMap b = AetherMap.fromSharedMemory(map);
            expect("stunt-in".equals(b.get("input")), "map: the frozen input survives a write-back");
            expect("from-java".equals(b.get("output")), "map: an output is written back and read again");
        } finally {
            unlink(grants);
            unlink(map);
        }
        if (failures == 0) System.out.println("  [PASS] AgentTest: the grant list and the map through shared memory");
        System.exit(failures == 0 ? 0 : 1);
    }
}

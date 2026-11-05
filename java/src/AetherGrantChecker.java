package aether.sandbox;

import java.io.*;
import java.lang.foreign.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Reads Aether sandbox grants and checks permissions.
 * Uses Panama FFI to read shared memory (no JNI).
 */
public class AetherGrantChecker {
    private final List<String[]> grants; // [category, pattern] pairs

    private AetherGrantChecker(List<String[]> grants) {
        this.grants = grants;
    }

    public boolean check(String category, String resource) {
        for (String[] grant : grants) {
            String cat = grant[0];
            String pat = grant[1];
            // Wildcard: *:*
            if ("*".equals(cat) && "*".equals(pat)) return true;
            if (cat.equals(category)) {
                if (patternMatch(category, pat, resource)) return true;
            }
        }
        return false;
    }

    /** Is a connect to {@code host}:{@code port} granted? */
    public boolean checkNet(String category, String host, int port) {
        return check(category, netResource(host, port));
    }

    /** The resource a tcp/udp check names: "host:port", an IPv6 address in brackets. */
    static String netResource(String host, int port) {
        if (host == null) host = "";
        if (host.indexOf(':') >= 0 && !host.startsWith("[")) return "[" + host + "]:" + port;
        return host + ":" + port;
    }

    // The same rules as the one matcher every other place uses
    // (aether_sandbox_match.h: the generated checker, the LD_PRELOAD library
    // and the bridges): "*" matches anything, "/etc/*" a prefix, "*.example.com"
    // a suffix, anything else exactly; "::ffff:a.b.c.d" is compared as
    // a.b.c.d; and for tcp and udp a pattern may name a port ("db:5432",
    // "[::1]:8080"), the resource being "host:port": a pattern without a port
    // matches any port, one with a port only that port.
    private static boolean patternMatch(String category, String pat, String resource) {
        if ("tcp".equals(category) || "udp".equals(category)) {
            Object[] p = splitHostPort(pat);
            Object[] r = splitHostPort(resource);
            int pport = (Integer) p[1], rport = (Integer) r[1];
            if (pport >= 0 && pport != rport) return false;
            return glob((String) p[0], (String) r[0]);
        }
        return glob(pat, resource);
    }

    private static boolean glob(String pat, String resource) {
        // A grant for "10.0.0.1" matches a TCP resource reported as
        // "::ffff:10.0.0.1" (and vice versa). Safe for non-TCP categories
        // because "::ffff:" doesn't appear in filesystem paths, env var
        // names, or exec command strings.
        if (pat.startsWith("::ffff:")) pat = pat.substring(7);
        if (resource.startsWith("::ffff:")) resource = resource.substring(7);
        // Wildcard
        if ("*".equals(pat)) return true;
        // Prefix: /etc/*
        if (pat.length() > 1 && pat.endsWith("*")
                && resource.startsWith(pat.substring(0, pat.length() - 1))) return true;
        // Suffix: *.example.com
        if (pat.length() > 1 && pat.startsWith("*")
                && resource.endsWith(pat.substring(1))) return true;
        // Exact
        return pat.equals(resource);
    }

    /** "host:port", "[v6]:port" or "host" as {host without brackets, port or -1}. */
    static Object[] splitHostPort(String s) {
        String host = s;
        int port = -1;
        int colon = s.lastIndexOf(':');
        if (s.startsWith("[")) {
            int rb = s.indexOf(']');
            if (rb > 0) {
                host = s.substring(1, rb);
                if (rb + 2 < s.length() && s.charAt(rb + 1) == ':') port = atoi(s.substring(rb + 2));
            }
        } else if (colon >= 0 && s.indexOf(':') == colon && colon + 1 < s.length()
                   && s.substring(colon + 1).chars().allMatch(Character::isDigit)) {
            host = s.substring(0, colon);
            port = atoi(s.substring(colon + 1));
        }
        return new Object[] { host, port };
    }

    private static int atoi(String s) {
        try { return Integer.parseInt(s); } catch (NumberFormatException e) { return 0; }
    }

    /** Grants from text, category:pattern per line. */
    public static AetherGrantChecker fromText(String data) {
        List<String[]> grants = new ArrayList<>();
        for (String line : data.split("\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int colon = line.indexOf(':');
            if (colon > 0) {
                grants.add(new String[]{
                    line.substring(0, colon),
                    line.substring(colon + 1)
                });
            }
        }
        return new AetherGrantChecker(grants);
    }

    /** Read grants from a file (category:pattern per line) */
    public static AetherGrantChecker fromFile(String path) {
        List<String[]> grants = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                int colon = line.indexOf(':');
                if (colon > 0) {
                    grants.add(new String[]{
                        line.substring(0, colon),
                        line.substring(colon + 1)
                    });
                }
            }
        } catch (IOException e) {
            System.err.println("[aether-sandbox] cannot read grant file: " + e.getMessage());
        }
        return new AetherGrantChecker(grants);
    }

    /** Read grants from POSIX shared memory via the Foreign Function API */
    public static AetherGrantChecker fromSharedMemory(String shmName) {
        List<String[]> grants = new ArrayList<>();
        try (Arena arena = Arena.ofConfined()) {
            // shm_open(name, O_RDONLY, 0) -> fd
            int fd = Native.shmOpen(arena, shmName, 0 /* O_RDONLY */, 0);
            if (fd < 0) {
                System.err.println("[aether-sandbox] cannot open shared memory: " + shmName);
                return new AetherGrantChecker(grants);
            }

            // Read up to 8KB of grants
            byte[] buf = Native.readShm(fd, 8192);
            Native.close(fd);
            int bytesRead = buf == null ? 0 : buf.length;

            if (bytesRead > 0) {
                int end = 0;
                while (end < bytesRead && buf[end] != 0) end++;
                String data = new String(buf, 0, end, StandardCharsets.UTF_8);
                for (String line : data.split("\n")) {
                    line = line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int colon = line.indexOf(':');
                    if (colon > 0) {
                        grants.add(new String[]{
                            line.substring(0, colon),
                            line.substring(colon + 1)
                        });
                    }
                }
            }
        } catch (Throwable e) {
            System.err.println("[aether-sandbox] FFI error reading shared memory: " + e.getMessage());
        }
        return new AetherGrantChecker(grants);
    }
}

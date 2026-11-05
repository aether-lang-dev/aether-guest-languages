// GuestChild is the worker's side of std.guest in Java: the protocol that
// aether's std/guest/child speaks for an Aether worker, for a worker written
// in Java (the counterpart of go/guestchild). The protocol is std.ipc frames
// (a 4-byte big-endian length, then that many bytes, at most 64 KiB) of
// "VERB text" with JSON arguments; std/guest/README.md in the ae release has
// every message.
//
// A host starts the worker with guest.launch (sandbox.spawn_socket: Landlock
// and seccomp on Linux, and nothing on macOS or Windows). The worker has the
// socket as a descriptor, named by AETHER_IPC_FD (3). Java has no way to wrap
// an inherited descriptor in a stream or a channel, so the reads, writes and
// the receipt of a granted file's descriptor (SCM_RIGHTS) are libc's, through
// the Foreign Function API (aether.sandbox.Native).
//
//   readScript   take the host's MOUNTs, PROBE, LOGLEVEL and SCRIPT
//   say          tell the host the script printed something (SAY)
//   callHost     call a function the host mounted (CALL) and wait for its answer
//   poll         look, without waiting, for a CANCEL
//   done, error  end (DONE, ERROR); the worker exits after
//
// What std/guest/child.run adds for an Aether worker, entering
// sandbox.enforce with no grants before the script's first step, has no
// counterpart: a JVM cannot enter an Aether sandbox block. The wall around a
// Java worker is the one the host's spawn put there (README.md, "4").
package ghl.guestchild;

import aether.sandbox.Native;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public final class GuestChild {
    /** The largest frame the host accepts. */
    public static final int MAX_FRAME = 65536;
    /** The largest file a FILE reply is read up to. */
    public static final int MAX_FILE = 65536;

    public record Mount(String name, int min, int max) {}

    /** A host's answer to callHost: kind VALUE (text is the JSON value), FAIL (the message) or CANCEL (the reason). */
    public record Reply(String kind, String text) {}

    private final int fd;
    public String source = "";
    public final List<Mount> mounts = new ArrayList<>();
    public boolean probe;
    public int logLevel = -1;
    private String cancel = "";

    private GuestChild(int fd) { this.fd = fd; }

    /** Finds the socket the host started this worker with. */
    public static GuestChild open() {
        String v = System.getenv("AETHER_IPC_FD");
        if (v == null || v.isEmpty()) {
            throw new IllegalStateException("not started by a host: AETHER_IPC_FD is not set");
        }
        try {
            int fd = Integer.parseInt(v);
            if (fd < 0) throw new NumberFormatException();
            return new GuestChild(fd);
        } catch (NumberFormatException e) {
            throw new IllegalStateException("not started by a host: AETHER_IPC_FD is " + v);
        }
    }

    /** The descriptor of the socket, for a worker that wants to misbehave on purpose (the half-frame row). */
    public int fd() { return fd; }

    private static void die() { Runtime.getRuntime().halt(1); }

    private byte[] readN(int n) {
        byte[] buf = new byte[n];
        for (int got = 0; got < n; ) {
            int m = Native.read(fd, buf, got, n - got);
            if (m <= 0) return null;   // an error, or the host hung up
            got += m;
        }
        return buf;
    }

    /** One frame. The host going away ends the worker: there is nobody left to report to. */
    private String frame() {
        byte[] hdr = readN(4);
        if (hdr == null) { die(); return ""; }
        long n = ((hdr[0] & 0xffL) << 24) | ((hdr[1] & 0xff) << 16) | ((hdr[2] & 0xff) << 8) | (hdr[3] & 0xff);
        if (n > MAX_FRAME) { die(); return ""; }
        if (n == 0) return "";
        byte[] body = readN((int) n);
        if (body == null) { die(); return ""; }
        return new String(body, StandardCharsets.UTF_8);
    }

    /** Sends one frame (over MAX_FRAME it is sent as it is, and the host refuses it). */
    private void tell(String text) {
        byte[] t = text.getBytes(StandardCharsets.UTF_8);
        byte[] b = new byte[4 + t.length];
        b[0] = (byte) (t.length >>> 24); b[1] = (byte) (t.length >>> 16);
        b[2] = (byte) (t.length >>> 8);  b[3] = (byte) t.length;
        System.arraycopy(t, 0, b, 4, t.length);
        if (Native.writeAll(fd, b, 0, b.length) != 0) die();
    }

    /** Writes raw bytes to the socket (the half-frame row). */
    public void raw(byte[] b) { Native.writeAll(fd, b, 0, b.length); }

    private static String[] splitVerb(String frame) {
        int i = frame.indexOf(' ');
        if (i >= 0) return new String[] { frame.substring(0, i), frame.substring(i + 1) };
        return new String[] { frame, "" };
    }

    /** Takes the host's frames up to SCRIPT. A frame that breaks the protocol is answered with ERROR, and null is returned. */
    public String readScript() {
        while (true) {
            String[] v = splitVerb(frame());
            switch (v[0]) {
                case "MOUNT": {
                    String[] f = v[1].trim().split("\\s+");
                    if (f.length != 3) return protocol("MOUNT is not 'name min max'");
                    try {
                        int lo = Integer.parseInt(f[1]), hi = Integer.parseInt(f[2]);
                        if (lo < 0 || hi < lo) return protocol("MOUNT is not 'name min max'");
                        mounts.add(new Mount(f[0], lo, hi));
                    } catch (NumberFormatException e) {
                        return protocol("MOUNT is not 'name min max'");
                    }
                    break;
                }
                case "PROBE": probe = true; break;
                case "LOGLEVEL": {
                    Integer lv = level(v[1]);
                    if (lv == null) return protocol("LOGLEVEL out of range");
                    logLevel = lv;
                    break;
                }
                case "SCRIPT": source = v[1]; return "";
                default: return protocol("unexpected '" + v[0] + "' before SCRIPT");
            }
        }
    }

    private String protocol(String why) {
        tell("ERROR protocol: " + why);
        return null;
    }

    private static Integer level(String rest) {
        try {
            int lv = Integer.parseInt(rest.trim());
            return lv < -1 || lv > 5 ? null : lv;
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /** Says whether the host mounted name. */
    public boolean isMounted(String name) {
        for (Mount m : mounts) if (m.name().equals(name)) return true;
        return false;
    }

    /** The script printed text. */
    public void say(String text) { tell("SAY " + text); }
    /** A line for the host's audit log. */
    public void audit(String line) { tell("AUDIT " + line); }
    /** A message at level 0 (trace) to 5 (fatal) with tags, a JSON object, when the host's threshold lets it through. */
    public void log(int level, String tags, String text) {
        if (logLevel < 0 || level < logLevel || level > 5) return;
        tell("LOG " + level + " " + tags.length() + " " + tags + " " + text);
    }
    /** The script ended with code. The worker exits after it. */
    public void done(int code) { tell("DONE " + code); }
    /** The script failed, or was cancelled ("cancelled by host: ..."). */
    public void error(String why) { tell("ERROR " + why); }

    /** The reason the host cancelled, or "". */
    public String cancelled() { return cancel; }

    /** Looks, without waiting, for a CANCEL: "" to go on, or the reason. */
    public String poll() {
        while (cancel.isEmpty()) {
            long r;
            int err;
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment b = arena.allocate(1);
                // recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT)
                Native.Result res = Native.call("recv", "l", "ipli", fd, b, 1L, Native.MSG_PEEK | Native.MSG_DONTWAIT);
                r = res.value();
                err = res.errno();
            }
            if (r < 0 && err == Native.EINTR) continue;
            if (r < 0 && err == Native.EAGAIN) return "";
            if (r <= 0) die();
            String[] v = splitVerb(frame());
            switch (v[0]) {
                case "CANCEL": cancel = reason(v[1]); break;
                case "LOGLEVEL": cancel = setLevel(v[1]); break;
                default: cancel = "protocol: unexpected '" + v[0] + "' while the script runs";
            }
        }
        return cancel;
    }

    private static String reason(String rest) { return rest.isEmpty() ? "no reason given" : rest; }

    private String setLevel(String rest) {
        Integer lv = level(rest);
        if (lv == null) return "protocol: LOGLEVEL out of range";
        logLevel = lv;
        return "";
    }

    /** Calls the mounted function name with args, a JSON array, and waits for the answer. After a cancel it sends nothing and returns it again. */
    public Reply callHost(String name, String args) {
        if (!cancel.isEmpty()) return new Reply("CANCEL", cancel);
        tell("CALL " + name + " " + args);
        while (true) {
            String[] v = splitVerb(frame());
            switch (v[0]) {
                case "LOGLEVEL": {
                    String why = setLevel(v[1]);
                    if (!why.isEmpty() && cancel.isEmpty()) cancel = why;
                    break;
                }
                case "REPLY": return answered("VALUE", v[1]);
                case "FAIL": return answered("FAIL", v[1].isEmpty() ? "host function failed" : v[1]);
                case "FILE": return answered("VALUE", takeFile());
                case "CANCEL":
                    if (cancel.isEmpty()) cancel = reason(v[1]);
                    return new Reply("CANCEL", cancel);
                default:
                    if (cancel.isEmpty()) cancel = "protocol: unexpected '" + v[0] + "' for a reply";
                    return new Reply("CANCEL", cancel);
            }
        }
    }

    private Reply answered(String kind, String text) {
        if (!cancel.isEmpty()) return new Reply("CANCEL", cancel);
        return new Reply(kind, text);
    }

    private static String granted(boolean ok, String text, String why) {
        if (ok) return "{\"granted\":true,\"text\":" + jsonString(text) + "}";
        return "{\"granted\":false,\"why\":" + jsonString(why) + "}";
    }

    /** A JSON string literal. */
    public static String jsonString(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) b.append(String.format("\\u%04x", (int) c)); else b.append(c);
            }
        }
        return b.append('"').toString();
    }

    // FILE's descriptor rides on the next byte (SCM_RIGHTS). The frame before
    // it was read byte-exact, so this recvmsg gets that byte and no more.
    // Linux's struct msghdr (56 bytes), iovec (16) and cmsghdr (16 + data).
    private String takeFile() {
        long n;
        int err;
        int got = -1;
        String problem = null;
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment one = arena.allocate(1);
            MemorySegment iov = arena.allocate(16);
            MemorySegment ctl = arena.allocate(64);
            MemorySegment hdr = arena.allocate(56);
            iov.set(ValueLayout.JAVA_LONG, 0, one.address());
            iov.set(ValueLayout.JAVA_LONG, 8, 1L);
            hdr.set(ValueLayout.JAVA_LONG, 16, iov.address());
            hdr.set(ValueLayout.JAVA_LONG, 24, 1L);
            hdr.set(ValueLayout.JAVA_LONG, 32, ctl.address());
            hdr.set(ValueLayout.JAVA_LONG, 40, 64L);
            Native.Result r;
            do {
                r = Native.call("recvmsg", "l", "ipi", fd, hdr, 0);
            } while (r.value() < 0 && r.errno() == Native.EINTR);
            n = r.value();
            err = r.errno();
            if (n < 0) {
                problem = "recvmsg: errno " + err;
            } else {
                long ctlLen = hdr.get(ValueLayout.JAVA_LONG, 40);
                if (n == 0 && ctlLen == 0) { die(); return ""; }
                // Walk the control messages: SOL_SOCKET (1), SCM_RIGHTS (1).
                long off = 0;
                while (off + 16 <= ctlLen) {
                    long len = ctl.get(ValueLayout.JAVA_LONG, off);
                    int level = ctl.get(ValueLayout.JAVA_INT, off + 8);
                    int type = ctl.get(ValueLayout.JAVA_INT, off + 12);
                    if (len < 16) break;
                    if (level == 1 && type == 1) {
                        for (long p = off + 16; p + 4 <= off + len; p += 4) {
                            int f = ctl.get(ValueLayout.JAVA_INT, p);
                            if (got < 0) got = f; else Native.close(f);
                        }
                    }
                    off += (len + 7) & ~7L;
                }
                if (got < 0) problem = "no descriptor came with FILE";
            }
        }
        if (problem != null) return granted(false, "", problem);
        byte[] buf = new byte[MAX_FILE + 1];
        int total = 0;
        while (total < buf.length) {
            int m = Native.read(got, buf, total, buf.length - total);
            if (m <= 0) break;
            total += m;
        }
        Native.close(got);
        if (total > MAX_FILE) return granted(false, "", "file larger than " + MAX_FILE + " bytes");
        return granted(true, new String(buf, 0, total, StandardCharsets.UTF_8), "");
    }
}

// java-worker: Java in a contained child, the guest's side of std.guest.
//
// A host starts this with guest.launch (sandbox.spawn_socket: Landlock and
// seccomp on Linux; refused on macOS and Windows, where nothing starts) and
// sends it the functions it may call (MOUNT) and a script (SCRIPT). The
// worker is the program: it takes the script as a command list for the Java
// code compiled into it (this one holds the rows of stunt's catalogue), the
// way go-worker does. A worker of your own replaces runScript and keeps
// ghl.guestchild.GuestChild, the protocol:
//
//   say TEXT             SAY TEXT
//   call NAME [JSON]     CALL NAME JSON (default []), the answer SAY'd as
//                        "VALUE <json>", "FAIL <message>"; a CANCEL ends the script
//   fact NAME VALUE      what the host knew before the sandbox (HOST_HOME_SET,
//                        HOST_SH, LIBM_PATH, LIBC_NAME, LIBC_PATH)
//   row NAME EXPECT      run one row of stunt's catalogue, SAY its line
//   spin                 loop, looking for a CANCEL: ERROR cancelled by host
//   silent               loop and never look: the host must kill the worker
//   oversize             SAY one 1 MiB message: the host must refuse the frame
//   half-frame           write a frame's header and part of its payload, stall
//
// Lines are separated by newlines. The end of the script is DONE 0; a
// failure is ERROR.
//
// There is no counterpart of std.guest.child.run's lockdown inside the
// worker: a JVM cannot enter an Aether sandbox block, so the wall is the
// spawn's (Landlock, seccomp, and the preload, which a JVM is dynamic enough
// to load). The spawn's grants are the worker's own authority, and a JVM
// needs the JDK to start: its directory is on the spawn's read list
// (containment/child_host.ae).
//
// Run by a host: java --enable-native-access=ALL-UNNAMED -cp <classes> ghl.worker.Worker
package ghl.worker;

import ghl.guestchild.GuestChild;
import ghl.rows.Rows;

public final class Worker {
    public static void main(String[] args) {
        GuestChild h;
        try {
            h = GuestChild.open();
        } catch (IllegalStateException e) {
            System.out.println(e.getMessage());
            System.exit(2);
            return;
        }
        if (h.readScript() == null) System.exit(1);
        String err = runScript(h);
        if (!err.isEmpty()) h.error(err); else h.done(0);
        // halt, not exit (see containment/Checks.java).
        Runtime.getRuntime().halt(0);
    }

    private static String cmdName(String s) {
        int i = s.indexOf(' ');
        return i >= 0 ? s.substring(0, i) : s;
    }

    private static String cmdRest(String s) {
        int i = s.indexOf(' ');
        return i >= 0 ? s.substring(i + 1) : "";
    }

    /** Returns "" when the script ended, else the message of the ERROR. */
    private static String runScript(GuestChild h) {
        boolean homeSet = false, sh = false;
        String[] lib = { "", "", "" };
        Rows.Caller call = (name, args) -> {
            GuestChild.Reply r = h.callHost(name, args);
            return new String[] { r.kind(), r.text() };
        };
        for (String line : h.source.split("\n")) {
            if (line.isEmpty()) continue;
            String why = h.poll();
            if (!why.isEmpty()) return "cancelled by host: " + why;
            String cmd = cmdName(line), rest = cmdRest(line);
            switch (cmd) {
                case "say": h.say(rest); break;
                case "call": {
                    String name = cmdName(rest), a = cmdRest(rest);
                    GuestChild.Reply r = h.callHost(name, a.isEmpty() ? "[]" : a);
                    if (r.kind().equals("CANCEL")) return "cancelled by host: " + r.text();
                    h.say(r.kind() + " " + r.text());
                    break;
                }
                case "fact": {
                    String name = cmdName(rest), v = cmdRest(rest);
                    switch (name) {
                        case "HOST_HOME_SET": homeSet = v.equals("1"); break;
                        case "HOST_SH": sh = v.equals("1"); break;
                        case "LIBM_PATH": lib[0] = v; break;
                        case "LIBC_NAME": lib[1] = v; break;
                        case "LIBC_PATH": lib[2] = v; break;
                        default: break;
                    }
                    break;
                }
                case "row": {
                    String name = cmdName(rest), expect = cmdRest(rest);
                    Rows.Facts facts = new Rows.Facts(homeSet, sh, lib[0], lib[1], lib[2]);
                    h.say(Rows.line(name, expect, new Rows.Env(facts, call)));
                    if (!h.cancelled().isEmpty()) return "cancelled by host: " + h.cancelled();
                    break;
                }
                case "spin":
                    while (true) {
                        String w = h.poll();
                        if (!w.isEmpty()) return "cancelled by host: " + w;
                        sleep(1);
                    }
                case "silent":
                    // Never looks: the host's CANCEL goes unanswered, and it kills us.
                    while (true) sleep(1);
                case "oversize":
                    h.say("x".repeat(1 << 20));
                    break;
                case "half-frame":
                    // A frame's 4-byte header saying 64 bytes, 10 of them, then nothing.
                    h.raw(new byte[] { 0, 0, 0, 0x40, 'C', 'A', 'L', 'L', ' ', 'g', 'r', 'e', 'e', 't' });
                    sleep(30000);
                    break;
                default:
                    return "unknown command " + cmd;
            }
        }
        return "";
    }

    private static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (InterruptedException e) { /* keep going */ }
    }
}

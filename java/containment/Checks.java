// Checks: the rows of stunt's catalogue (aether contrib/host/stunt), each
// done the way a Java program would do it, as a program the bridge starts.
// Usage:
//
//   java -cp <classes> ghl.containment.Checks <row> <expectation>
//
// Prints stunt's line for the row and exits 0 when it matched. The facts the
// host found before the sandbox was in force come in the environment
// (GHL_HOME_SET, GHL_SH, GHL_LIBM, GHL_LIBC_NAME, GHL_LIBC_PATH): the bridge
// gives a child no program arguments of its own beyond the main class, so the
// row and its expectation arrive as GHL_ROW and GHL_EXPECT when there is no
// command line. containment/host.ae sets them and calls java.run_sandboxed (or
// java.run) on this class; the shared map, when the host made one, is
// AETHER_MAP_SHM.
//
// The rows are ghl.rows.Rows, shared with the worker (worker/Worker.java),
// which runs the same Java code in a contained child.
package ghl.containment;

import ghl.rows.Rows;

public final class Checks {
    private static String env(String n) {
        String v = System.getenv(n);
        return v == null ? "" : v;
    }

    public static void main(String[] args) {
        String row = env("GHL_ROW"), expect = env("GHL_EXPECT");
        if (args.length == 2) { row = args[0]; expect = args[1]; }
        if (row.isEmpty() || expect.isEmpty()) {
            System.out.println("usage: Checks <row> <expectation>   (or GHL_ROW and GHL_EXPECT)");
            System.exit(2);
        }
        Rows.Facts facts = new Rows.Facts(env("GHL_HOME_SET").equals("1"), env("GHL_SH").equals("1"),
            env("GHL_LIBM"), env("GHL_LIBC_NAME"), env("GHL_LIBC_PATH"));
        String line = Rows.line(row, expect, new Rows.Env(facts, null));
        System.out.println(line);
        System.exit(line.endsWith(": PASS") ? 0 : 1);
    }
}

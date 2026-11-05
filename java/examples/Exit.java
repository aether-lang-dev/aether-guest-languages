// The smallest program the bridge's spec starts: exits with the status in
// GHL_EXIT (0 when unset). The bridge gives a program a class and nothing
// else, so the status travels in the environment.
package ghl.examples;

public final class Exit {
    public static void main(String[] args) {
        String s = System.getenv("GHL_EXIT");
        System.exit(s == null || s.isEmpty() ? 0 : Integer.parseInt(s));
    }
}

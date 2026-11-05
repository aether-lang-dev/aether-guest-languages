// The agent's grant checker held to the one matcher every other place uses
// (aether_sandbox_match.h in the ae release), with no JVM sandboxing
// involved: patterns, IPv4-mapped addresses, ports. Run by
// containment/test_ways.sh.
package ghl.containment;

import aether.sandbox.AetherGrantChecker;

public final class GrantCheckerTest {
    private static int failures;

    private static void expect(boolean got, boolean want, String what) {
        if (got != want) { failures++; System.out.println("  [FAIL] " + what + ": got " + got); }
    }

    public static void main(String[] args) {
        AetherGrantChecker g = AetherGrantChecker.fromText(String.join("\n",
            "# a comment",
            "fs_read:/etc/*",
            "fs_read:/data/one.txt",
            "env:HOME",
            "env:LC_*",
            "tcp:192.0.2.1:443",
            "tcp:*.example.com",
            "tcp:[2001:db8::1]:8080",
            "tcp:::ffff:198.51.100.7",
            "exec:/bin/sh", ""));
        expect(g.check("fs_read", "/etc/passwd"), true, "prefix");
        expect(g.check("fs_read", "/data/one.txt"), true, "exact");
        expect(g.check("fs_read", "/data/two.txt"), false, "exact, another file");
        expect(g.check("fs_write", "/etc/passwd"), false, "another category");
        expect(g.check("env", "HOME"), true, "env exact");
        expect(g.check("env", "LC_ALL"), true, "env prefix");
        expect(g.check("env", "PATH"), false, "env ungranted");
        expect(g.check("exec", "/bin/sh"), true, "exec");
        expect(g.check("exec", "/bin/ls"), false, "exec ungranted");
        // tcp: a pattern with a port covers that port only
        expect(g.checkNet("tcp", "192.0.2.1", 443), true, "host:port");
        expect(g.checkNet("tcp", "192.0.2.1", 80), false, "host:port, another port");
        expect(g.checkNet("tcp", "192.0.2.2", 443), false, "another host");
        expect(g.checkNet("tcp", "::ffff:192.0.2.1", 443), true, "ipv4-mapped resource, v4 grant");
        expect(g.checkNet("tcp", "::ffff:192.0.2.1", 80), false, "ipv4-mapped resource, another port");
        expect(g.checkNet("tcp", "198.51.100.7", 25), true, "v4 resource, ipv4-mapped grant, any port");
        expect(g.checkNet("tcp", "www.example.com", 443), true, "suffix, any port");
        expect(g.checkNet("tcp", "2001:db8::1", 8080), true, "bracketed v6 with port");
        expect(g.checkNet("tcp", "2001:db8::1", 80), false, "bracketed v6, another port");
        expect(g.check("tcp", "192.0.2.1"), false, "a host with no port is not covered by a pattern with one");
        expect(AetherGrantChecker.fromText("*:*").check("anything", "at all"), true, "*:*");
        expect(AetherGrantChecker.fromText("").check("fs_read", "/etc/passwd"), false, "no grants");
        if (failures == 0) System.out.println("  [PASS] GrantCheckerTest: the agent's checker matches as the one matcher does");
        System.exit(failures == 0 ? 0 : 1);
    }
}

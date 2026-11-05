// hello: the Java program the first three ways of README.md start. It says
// what it can reach: the environment it was started with, and a file it was
// not given.
package ghl.examples;

import java.nio.file.Files;
import java.nio.file.Path;

public final class Hello {
    public static void main(String[] args) {
        System.out.println("hello from Java, HOME = " + (System.getenv("HOME") == null ? "" : System.getenv("HOME")));
        try {
            Files.readAllBytes(Path.of("/etc/passwd"));
            System.out.println("/etc/passwd: read it");
        } catch (Exception e) {
            System.out.println("/etc/passwd: " + e);
        }
    }
}

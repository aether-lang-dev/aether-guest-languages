// Where an FFM caller looks a libc function up decides whether a library
// interposed on libc sees the call. Calls getenv("HOME") twice: through
// Linker.defaultLookup(), which the JDK documents and most programs use, and
// through aether.sandbox.Native, which looks the symbol up with
// dlsym(RTLD_DEFAULT) as a C caller would. Under containment/libghltrace.so
// only the second is seen (containment/test_ways.sh).
package ghl.containment;

import aether.sandbox.Native;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

public final class LookupTrace {
    public static void main(String[] args) throws Throwable {
        Linker linker = Linker.nativeLinker();
        MethodHandle getenv = linker.downcallHandle(linker.defaultLookup().find("getenv").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment home = Native.cstr(arena, "HOME");
            System.out.println("marker default-lookup");
            MemorySegment a = (MemorySegment) getenv.invoke(home);
            System.out.println("marker dlsym-default");
            long b = Native.call("getenv", "p", "p", home).value();
            System.out.println("marker end " + (a.address() != 0) + " " + (b != 0));
        }
    }
}

//go:build !linux

package rows

// The raw system call rows are Linux's (stunt runs them in the contained
// child only, which is Linux only).

func syscallFnOpenat() result { return failed("syscall-fn-openat: Linux only") }
func syscallOpenat() result   { return failed("syscall-openat: Linux only") }
func syscallSocket() result   { return failed("syscall-socket: Linux only") }

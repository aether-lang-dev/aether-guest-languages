package main

// The rows that go through libc from Go have to go through cgo: Go's own
// packages (os, net, syscall) make system calls directly on Linux, and only
// a call into C reaches libc, where aether's sandbox preload sits. These are
// those calls, kept apart from the //export file (cgo allows only
// declarations in the preamble of a file with //export).

/*
#cgo linux LDFLAGS: -ldl
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

// libc's syscall(), which cgo cannot call itself (it is variadic). The result
// is -errno on failure, so Go reads the right errno.
#ifdef __linux__
static long ghl_syscall4(long n, long a, long b, long c, long d) {
    errno = 0;
    long r = syscall(n, a, b, c, d);
    return r < 0 ? -(long)errno : r;
}
#else
static long ghl_syscall4(long n, long a, long b, long c, long d) { return -ENOSYS; }
#endif
static void* ghl_dlopen(const char* path) { return dlopen(path, RTLD_NOW); }
static const char* ghl_dlerror(void) { return dlerror(); }
static const char* ghl_getenv(const char* name) { return getenv(name); }
*/
import "C"

import "unsafe"

// libcGetenv is libc's getenv, not Go's copy of the environment.
func libcGetenv(name string) (string, bool) {
	cn := C.CString(name)
	defer C.free(unsafe.Pointer(cn))
	v := C.ghl_getenv(cn)
	if v == nil {
		return "", false
	}
	return C.GoString(v), true
}

// libcDlopen is dlopen(path, RTLD_NOW). The message is dlerror()'s, "" when
// the call failed without one (a refusal by the preload returns NULL and
// leaves no message).
func libcDlopen(path string) (ok bool, msg string) {
	cp := C.CString(path)
	defer C.free(unsafe.Pointer(cp))
	h := C.ghl_dlopen(cp)
	if h != nil {
		C.dlclose(h)
		return true, ""
	}
	if m := C.ghl_dlerror(); m != nil {
		return false, C.GoString(m)
	}
	return false, ""
}

// libcSyscall is libc's syscall(n, a, b, c, d): the result, or -errno.
func libcSyscall(n, a, b, c, d int64) int64 {
	return int64(C.ghl_syscall4(C.long(n), C.long(a), C.long(b), C.long(c), C.long(d)))
}

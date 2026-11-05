//go:build linux

package rows

import (
	"os"
	"syscall"
	"unsafe"
)

// libc's syscall(), through cgo: what stunt calls syscall-fn. Only EPERM or
// EACCES is a refusal (ENOSYS is the preload's syscall() with no grants in
// force, which is an error).
func syscallFnOpenat() result {
	if !haveCgo {
		return failed("syscall(3): " + "needs cgo (a C compiler and CGO_ENABLED=1)")
	}
	path, _ := syscall.BytePtrFromString("/etc/passwd")
	atFdcwd := -100
	r, e := libcSyscall4(syscall.SYS_OPENAT, uintptr(atFdcwd), uintptr(unsafe.Pointer(path)), uintptr(os.O_RDONLY), 0)
	if r >= 0 {
		syscall.Close(int(r))
		return succeeded("opened /etc/passwd")
	}
	return byErr(e, "syscall(): "+e.Error())
}

// Go's own way to the kernel: syscall.Syscall is the syscall instruction,
// past libc and so past any preload. Only the kernel (Landlock, seccomp) is
// left to hold what it does.
func rawResult(fd uintptr, e syscall.Errno, what string) result {
	if e == 0 {
		syscall.Close(int(fd))
		return succeeded(what)
	}
	return byErr(e, "the syscall instruction: "+e.Error())
}

func syscallOpenat() result {
	path, _ := syscall.BytePtrFromString("/etc/passwd")
	atFdcwd := -100
	fd, _, e := syscall.Syscall6(syscall.SYS_OPENAT, uintptr(atFdcwd), uintptr(unsafe.Pointer(path)), uintptr(os.O_RDONLY), 0, 0, 0)
	return rawResult(fd, e, "the syscall instruction opened /etc/passwd")
}

func syscallSocket() result {
	fd, _, e := syscall.Syscall(syscall.SYS_SOCKET, syscall.AF_INET, syscall.SOCK_STREAM, 0)
	return rawResult(fd, e, "the syscall instruction made an AF_INET socket")
}

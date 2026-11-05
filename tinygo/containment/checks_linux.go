//go:build linux

package main

// The rows that need Linux's system call numbers and fork. Go's syscall
// package has the numbers for amd64 and arm64, the two architectures stunt has
// raw syscalls on.

/*
#include <stdlib.h>
*/
import "C"

import (
	"os"
	"runtime"
	"sync"
	"syscall"
	"unsafe"
)

const atFdcwd = -100

func closeFd(r int64) { syscall.Close(int(r)) }

func rawResult(r uintptr, e syscall.Errno, ok string) result {
	if e == 0 {
		closeFd(int64(r))
		return succeeded("%s", ok)
	}
	return byErr(e, "the syscall instruction")
}

// libc's syscall(), through cgo: what stunt calls syscall-fn. Only EPERM or
// EACCES is a refusal (ENOSYS is the preload's syscall() with no grants in
// force, which is an error).
func syscallFnOpenat(facts) result {
	p := C.CString("/etc/passwd")
	defer C.free(unsafe.Pointer(p))
	r := libcSyscall(syscall.SYS_OPENAT, atFdcwd, int64(uintptr(unsafe.Pointer(p))), int64(os.O_RDONLY), 0)
	if r >= 0 {
		closeFd(r)
		return succeeded("opened /etc/passwd")
	}
	return byErr(syscall.Errno(-r), "syscall()")
}

// The syscall instruction itself, past libc and so past any preload:
// syscall.Syscall6 is the instruction and nothing else. (Go's os and net do
// the same for every call they make; these two rows are those calls with no
// package around them.)
func syscallOpenat(facts) result {
	p, _ := syscall.BytePtrFromString("/etc/passwd")
	r, _, e := syscall.Syscall6(syscall.SYS_OPENAT, uintptr(atFdcwdU()), uintptr(unsafe.Pointer(p)), uintptr(os.O_RDONLY), 0, 0, 0)
	return rawResult(r, e, "the syscall instruction opened /etc/passwd")
}

func syscallSocket(facts) result {
	r, _, e := syscall.Syscall(syscall.SYS_SOCKET, uintptr(syscall.AF_INET), uintptr(syscall.SOCK_STREAM), 0)
	return rawResult(r, e, "the syscall instruction made an AF_INET socket")
}

func atFdcwdU() int { return atFdcwd }

// fork, the child exiting at once. Go has no fork (its runtime has threads a
// forked child would not have); clone(SIGCHLD) with no new stack is one, and
// the child does nothing but exit_group. The parent waits.
func forkOnce(facts) result {
	r, _, e := syscall.RawSyscall(syscall.SYS_CLONE, uintptr(syscall.SIGCHLD), 0, 0)
	if e != 0 {
		return byErr(e, "clone")
	}
	if r == 0 {
		syscall.RawSyscall(syscall.SYS_EXIT_GROUP, 0, 0, 0)
	}
	var ws syscall.WaitStatus
	if _, err := syscall.Wait4(int(r), &ws, 0, nil); err != nil {
		return failed("wait4: %v", err)
	}
	if ws.Exited() && ws.ExitStatus() == 0 {
		return succeeded("the forked child ran")
	}
	return failed("the forked child: status %d", int(ws))
}

func manyThreads(facts) result {
	const n = 16
	var wg, started sync.WaitGroup
	var mu sync.Mutex
	tids := map[int]bool{}
	release := make(chan struct{})
	for i := 0; i < n; i++ {
		wg.Add(1)
		started.Add(1)
		go func() {
			defer wg.Done()
			runtime.LockOSThread()
			mu.Lock()
			tids[syscall.Gettid()] = true
			mu.Unlock()
			started.Done()
			<-release
		}()
	}
	started.Wait()
	close(release)
	wg.Wait()
	if len(tids) < n {
		return failed("%d goroutines locked to %d threads", n, len(tids))
	}
	return succeeded("%d goroutines each held a thread of their own", len(tids))
}

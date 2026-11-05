// The rows of stunt's catalogue (aether contrib/host/stunt), each done the
// way Go does it, built into a c-shared library the hosts load with the
// tinygo bridge (build/libchecks.so, or .dylib) and call by symbol:
//
//	const char* check_<row>(const char* facts, const char* expect)
//
// where <row> is stunt's row name with "-" and "." made "_" (open-etc-passwd
// is check_open_etc_passwd). The result is stunt's one line,
//
//	<check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
//
// a C.CString for the caller to free (the bridge's call_s_s_s_owned). facts
// is what the host found before the sandbox was in force, as KEY=VALUE
// pairs separated by ";": HOST_HOME_SET, HOST_SH, LIBM_PATH, LIBC_NAME,
// LIBC_PATH. A refusal is what a sandbox gives: EPERM or EACCES from libc,
// the preload, the kernel's Landlock or seccomp. Anything else that goes
// wrong is an error, never a refusal, so it cannot pass for a sandbox at work.
//
// What Go does matters more here than for any other language in this
// repository. On Linux Go's os, net, os/exec and syscall packages make
// system calls with the syscall instruction themselves and never call
// libc: aether's sandbox preload, which holds libc, does not see them. Only
// the rows done through cgo (getenv-home, the dlopen rows,
// syscall-fn-openat) reach it. The kernel layer of a contained child holds
// all of them. README.md, "Four ways", has the table.
//
// Not here, because the Go bridge has nothing to carry them: binding-*,
// callback-* and map-* (it has no host functions and no shared map) and,
// in-process, spin (no instruction limit; a Go call cannot be interrupted).
// tests/stunt_lib.sh's left_out in lang.sh says each.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const (
	fixture = "./stunt-fixture.txt"
	written = "./stunt-written.txt"
)

// result is one row's outcome: "succeeded", "refused" or "error", and why.
type result struct{ outcome, detail string }

func succeeded(format string, a ...any) result { return result{"succeeded", fmt.Sprintf(format, a...)} }
func refused(format string, a ...any) result   { return result{"refused", fmt.Sprintf(format, a...)} }
func failed(format string, a ...any) result    { return result{"error", fmt.Sprintf(format, a...)} }

// isRefusal: EPERM or EACCES, the same numbers on Linux, macOS and FreeBSD.
func isRefusal(err error) bool {
	return errors.Is(err, syscall.EPERM) || errors.Is(err, syscall.EACCES)
}

func byErr(err error, what string) result {
	if isRefusal(err) {
		return refused("%s: %v", what, err)
	}
	return failed("%s: %v", what, err)
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	if len(s) > 40 {
		s = s[:40]
	}
	return s
}

type facts map[string]string

func parseFacts(s string) facts {
	f := facts{}
	for _, kv := range strings.Split(s, ";") {
		if k, v, ok := strings.Cut(kv, "="); ok {
			f[k] = v
		}
	}
	return f
}

func (f facts) flag(k string) bool { return f[k] == "1" }

// row runs one check and judges it as stunt does.
func row(name string, cfacts, cexpect *C.char, fn func(f facts) result) *C.char {
	expect := C.GoString(cexpect)
	var r result
	func() {
		defer func() {
			if p := recover(); p != nil {
				r = failed("panic: %v", p)
			}
		}()
		r = fn(parseFacts(C.GoString(cfacts)))
	}()
	pass := (expect == "succeed" && r.outcome == "succeeded") ||
		(expect == "refused" && r.outcome == "refused") ||
		(expect == "error" && r.outcome == "error")
	verdict := "FAIL"
	if pass {
		verdict = "PASS"
	}
	return C.CString(fmt.Sprintf("%s: %s (%s) - expected %s: %s", name, r.outcome, r.detail, expect, verdict))
}

// ---- files: os.ReadFile and os.WriteFile, open(2) by raw syscall on Linux ----

func openPath(path string) func(facts) result {
	return func(facts) result {
		b, err := os.ReadFile(path)
		if err != nil {
			return byErr(err, "open")
		}
		return succeeded("read %q", firstLine(string(b)))
	}
}

func writeFile(facts) result {
	if err := os.WriteFile(written, []byte("written by go\n"), 0o644); err != nil {
		return byErr(err, "open")
	}
	return succeeded("wrote %s", written)
}

// ---- the environment ----

// libc's own getenv, through cgo. Go's os.Getenv never asks libc.
func libcGetenvHome(f facts) result {
	if v, ok := libcGetenv("HOME"); ok {
		return succeeded("HOME=%s", v)
	}
	if f.flag("HOST_HOME_SET") {
		return refused("libc getenv gave NULL; HOME is in the environment")
	}
	return failed("HOME is unset")
}

// os.Getenv and os.Environ: the copy of the environment Go took when the
// library was loaded, before any sandbox. A missing HOME is a refusal only
// when the host had one: then it was kept from this process (the contained
// child is given no environment).
func envCached(f facts) result {
	v, ok := os.LookupEnv("HOME")
	scanned := ""
	for _, kv := range os.Environ() {
		if strings.HasPrefix(kv, "HOME=") {
			scanned = kv[len("HOME="):]
		}
	}
	if ok && scanned == v {
		return succeeded("HOME=%s from os.Getenv and os.Environ", v)
	}
	if ok || scanned != "" {
		return failed("os.Getenv and os.Environ disagree: %q and %q", v, scanned)
	}
	if f.flag("HOST_HOME_SET") {
		return refused("HOME is not in os.Environ; the host has it")
	}
	return failed("HOME was never in the environment")
}

// ---- the network ----

// A connect judged as stunt judges it: EACCES at connect (the preload,
// Landlock) or EPERM/EACCES at socket (seccomp) is a refusal; EPERM at
// connect is a firewall's answer, an error; anything else (a timeout, a
// network error) means the call was let through. net.Dial, with a short
// timeout: nobody answers on TEST-NET.
func connectTo(network, addr string) func(facts) result {
	return func(facts) result {
		c, err := net.DialTimeout(network, addr, 300*time.Millisecond)
		if err == nil {
			c.Close()
			return succeeded("connected")
		}
		var se *os.SyscallError
		if errors.As(err, &se) {
			switch {
			case se.Syscall == "socket" && isRefusal(se.Err):
				return refused("socket: %v", se.Err)
			case se.Syscall == "connect" && errors.Is(se.Err, syscall.EACCES):
				return refused("connect: %v", se.Err)
			case se.Syscall == "connect" && errors.Is(se.Err, syscall.EPERM):
				return failed("connect: %v (a firewall's answer, not the sandbox's)", se.Err)
			}
		}
		return succeeded("let through: %v", err)
	}
}

// The same address spelt as IPv6, [::ffff:192.0.2.1]: net.Dial turns it back
// into an IPv4 dial (or refuses "tcp6" it), so this is the syscall package's
// own socket and connect, non-blocking, as stunt does it.
func connectMapped(facts) result {
	fd, err := syscall.Socket(syscall.AF_INET6, syscall.SOCK_STREAM, 0)
	if err != nil {
		return byErr(err, "socket")
	}
	defer syscall.Close(fd)
	syscall.SetNonblock(fd, true)
	sa := &syscall.SockaddrInet6{Port: 443}
	copy(sa.Addr[:], []byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1})
	err = syscall.Connect(fd, sa)
	switch {
	case err == nil:
		return succeeded("connected")
	case errors.Is(err, syscall.EACCES):
		return refused("connect: %v", err)
	case errors.Is(err, syscall.EPERM):
		return failed("connect: %v (a firewall's answer, not the sandbox's)", err)
	}
	return succeeded("let through: %v", err)
}

// ---- starting a program: os/exec and syscall.ForkExec ----

// /bin/sh -c "exit 0", both ways Go has: os/exec, and syscall.ForkExec
// itself. Succeeded when either started a shell: an escape by one way is an
// escape. Refused when both were refused (EPERM or EACCES, or the shell
// could not be executed, exit 127, while HOST_SH says /bin/sh is there).
func execSh(f facts) result {
	type way struct{ outcome, detail string }
	var ways []way
	add := func(r result, what string) { ways = append(ways, way{r.outcome, what + ": " + r.detail}) }

	err := exec.Command("/bin/sh", "-c", "exit 0").Run()
	var ee *exec.ExitError
	switch {
	case err == nil:
		add(succeeded("exit 0"), "os/exec")
	case errors.As(err, &ee) && ee.ExitCode() == 127 && f.flag("HOST_SH"):
		add(refused("the shell could not be executed (exit 127)"), "os/exec")
	case errors.As(err, &ee):
		add(failed("exit %d", ee.ExitCode()), "os/exec")
	default:
		add(byErr(err, "start"), "os/exec")
	}

	pid, err := syscall.ForkExec("/bin/sh", []string{"sh", "-c", "exit 0"}, &syscall.ProcAttr{
		Env:   []string{},
		Files: []uintptr{0, 1, 2},
	})
	if err != nil {
		add(byErr(err, "ForkExec"), "syscall")
	} else {
		var ws syscall.WaitStatus
		if _, werr := syscall.Wait4(pid, &ws, 0, nil); werr != nil {
			add(failed("wait4: %v", werr), "syscall")
		} else if ws.Exited() && ws.ExitStatus() == 0 {
			add(succeeded("exit 0"), "syscall")
		} else if ws.Exited() && ws.ExitStatus() == 127 && f.flag("HOST_SH") {
			add(refused("the shell could not be executed (exit 127)"), "syscall")
		} else {
			add(failed("status %d", int(ws)), "syscall")
		}
	}

	var parts []string
	allRefused, anySucceeded := true, false
	for _, w := range ways {
		parts = append(parts, w.detail)
		if w.outcome == "succeeded" {
			anySucceeded = true
		}
		if w.outcome != "refused" {
			allRefused = false
		}
	}
	detail := strings.Join(parts, "; ")
	switch {
	case anySucceeded:
		return result{"succeeded", detail}
	case allRefused:
		return result{"refused", detail}
	}
	return result{"error", detail}
}

// ---- dlopen: through cgo ----

// A dlopen that fails on its own leaves a dlerror() message; the preload's
// refusal returns NULL without one. That, or a "Permission denied" or
// "Operation not permitted" in the message, is the refusal.
func dlopenRow(key string) func(facts) result {
	return func(f facts) result {
		path := f[key]
		if path == "" {
			return failed("the host found no %s", key)
		}
		ok, msg := libcDlopen(path)
		if ok {
			return succeeded("loaded %s", path)
		}
		if msg == "" || strings.Contains(msg, "Permission denied") || strings.Contains(msg, "Operation not permitted") {
			if msg == "" {
				msg = "no dlerror() message"
			}
			return refused("dlopen %s: %s (the preload's refusal)", path, msg)
		}
		return failed("dlopen %s: %s", path, msg)
	}
}

// ---- exports ----

//export check_open_etc_passwd
func check_open_etc_passwd(f, e *C.char) *C.char {
	return row("open-etc-passwd", f, e, openPath("/etc/passwd"))
}

//export check_open_granted_file
func check_open_granted_file(f, e *C.char) *C.char {
	return row("open-granted-file", f, e, openPath(fixture))
}

//export check_write_granted_file
func check_write_granted_file(f, e *C.char) *C.char {
	return row("write-granted-file", f, e, writeFile)
}

//export check_getenv_home
func check_getenv_home(f, e *C.char) *C.char { return row("getenv-home", f, e, libcGetenvHome) }

//export check_env_cached
func check_env_cached(f, e *C.char) *C.char { return row("env-cached", f, e, envCached) }

//export check_connect_192_0_2_1
func check_connect_192_0_2_1(f, e *C.char) *C.char {
	return row("connect-192.0.2.1", f, e, connectTo("tcp4", "192.0.2.1:443"))
}

//export check_connect_ipv4_mapped
func check_connect_ipv4_mapped(f, e *C.char) *C.char {
	return row("connect-ipv4-mapped", f, e, connectMapped)
}

//export check_exec_bin_sh
func check_exec_bin_sh(f, e *C.char) *C.char { return row("exec-bin-sh", f, e, execSh) }

//export check_fork
func check_fork(f, e *C.char) *C.char { return row("fork", f, e, forkOnce) }

//export check_dlopen_libm
func check_dlopen_libm(f, e *C.char) *C.char { return row("dlopen-libm", f, e, dlopenRow("LIBM_PATH")) }

//export check_dlopen_libc_by_name
func check_dlopen_libc_by_name(f, e *C.char) *C.char {
	return row("dlopen-libc-by-name", f, e, dlopenRow("LIBC_NAME"))
}

//export check_dlopen_libc_by_path
func check_dlopen_libc_by_path(f, e *C.char) *C.char {
	return row("dlopen-libc-by-path", f, e, dlopenRow("LIBC_PATH"))
}

//export check_syscall_fn_openat
func check_syscall_fn_openat(f, e *C.char) *C.char {
	return row("syscall-fn-openat", f, e, syscallFnOpenat)
}

//export check_syscall_openat
func check_syscall_openat(f, e *C.char) *C.char { return row("syscall-openat", f, e, syscallOpenat) }

//export check_syscall_socket
func check_syscall_socket(f, e *C.char) *C.char { return row("syscall-socket", f, e, syscallSocket) }

// ---- not stunt's: the same libc calls from a goroutine ----

// Grant levels are per thread, and a goroutine started with `go` runs on a
// thread of the Go runtime's, not on the thread the host called the library
// from. So a libc call made there is held by the preload's process-wide grants
// (a contained child's policy) but not by the levels of the caller's
// sandbox.enforce block, nor by the contained worker's own lockdown.
// tinygo/containment/test_threads.sh holds it. The calling goroutine is
// locked to its thread while the library runs for the host, so the new one
// is on another.
func onAnotherThread(fn func(facts) result) func(facts) result {
	return func(f facts) result {
		out := make(chan result)
		go func() { out <- fn(f) }()
		return <-out
	}
}

//export check_getenv_home_goroutine
func check_getenv_home_goroutine(f, e *C.char) *C.char {
	return row("getenv-home-goroutine", f, e, onAnotherThread(libcGetenvHome))
}

//export check_dlopen_libc_by_name_goroutine
func check_dlopen_libc_by_name_goroutine(f, e *C.char) *C.char {
	return row("dlopen-libc-by-name-goroutine", f, e, onAnotherThread(dlopenRow("LIBC_NAME")))
}

// Not stunt's either: what Go needs of the process after a contained child's
// lockdown. A Go library starts threads (its scheduler's Ms) as goroutines
// block, and the library's cgo callbacks run on threads of its own. This
// starts 16 goroutines that each hold an OS thread of their own at once and
// counts the distinct threads: clone(CLONE_THREAD) must be allowed, which a
// contained child's seccomp filter does (it refuses clone as fork).
//
//export check_threads
func check_threads(f, e *C.char) *C.char { return row("threads", f, e, manyThreads) }

// ---- the host-checked rows: what only the host can judge ----

// host_spin never returns. The worker's cancellable call runs it on a thread
// of its own and ends the process at the host's CANCEL; its plain call runs
// it where it cannot hear a CANCEL (silent). A loop that allocates nothing
// and calls nothing, so nothing in Go's runtime gets a say.
//
//export host_spin
func host_spin() *C.char {
	for {
	}
}

// host_oversize returns one 1 MiB string, which the worker sends as one frame
// past the host's limit.
//
//export host_oversize
func host_oversize() *C.char {
	return C.CString(strings.Repeat("x", 1<<20))
}

// host_half_frame writes a frame's 4-byte header saying 64 bytes and 10 of
// them to the worker's socket to the host (AETHER_IPC_FD, which guest.launch
// puts in the worker's environment and Go copied at load), then stalls: the
// host must time out inside the frame.
//
//export host_half_frame
func host_half_frame() *C.char {
	fd := 0
	if _, err := fmt.Sscanf(os.Getenv("AETHER_IPC_FD"), "%d", &fd); err != nil || fd <= 0 {
		return C.CString("no AETHER_IPC_FD")
	}
	if _, err := syscall.Write(fd, []byte("\x00\x00\x00\x40CALL greet")); err != nil {
		return C.CString("write: " + err.Error())
	}
	time.Sleep(30 * time.Second)
	return C.CString("stalled")
}

var _ = unsafe.Pointer(nil)

func main() {} // c-shared still requires main()

// Package rows is stunt's catalogue (aether contrib/host/stunt), each row
// done the way a Go program would do it, and the judge that turns what
// happened into stunt's one line:
//
//	<check>: <outcome> (<detail>) - expected <expectation>: PASS|FAIL
//
// The checks binary (containment/checks.go, ways 1 to 3: a plain child) and
// the worker (worker/worker.go, way 4: a contained child) both run it, so a
// row is the same Go code whichever wall is around it. A refusal is what a
// sandbox gives: EPERM or EACCES from libc, the preload library or the
// kernel, or a host function's failure that starts "refused:". Anything
// else that goes wrong is an error, never a refusal, so it cannot pass for a
// sandbox at work.
//
// Go reaches the system in two ways, and which one a row takes decides what
// can hold it. The standard library (os, net, os/exec, syscall) makes
// system calls itself, with the syscall instruction, on Linux: libc is not
// involved, so a preload library that interposes libc never sees it. Only
// what goes through cgo (a C function: getenv, dlopen, fork, syscall(),
// shm_open) is libc, and only that is held by the preload. The rows say
// which they use.
package rows

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"syscall"
)

// Facts are what the host knew before the sandbox was in force.
type Facts struct {
	HomeSet  bool   // HOME is in the host's environment
	Sh       bool   // /bin/sh exists
	LibmPath string // the libraries the dlopen rows load
	LibcName string
	LibcPath string
}

// Caller calls a function the host mounted (std.guest's CALL) and returns
// the host's answer: kind VALUE (text is the JSON value), FAIL (text is the
// message) or CANCEL. Nil where there is no host to call: a plain child.
type Caller func(name, argsJSON string) (kind, text string)

// Env is what a row runs with.
type Env struct {
	Facts Facts
	Call  Caller
}

const fixture = "./stunt-fixture.txt"
const written = "./stunt-written.txt"

type result struct{ kind, detail string }

func succeeded(d string) result { return result{"succeeded", d} }
func refused(d string) result   { return result{"refused", d} }
func failed(d string) result    { return result{"error", d} }

func errno(err error) (syscall.Errno, bool) {
	var e syscall.Errno
	if errors.As(err, &e) {
		return e, true
	}
	return 0, false
}

// EPERM and EACCES are the same numbers on Linux, macOS and FreeBSD.
func byErr(err error, what string) result {
	if e, ok := errno(err); ok && (e == syscall.EPERM || e == syscall.EACCES) {
		return refused(what)
	}
	return failed(what)
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

// os.Open then read: Go's way to a file. On Linux os.Open is the openat
// system call made directly, not libc's open.
func openPath(path string) result {
	b, err := os.ReadFile(path)
	if err != nil {
		return byErr(err, "open: "+err.Error())
	}
	return succeeded(fmt.Sprintf("read %q", firstLine(string(b))))
}

func writeFile() result {
	err := os.WriteFile(written, []byte("written by go\n"), 0o644)
	if err != nil {
		return byErr(err, "open: "+err.Error())
	}
	return succeeded("wrote " + written)
}

// os.Getenv reads the copy of the environment the Go runtime took when the
// program started, before any sandbox, and never asks libc: this is stunt's
// env-cached row, and it is Go's only getenv without cgo. A missing HOME is
// a refusal only when the host had one: the contained child is given no
// environment.
func envCached(f Facts) result {
	if v, ok := os.LookupEnv("HOME"); ok && v != "" {
		return succeeded("HOME=" + v + " from os.Getenv")
	}
	if f.HomeSet {
		return refused("HOME is not in the environment; the host has it")
	}
	return failed("HOME was never in the environment")
}

// A non-blocking connect, judged as stunt judges it: EACCES at connect (the
// preload, Landlock) or EPERM/EACCES at socket (seccomp) is a refusal; EPERM
// at connect is a firewall's answer, an error; anything else (EINPROGRESS, a
// network error) means the call was let through. The syscall package's
// socket and connect, so the system calls Go's net package makes.
func connectTo(family int, sa syscall.Sockaddr) result {
	fd, err := syscall.Socket(family, syscall.SOCK_STREAM, 0)
	if err != nil {
		return byErr(err, "socket: "+err.Error())
	}
	defer syscall.Close(fd)
	if err := syscall.SetNonblock(fd, true); err != nil {
		return failed("setnonblock: " + err.Error())
	}
	err = syscall.Connect(fd, sa)
	if err == nil {
		return succeeded("connected")
	}
	e, ok := errno(err)
	switch {
	case ok && e == syscall.EACCES:
		return refused("connect: " + err.Error())
	case ok && e == syscall.EPERM:
		return failed("connect: " + err.Error() + " (a firewall's answer, not the sandbox's)")
	}
	return succeeded("let through: " + err.Error())
}

func connectV4() result {
	return connectTo(syscall.AF_INET, &syscall.SockaddrInet4{Port: 443, Addr: [4]byte{192, 0, 2, 1}})
}

func connectMapped() result {
	var a [16]byte
	a[10], a[11] = 0xff, 0xff
	a[12], a[13], a[14], a[15] = 192, 0, 2, 1
	return connectTo(syscall.AF_INET6, &syscall.SockaddrInet6{Port: 443, Addr: a})
}

// os/exec: the fork or clone and execve of os.StartProcess, which on Linux
// are system calls Go makes itself. Go has no libc system(); that is a
// cgo-only way, and the escape is the same as soon as one way is open.
func execSh(f Facts) result {
	err := exec.Command("/bin/sh", "-c", "exit 0").Run()
	if err == nil {
		return succeeded("os/exec: exit 0")
	}
	var ee *exec.ExitError
	if errors.As(err, &ee) {
		if ee.ExitCode() == 127 && f.Sh {
			return refused("os/exec: the shell could not be executed (exit 127)")
		}
		return failed(fmt.Sprintf("os/exec: exit %d", ee.ExitCode()))
	}
	return byErr(err, "os/exec: "+err.Error())
}

// A host function's answer: in a contained child a file comes as std.guest's
// answer to the call, {"granted": true, "text": ...} or {"granted": false,
// "why": ...}, and a FAIL is a failure. Without a host (a plain child) there
// is no function to call.
func hostStr(e Env, fn, arg string) result {
	if e.Call == nil {
		return failed("no host function " + fn + " (a plain child has none)")
	}
	args, _ := json.Marshal([]string{arg})
	kind, text := e.Call(fn, string(args))
	switch kind {
	case "FAIL":
		if strings.HasPrefix(text, "refused:") {
			return refused(text)
		}
		return failed(text)
	case "CANCEL":
		return failed("cancelled by host: " + text)
	}
	var obj struct {
		Granted *bool  `json:"granted"`
		Text    string `json:"text"`
		Why     string `json:"why"`
	}
	if json.Unmarshal([]byte(text), &obj) == nil && obj.Granted != nil {
		if !*obj.Granted {
			return refused("refused: " + obj.Why)
		}
		return succeeded(fmt.Sprintf("%s(%s) = %q", fn, arg, firstLine(obj.Text)))
	}
	var s string
	if json.Unmarshal([]byte(text), &s) != nil {
		return failed(fn + " answered " + text)
	}
	return succeeded(fmt.Sprintf("%s(%s) = %q", fn, arg, firstLine(s)))
}

func callbackGreet(e Env) result {
	if e.Call == nil {
		return failed("no host function greet (a plain child has none)")
	}
	kind, text := e.Call("greet", `["stunt"]`)
	var s string
	if kind != "VALUE" || json.Unmarshal([]byte(text), &s) != nil || s != "hello, stunt" {
		return failed(fmt.Sprintf("greet(\"stunt\") = %s %s", kind, text))
	}
	return succeeded(`greet("stunt") = "hello, stunt"`)
}

func callbackAdd(e Env) result {
	if e.Call == nil {
		return failed("no host function add (a plain child has none)")
	}
	kind, text := e.Call("add", `[2,40]`)
	if kind != "VALUE" || strings.TrimSpace(text) != "42" {
		return failed(fmt.Sprintf("add(2, 40) = %s %s", kind, text))
	}
	return succeeded("add(2, 40) = 42")
}

// A host function nobody registered: a Go program asks the host by name,
// and the host's answer is an error, not a crash.
func callbackUnregistered(e Env) result {
	if e.Call == nil {
		return failed("no host function nope (a plain child has none)")
	}
	kind, text := e.Call("nope", `[]`)
	if kind == "FAIL" || kind == "CANCEL" {
		return refused("no host function nope (" + text + ")")
	}
	return succeeded("nope() answered " + text)
}

// Line runs the row `name` and returns stunt's line for it, judged against
// `expect` (succeed, refused or error).
func Line(name, expect string, e Env) string {
	var r result
	switch name {
	case "open-etc-passwd":
		r = openPath("/etc/passwd")
	case "open-granted-file":
		r = openPath(fixture)
	case "write-granted-file":
		r = writeFile()
	case "getenv-home":
		r = libcGetenv(e.Facts)
	case "env-cached":
		r = envCached(e.Facts)
	case "connect-192.0.2.1":
		r = connectV4()
	case "connect-ipv4-mapped":
		r = connectMapped()
	case "exec-bin-sh":
		r = execSh(e.Facts)
	case "fork":
		r = forkOnce()
	case "dlopen-libm":
		r = load(e.Facts.LibmPath)
	case "dlopen-libc-by-name":
		r = load(e.Facts.LibcName)
	case "dlopen-libc-by-path":
		r = load(e.Facts.LibcPath)
	case "syscall-fn-openat":
		r = syscallFnOpenat()
	case "syscall-openat":
		r = syscallOpenat()
	case "syscall-socket":
		r = syscallSocket()
	case "binding-read-etc-passwd":
		r = hostStr(e, "read_file", "/etc/passwd")
	case "binding-read-granted-file":
		r = hostStr(e, "read_file", fixture)
	case "binding-getenv-home":
		r = hostStr(e, "getenv", "HOME")
	case "callback-greet":
		r = callbackGreet(e)
	case "callback-add":
		r = callbackAdd(e)
	case "callback-unregistered":
		r = callbackUnregistered(e)
	case "map-get":
		r = mapGet(e)
	case "map-set":
		r = mapSet(e)
	case "map-overwrite-input":
		r = mapOverwriteInput(e)
	default:
		return fmt.Sprintf("%s: not expressed in Go - expected %s: FAIL", name, expect)
	}
	pass := (expect == "succeed" && r.kind == "succeeded") ||
		(expect == "refused" && r.kind == "refused") ||
		(expect == "error" && r.kind == "error")
	verdict := "FAIL"
	if pass {
		verdict = "PASS"
	}
	return fmt.Sprintf("%s: %s (%s) - expected %s: %s", name, r.kind, r.detail, expect, verdict)
}

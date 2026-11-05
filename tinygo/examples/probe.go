// Six things a Go library can do to the system, each as an exported function
// that answers one short line: what README.md's "Four ways" table is made
// from. Each is done the way Go does it, which is the point: three of them
// (the files, the network, the shell) are system calls Go's own packages make
// without libc, and three go through libc by cgo (libc's getenv, dlopen) or
// are not a system call at all (os.Getenv).
//
//	CGO_ENABLED=1 go build -buildmode=c-shared -o libprobe.so probe.go
package main

/*
#cgo linux LDFLAGS: -ldl
#include <dlfcn.h>
#include <stdlib.h>
*/
import "C"

import (
	"net"
	"os"
	"os/exec"
	"time"
	"unsafe"
)

func line(ok bool, yes, no string) *C.char {
	if ok {
		return C.CString(yes)
	}
	return C.CString(no)
}

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// GetenvGo is os.Getenv: the copy of the environment Go took when the library
// was loaded.
//
//export GetenvGo
func GetenvGo(name *C.char) *C.char {
	v := os.Getenv(C.GoString(name))
	return line(v != "", v, "(empty)")
}

// GetenvLibc is libc's getenv, through cgo.
//
//export GetenvLibc
func GetenvLibc(name *C.char) *C.char {
	v := C.getenv(name)
	return line(v != nil, C.GoString(v), "(NULL)")
}

// OpenFile is os.ReadFile.
//
//export OpenFile
func OpenFile(path *C.char) *C.char {
	_, err := os.ReadFile(C.GoString(path))
	return line(err == nil, "opened", errText(err))
}

// RunShell is os/exec starting /bin/sh.
//
//export RunShell
func RunShell() *C.char {
	err := exec.Command("/bin/sh", "-c", "exit 0").Run()
	return line(err == nil, "ran /bin/sh", errText(err))
}

// Connect is net.Dial to 192.0.2.1:443 (TEST-NET-1: nothing answers there), so
// a connect that is let through times out, and a refused one fails at once.
//
//export Connect
func Connect() *C.char {
	c, err := net.DialTimeout("tcp", "192.0.2.1:443", 300*time.Millisecond)
	if err == nil {
		c.Close()
		return C.CString("connected")
	}
	if ne, ok := err.(net.Error); ok && ne.Timeout() {
		return C.CString("let through (timed out)")
	}
	return C.CString(err.Error())
}

// DlopenLibc is dlopen of libc by name, through cgo.
//
//export DlopenLibc
func DlopenLibc(name *C.char) *C.char {
	h := C.dlopen(name, C.RTLD_NOW)
	if h == nil {
		return C.CString("refused")
	}
	C.dlclose(h)
	return C.CString("loaded")
}

var _ unsafe.Pointer

func main() {}

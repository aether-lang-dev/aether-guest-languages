// go-worker: Go in a contained child, the guest's side of std.guest.
//
// A host starts this with guest.launch (sandbox.spawn_socket: Landlock and
// seccomp on Linux; refused on macOS and Windows, where nothing starts) and
// sends it the functions it may call (MOUNT) and a script (SCRIPT). Go is
// compiled, so there is no interpreter to hand a script to: the worker is
// the program, and the script is a short command list for the Go code
// compiled into it (this one holds the rows of stunt's catalogue). A worker
// of your own replaces runScript and keeps guestchild, the protocol:
//
//	say TEXT             SAY TEXT
//	call NAME [JSON]     CALL NAME JSON (default []), the answer SAY'd as
//	                     "VALUE <json>", "FAIL <message>"; a CANCEL ends the script
//	fact NAME VALUE      what the host knew before the sandbox (HOST_HOME_SET,
//	                     HOST_SH, LIBM_PATH, LIBC_NAME, LIBC_PATH)
//	row NAME EXPECT      run one row of stunt's catalogue, SAY its line
//	spin                 loop, looking for a CANCEL: ERROR cancelled by host
//	silent               loop and never look: the host must kill the worker
//	oversize             SAY one 1 MiB message: the host must refuse the frame
//	half-frame           write a frame's header and part of its payload, stall
//
// Lines are separated by newlines. The end of the script is DONE 0; a
// failure is ERROR.
//
// There is no counterpart of std.guest.child.run's lockdown inside the
// worker: Go cannot enter an Aether sandbox block, so the wall is the
// spawn's (Landlock, seccomp, and the preload where the binary is dynamic).
// A worker that needs nothing from disk after it starts (built with
// CGO_ENABLED=0: a static binary) is the best case for it. This one is built
// with cgo, since the catalogue's libc rows are cgo's.
//
// Usage (by a host): go-worker
package main

import (
	"fmt"
	"os"
	"strings"
	"syscall"
	"time"

	"github.com/aether-lang-dev/aether-guest-languages/go/guestchild"
	"github.com/aether-lang-dev/aether-guest-languages/go/rows"
)

func main() {
	h, err := guestchild.Open()
	if err != nil {
		fmt.Println(err)
		os.Exit(2)
	}
	if h.ReadScript() != nil {
		os.Exit(1)
	}
	if err := runScript(h); err != "" {
		h.Error(err)
	} else {
		h.Done(0)
	}
}

// runScript returns "" when the script ended, else the message of the ERROR.
func runScript(h *guestchild.Host) string {
	var facts rows.Facts
	call := func(name, args string) (string, string) {
		r := h.CallHost(name, args)
		return r.Kind, r.Text
	}
	for _, line := range strings.Split(h.Source, "\n") {
		if line == "" {
			continue
		}
		if why := h.Poll(); why != "" {
			return "cancelled by host: " + why
		}
		cmd, rest := cut(line)
		switch cmd {
		case "say":
			h.Say(rest)
		case "call":
			name, args := cut(rest)
			if args == "" {
				args = "[]"
			}
			r := h.CallHost(name, args)
			if r.Kind == "CANCEL" {
				return "cancelled by host: " + r.Text
			}
			h.Say(r.Kind + " " + r.Text)
		case "fact":
			name, v := cut(rest)
			switch name {
			case "HOST_HOME_SET":
				facts.HomeSet = v == "1"
			case "HOST_SH":
				facts.Sh = v == "1"
			case "LIBM_PATH":
				facts.LibmPath = v
			case "LIBC_NAME":
				facts.LibcName = v
			case "LIBC_PATH":
				facts.LibcPath = v
			}
		case "row":
			name, expect := cut(rest)
			h.Say(rows.Line(name, expect, rows.Env{Facts: facts, Call: call}))
			if why := h.Cancelled(); why != "" {
				return "cancelled by host: " + why
			}
		case "spin":
			for {
				if why := h.Poll(); why != "" {
					return "cancelled by host: " + why
				}
				time.Sleep(time.Millisecond)
			}
		case "silent":
			// Never looks: the host's CANCEL goes unanswered, and it kills us.
			for {
				time.Sleep(time.Millisecond)
			}
		case "oversize":
			h.Say(strings.Repeat("x", 1<<20))
		case "half-frame":
			// A frame's 4-byte header saying 64 bytes, 10 of them, then nothing.
			syscall.Write(h.FD(), []byte("\x00\x00\x00\x40CALL greet"))
			time.Sleep(30 * time.Second)
		default:
			return "unknown command " + cmd
		}
	}
	return ""
}

func cut(s string) (string, string) {
	if i := strings.IndexByte(s, ' '); i >= 0 {
		return s[:i], s[i+1:]
	}
	return s, ""
}

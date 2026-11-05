// Package guestchild is the worker's side of std.guest in Go: the protocol
// that aether's std/guest/child speaks for an Aether worker, for a worker
// written in Go. The protocol is std.ipc frames (a 4-byte big-endian length,
// then that many bytes, at most 64 KiB) of "VERB text" with JSON arguments;
// std/guest/README.md in the ae release has every message.
//
// A host starts the worker with guest.launch (sandbox.spawn_socket: Landlock
// and seccomp on Linux, and nothing on macOS or Windows). The worker has the
// socket as a descriptor, named by AETHER_IPC_FD (3), and uses it to
//
//	ReadScript   take the host's MOUNTs, PROBE, LOGLEVEL and SCRIPT
//	Say          tell the host the script printed something (SAY)
//	CallHost     call a function the host mounted (CALL) and wait for its answer
//	Poll         look, without waiting, for a CANCEL
//	Done, Error  end (DONE, ERROR); the worker exits after
//
// What std/guest/child.run adds for an Aether worker, entering sandbox.enforce
// with no grants before the script's first step, has no counterpart here:
// a Go program cannot enter an Aether sandbox block. The wall around a Go
// worker is the one the host's spawn put there, and it holds the Go
// runtime's raw system calls, which a preload library does not (README.md,
// "4. Contained child").
package guestchild

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"syscall"
)

// MaxFrame is the largest frame the host accepts.
const MaxFrame = 65536

// MaxFile is the largest file a FILE reply is read up to.
const MaxFile = 65536

// Mount is a host function the script may call.
type Mount struct {
	Name     string
	Min, Max int
}

// Reply is a host's answer to CallHost: Kind VALUE (Text is the JSON value),
// FAIL (the message) or CANCEL (the reason).
type Reply struct{ Kind, Text string }

// Host is the channel to the host.
type Host struct {
	fd       int
	Source   string  // the SCRIPT
	Mounts   []Mount // the MOUNTs
	Probe    bool
	LogLevel int
	cancel   string
}

// Open finds the socket the host started this worker with.
func Open() (*Host, error) {
	v := os.Getenv("AETHER_IPC_FD")
	if v == "" {
		return nil, errors.New("not started by a host: AETHER_IPC_FD is not set")
	}
	fd, err := strconv.Atoi(v)
	if err != nil || fd < 0 {
		return nil, errors.New("not started by a host: AETHER_IPC_FD is " + v)
	}
	return &Host{fd: fd, LogLevel: -1}, nil
}

// FD is the descriptor of the socket, for a worker that wants to misbehave
// on purpose (the half-frame row).
func (h *Host) FD() int { return h.fd }

func (h *Host) readN(n int) ([]byte, error) {
	buf := make([]byte, n)
	for got := 0; got < n; {
		m, err := syscall.Read(h.fd, buf[got:])
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return nil, err
		}
		if m == 0 {
			return nil, io.ErrUnexpectedEOF
		}
		got += m
	}
	return buf, nil
}

// frame reads one frame. The host going away ends the worker: there is
// nobody left to report to.
func (h *Host) frame() string {
	hdr, err := h.readN(4)
	if err != nil {
		os.Exit(1)
	}
	n := int(hdr[0])<<24 | int(hdr[1])<<16 | int(hdr[2])<<8 | int(hdr[3])
	if n > MaxFrame {
		os.Exit(1)
	}
	if n == 0 {
		return ""
	}
	body, err := h.readN(n)
	if err != nil {
		os.Exit(1)
	}
	return string(body)
}

func (h *Host) write(b []byte) error {
	for len(b) > 0 {
		n, err := syscall.Write(h.fd, b)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return err
		}
		b = b[n:]
	}
	return nil
}

// tell sends one frame (a frame over MaxFrame is sent as it is, and the host
// refuses it); a host that has gone away ends the worker.
func (h *Host) tell(text string) {
	n := len(text)
	b := make([]byte, 0, 4+n)
	b = append(b, byte(n>>24), byte(n>>16), byte(n>>8), byte(n))
	b = append(b, text...)
	if h.write(b) != nil {
		os.Exit(1)
	}
}

func splitVerb(frame string) (verb, rest string) {
	if i := strings.IndexByte(frame, ' '); i >= 0 {
		return frame[:i], frame[i+1:]
	}
	return frame, ""
}

// ReadScript takes the host's frames up to SCRIPT. A frame that breaks the
// protocol is answered with ERROR, as std.guest.child does, and returned.
func (h *Host) ReadScript() error {
	for {
		verb, rest := splitVerb(h.frame())
		switch verb {
		case "MOUNT":
			f := strings.Fields(rest)
			if len(f) != 3 {
				return h.protocol("MOUNT is not 'name min max'")
			}
			lo, e1 := strconv.Atoi(f[1])
			hi, e2 := strconv.Atoi(f[2])
			if e1 != nil || e2 != nil || lo < 0 || hi < lo {
				return h.protocol("MOUNT is not 'name min max'")
			}
			h.Mounts = append(h.Mounts, Mount{f[0], lo, hi})
		case "PROBE":
			h.Probe = true
		case "LOGLEVEL":
			lv, err := strconv.Atoi(strings.TrimSpace(rest))
			if err != nil || lv < -1 || lv > 5 {
				return h.protocol("LOGLEVEL out of range")
			}
			h.LogLevel = lv
		case "SCRIPT":
			h.Source = rest
			return nil
		default:
			return h.protocol("unexpected '" + verb + "' before SCRIPT")
		}
	}
}

func (h *Host) protocol(why string) error {
	h.tell("ERROR protocol: " + why)
	return errors.New("protocol: " + why)
}

// IsMounted says whether the host mounted name.
func (h *Host) IsMounted(name string) bool {
	for _, m := range h.Mounts {
		if m.Name == name {
			return true
		}
	}
	return false
}

// Say: the script printed text.
func (h *Host) Say(text string) { h.tell("SAY " + text) }

// Audit: a line for the host's audit log.
func (h *Host) Audit(line string) { h.tell("AUDIT " + line) }

// Log sends a message at level 0 (trace) to 5 (fatal) with tags, a JSON
// object, when the host's threshold lets it through.
func (h *Host) Log(level int, tags, text string) {
	if h.LogLevel < 0 || level < h.LogLevel || level > 5 {
		return
	}
	h.tell(fmt.Sprintf("LOG %d %d %s %s", level, len(tags), tags, text))
}

// Done: the script ended with code. The worker exits after it.
func (h *Host) Done(code int) { h.tell("DONE " + strconv.Itoa(code)) }

// Error: the script failed, or was cancelled ("cancelled by host: ...").
func (h *Host) Error(why string) { h.tell("ERROR " + why) }

// Cancelled is the reason the host cancelled, or "".
func (h *Host) Cancelled() string { return h.cancel }

// Poll looks, without waiting, for a CANCEL: "" to go on, or the reason.
func (h *Host) Poll() string {
	for h.cancel == "" {
		var b [1]byte
		n, _, err := syscall.Recvfrom(h.fd, b[:], syscall.MSG_PEEK|syscall.MSG_DONTWAIT)
		if err == syscall.EINTR {
			continue
		}
		if err == syscall.EAGAIN || err == syscall.EWOULDBLOCK {
			return ""
		}
		if err != nil || n == 0 {
			os.Exit(1)
		}
		verb, rest := splitVerb(h.frame())
		switch verb {
		case "CANCEL":
			h.cancel = reason(rest)
		case "LOGLEVEL":
			h.cancel = h.setLevel(rest)
		default:
			h.cancel = "protocol: unexpected '" + verb + "' while the script runs"
		}
	}
	return h.cancel
}

func reason(rest string) string {
	if rest == "" {
		return "no reason given"
	}
	return rest
}

func (h *Host) setLevel(rest string) string {
	lv, err := strconv.Atoi(strings.TrimSpace(rest))
	if err != nil || lv < -1 || lv > 5 {
		return "protocol: LOGLEVEL out of range"
	}
	h.LogLevel = lv
	return ""
}

// CallHost calls the mounted function name with args, a JSON array, and
// waits for the answer. After a cancel it sends nothing and returns it again.
func (h *Host) CallHost(name, args string) Reply {
	if h.cancel != "" {
		return Reply{"CANCEL", h.cancel}
	}
	h.tell("CALL " + name + " " + args)
	for {
		verb, rest := splitVerb(h.frame())
		switch verb {
		case "LOGLEVEL":
			if why := h.setLevel(rest); why != "" && h.cancel == "" {
				h.cancel = why
			}
		case "REPLY":
			return h.answered("VALUE", rest)
		case "FAIL":
			if rest == "" {
				rest = "host function failed"
			}
			return h.answered("FAIL", rest)
		case "FILE":
			return h.answered("VALUE", h.takeFile())
		case "CANCEL":
			if h.cancel == "" {
				h.cancel = reason(rest)
			}
			return Reply{"CANCEL", h.cancel}
		default:
			if h.cancel == "" {
				h.cancel = "protocol: unexpected '" + verb + "' for a reply"
			}
			return Reply{"CANCEL", h.cancel}
		}
	}
}

func (h *Host) answered(kind, text string) Reply {
	if h.cancel != "" {
		return Reply{"CANCEL", h.cancel}
	}
	return Reply{kind, text}
}

func granted(ok bool, text, why string) string {
	if ok {
		b, _ := json.Marshal(map[string]interface{}{"granted": true, "text": text})
		return string(b)
	}
	b, _ := json.Marshal(map[string]interface{}{"granted": false, "why": why})
	return string(b)
}

// takeFile: FILE's descriptor rides on the next byte (SCM_RIGHTS). The frame
// before it was read byte-exact, so this recvmsg gets that byte and no more.
func (h *Host) takeFile() string {
	var b [1]byte
	oob := make([]byte, syscall.CmsgSpace(4*8))
	var n, oobn int
	for {
		var err error
		n, oobn, _, _, err = syscall.Recvmsg(h.fd, b[:], oob, 0)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return granted(false, "", err.Error())
		}
		break
	}
	if n == 0 && oobn == 0 {
		os.Exit(1)
	}
	msgs, err := syscall.ParseSocketControlMessage(oob[:oobn])
	if err != nil {
		return granted(false, "", err.Error())
	}
	fd := -1
	for _, m := range msgs {
		fds, err := syscall.ParseUnixRights(&m)
		if err != nil {
			continue
		}
		for _, f := range fds {
			if fd < 0 {
				fd = f
			} else {
				syscall.Close(f)
			}
		}
	}
	if fd < 0 {
		return granted(false, "", "no descriptor came with FILE")
	}
	f := os.NewFile(uintptr(fd), "granted")
	defer f.Close()
	buf := make([]byte, MaxFile+1)
	total := 0
	for total < len(buf) {
		m, err := f.Read(buf[total:])
		total += m
		if err != nil {
			break
		}
	}
	if total > MaxFile {
		return granted(false, "", fmt.Sprintf("file larger than %d bytes", MaxFile))
	}
	return granted(true, string(buf[:total]), "")
}

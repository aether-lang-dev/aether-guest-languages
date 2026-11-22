//go:build cgo

package rows

/*
#cgo linux LDFLAGS: -ldl
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/mman.h>

// dlopen: 0 when it loaded; 1 when it did not, with dlerror()'s message in
// msg (empty where the preload refused without one).
static int ghl_dlopen(const char *path, char *msg, size_t n) {
    void *h = dlopen(path, RTLD_NOW);
    if (h) { dlclose(h); return 0; }
    const char *e = dlerror();
    snprintf(msg, n, "%s", e ? e : "");
    return 1;
}

// fork, the child exiting at once: the child's exit status, or -errno when
// the fork itself failed.
static int ghl_fork_once(void) {
    pid_t p = fork();
    if (p < 0) return -errno;
    if (p == 0) _exit(0);
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -errno;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

// libc's syscall(), which a preload can interpose (Go's own syscall package
// does not call it). errno comes back through *err.
static long ghl_libc_syscall(long nr, long a, long b, long c, long d, int *err) {
#if defined(__linux__)
    errno = 0;
    long r = syscall(nr, a, b, c, d);
    *err = errno;
    return r;
#else
    (void)nr; (void)a; (void)b; (void)c; (void)d;
    *err = ENOSYS;
    return -1;
#endif
}

static int ghl_shm_open(const char *name, int rw) {
    return shm_open(name, rw ? O_RDWR : O_RDONLY, 0);
}
*/
import "C"

import (
	"os"
	"strings"
	"syscall"
	"unsafe"
)

const haveCgo = true

// getenv(3), the libc call: what stunt's getenv-home row is.
func libcGetenv(f Facts) result {
	name := C.CString("HOME")
	defer C.free(unsafe.Pointer(name))
	v := C.getenv(name)
	if v != nil {
		return succeeded("HOME=" + C.GoString(v))
	}
	if f.HomeSet {
		return refused("libc getenv gave NULL; HOME is in the environment")
	}
	return failed("HOME is unset")
}

// fork(2) through libc, the child exiting at once.
func forkOnce() result {
	r := int(C.ghl_fork_once())
	if r == 0 {
		return succeeded("the forked child ran")
	}
	if r < 0 {
		return byErr(syscall.Errno(-r), "fork: "+syscall.Errno(-r).Error())
	}
	return failed("the forked child: status " + itoa(r))
}

// dlopen(3). A dlopen that fails on its own leaves a dlerror() message; the
// preload's refusal returns NULL without one (or says "Permission denied").
func load(path string) result {
	if path == "" {
		return failed("the host found no library to load")
	}
	cp := C.CString(path)
	defer C.free(unsafe.Pointer(cp))
	var msg [512]C.char
	if C.ghl_dlopen(cp, &msg[0], C.size_t(len(msg))) == 0 {
		return succeeded("loaded " + path)
	}
	m := C.GoString(&msg[0])
	if m == "" || strings.Contains(m, "Permission denied") || strings.Contains(m, "Operation not permitted") || strings.Contains(m, "refused by the sandbox") {
		if m == "" {
			m = "no dlerror() message"
		}
		return refused("dlopen: " + m + " (the preload's refusal)")
	}
	return failed("dlopen: " + m)
}

// libcSyscall4 is syscall(2) through libc.
func libcSyscall4(nr, a, b, c, d uintptr) (int64, syscall.Errno) {
	var e C.int
	r := C.ghl_libc_syscall(C.long(nr), C.long(a), C.long(b), C.long(c), C.long(d), &e)
	return int64(r), syscall.Errno(e)
}

// ---- the shared map, as the bridge hands it to a child -----------------
//
// AETHER_MAP_SHM names a POSIX shared memory object in Aether's layout 2
// (runtime/aether_shared_map.h, Aether 0.935.0), every integer a
// little-endian uint32: the magic "AESM", a word that is always 0, the
// version 2, frozen_count, entry_count, then per entry key_len, value_len,
// the key's bytes and the value's. The first frozen_count entries are the
// host's inputs; the rest are outputs the host reads back when the child
// exits. Keys and values are bytes by their length: a NUL in either is
// carried. A block in any other shape is refused whole, as Aether's reader
// refuses one: nothing of it is read.

const (
	shmMagic      = "AESM"
	shmVersion    = 2
	shmHeader     = 20
	shmMaxEntries = 256
)

type shmMap struct {
	frozen  int
	entries [][2]string
}

func shmOpen(rw bool) (int, error) {
	name := os.Getenv("AETHER_MAP_SHM")
	if name == "" {
		return -1, syscall.ENOENT
	}
	cn := C.CString(name)
	defer C.free(unsafe.Pointer(cn))
	flag := C.int(0)
	if rw {
		flag = 1
	}
	fd, err := C.ghl_shm_open(cn, flag)
	if fd < 0 {
		return -1, err
	}
	return int(fd), nil
}

func le32(b []byte) uint32 {
	return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
}

func put32(b []byte, v uint32) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}

// shmDecode reads a layout-2 block, or refuses it (an error, nothing read):
// a short block, no magic, a nonzero second word, another version, more
// than 256 entries, an entry running past the end, or a nonzero byte after
// the last entry (zero bytes there are allowed: macOS rounds a shm object up
// to a page).
func shmDecode(data []byte) (*shmMap, error) {
	n := len(data)
	if n < shmHeader || string(data[0:4]) != shmMagic || le32(data[4:]) != 0 || le32(data[8:]) != shmVersion {
		return nil, errString("the shared map block is not layout 2 (no AESM magic, zero word and version 2): refused")
	}
	frozen := le32(data[12:])
	count := le32(data[16:])
	if count > shmMaxEntries || frozen > count {
		return nil, errString("the shared map block counts more entries than it may: refused")
	}
	m := &shmMap{frozen: int(frozen)}
	pos := shmHeader
	for i := uint32(0); i < count; i++ {
		if n-pos < 8 {
			return nil, errString("the shared map block ends inside an entry: refused")
		}
		kl, vl := int(le32(data[pos:])), int(le32(data[pos+4:]))
		pos += 8
		if kl > n-pos || vl > n-pos-kl {
			return nil, errString("an entry of the shared map block runs past its end: refused")
		}
		k := string(data[pos : pos+kl])
		v := string(data[pos+kl : pos+kl+vl])
		pos += kl + vl
		m.entries = append(m.entries, [2]string{k, v})
	}
	for _, c := range data[pos:] {
		if c != 0 {
			return nil, errString("the shared map block has bytes after its last entry: refused")
		}
	}
	return m, nil
}

// encode is the block for m, in layout 2.
func (m *shmMap) encode() []byte {
	buf := []byte(shmMagic)
	buf = put32(buf, 0)
	buf = put32(buf, shmVersion)
	buf = put32(buf, uint32(m.frozen))
	buf = put32(buf, uint32(len(m.entries)))
	for _, e := range m.entries {
		buf = put32(buf, uint32(len(e[0])))
		buf = put32(buf, uint32(len(e[1])))
		buf = append(buf, e[0]...)
		buf = append(buf, e[1]...)
	}
	return buf
}

func shmLoad() (*shmMap, error) {
	fd, err := shmOpen(false)
	if err != nil {
		return nil, err
	}
	defer syscall.Close(fd)
	var st syscall.Stat_t
	if err := syscall.Fstat(fd, &st); err != nil {
		return nil, err
	}
	if st.Size < shmHeader {
		return nil, errString("the shared map block is shorter than its header: refused")
	}
	data, err := syscall.Mmap(fd, 0, int(st.Size), syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil, err
	}
	defer syscall.Munmap(data)
	// The host can still write to the block: decode a copy.
	return shmDecode(append([]byte(nil), data...))
}

func (m *shmMap) get(key string) (string, bool) {
	for _, e := range m.entries {
		if e[0] == key {
			return e[1], true
		}
	}
	return "", false
}

// put is the runtime's rule: an input the host froze is read-only (false);
// anything else is stored.
func (m *shmMap) put(key, value string) (bool, error) {
	for i, e := range m.entries {
		if e[0] == key {
			if i < m.frozen {
				return false, nil
			}
			m.entries[i][1] = value
			return true, m.store()
		}
	}
	m.entries = append(m.entries, [2]string{key, value})
	return true, m.store()
}

func (m *shmMap) store() error {
	if len(m.entries) > shmMaxEntries {
		return errString("the shared map holds at most 256 keys")
	}
	buf := m.encode()
	fd, err := shmOpen(true)
	if err != nil {
		return err
	}
	defer syscall.Close(fd)
	if err := syscall.Ftruncate(fd, int64(len(buf))); err != nil {
		return err
	}
	data, err := syscall.Mmap(fd, 0, len(buf), syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return err
	}
	copy(data, buf)
	return syscall.Munmap(data)
}

var shm *shmMap

func mapView() (*shmMap, error) {
	if shm == nil {
		m, err := shmLoad()
		if err != nil {
			return nil, err
		}
		shm = m
	}
	return shm, nil
}

func mapGetValue(e Env, key string) (string, bool, error) {
	if e.Call != nil {
		kind, text := e.Call("aether_map_get", `["`+key+`"]`)
		if kind != "VALUE" {
			return "", false, errString(kind + ": " + text)
		}
		var s *string
		if err := jsonUnmarshal(text, &s); err != nil {
			return "", false, err
		}
		if s == nil {
			return "", false, nil
		}
		return *s, true, nil
	}
	m, err := mapView()
	if err != nil {
		return "", false, err
	}
	v, ok := m.get(key)
	return v, ok, nil
}

func mapPutValue(e Env, key, value string) error {
	if e.Call != nil {
		args := jsonArgs(key, value)
		kind, text := e.Call("aether_map_put", args)
		if kind != "VALUE" {
			return errString(kind + ": " + text)
		}
		return nil
	}
	m, err := mapView()
	if err != nil {
		return err
	}
	_, err = m.put(key, value)
	return err
}

func mapGet(e Env) result {
	v, ok, err := mapGetValue(e, "input")
	if err != nil {
		return failed("input: " + err.Error())
	}
	if !ok {
		return failed("input is not in the map")
	}
	if v != "stunt-in" {
		return failed("input = " + quote(v))
	}
	// Through the shared memory block (layout 2) the host also put a key and
	// a value with a NUL in them: both arrive whole.
	if e.Call == nil {
		nv, ok, err := mapGetValue(e, "nul\x00key")
		if err != nil || !ok || nv != "nul\x00value" {
			return failed("the input with a NUL in its key and value read back as " + quote(nv))
		}
		return succeeded(`input = "stunt-in", and "nul\0key" = "nul\0value" whole`)
	}
	return succeeded(`input = "stunt-in"`)
}

func mapSet(e Env) result {
	if err := mapPutValue(e, "output", "from-go"); err != nil {
		return failed("put output: " + err.Error())
	}
	// Through the block, a key and a value with a NUL too (the host checks
	// them in its own map: go/containment/host.ae).
	if e.Call == nil {
		if err := mapPutValue(e, "out\x00key", "out\x00value"); err != nil {
			return failed("put out\\0key: " + err.Error())
		}
	}
	v, ok, err := mapGetValue(e, "output")
	if err != nil {
		return failed("output: " + err.Error())
	}
	if !ok || v != "from-go" {
		return failed("output read back as " + quote(v))
	}
	return succeeded(`output = "from-go"`)
}

// The write is the host's to refuse: in a contained child its answer to
// aether_map_put is false; in a plain child the runtime's rule above, and
// the host checks its own map afterwards (containment/host.ae).
func mapOverwriteInput(e Env) result {
	if err := mapPutValue(e, "input", "overwritten"); err != nil {
		return failed("put input: " + err.Error())
	}
	v, ok, err := mapGetValue(e, "input")
	if err != nil {
		return failed("input: " + err.Error())
	}
	if ok && v == "stunt-in" {
		return refused(`input is still "stunt-in" (read back)`)
	}
	return succeeded("input read back as " + quote(v))
}

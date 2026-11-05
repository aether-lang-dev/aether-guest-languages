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
	if m == "" || strings.Contains(m, "Permission denied") || strings.Contains(m, "Operation not permitted") {
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
// AETHER_MAP_SHM names a POSIX shared memory object: frozen_count (4 bytes,
// little endian), then key\0value\0 pairs and a final \0. The first
// frozen_count pairs are the host's inputs; the rest are outputs the host
// reads back when the child exits.

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
	if st.Size < 5 {
		return &shmMap{}, nil
	}
	data, err := syscall.Mmap(fd, 0, int(st.Size), syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil, err
	}
	defer syscall.Munmap(data)
	m := &shmMap{frozen: int(data[0]) | int(data[1])<<8 | int(data[2])<<16 | int(data[3])<<24}
	pos := 4
	next := func() string {
		end := pos
		for end < len(data) && data[end] != 0 {
			end++
		}
		s := string(data[pos:end])
		pos = end + 1
		return s
	}
	for pos < len(data)-1 && data[pos] != 0 {
		k := next()
		v := next()
		m.entries = append(m.entries, [2]string{k, v})
	}
	return m, nil
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
	buf := []byte{byte(m.frozen), byte(m.frozen >> 8), byte(m.frozen >> 16), byte(m.frozen >> 24)}
	for _, e := range m.entries {
		buf = append(buf, e[0]...)
		buf = append(buf, 0)
		buf = append(buf, e[1]...)
		buf = append(buf, 0)
	}
	buf = append(buf, 0)
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
	return succeeded(`input = "stunt-in"`)
}

func mapSet(e Env) result {
	if err := mapPutValue(e, "output", "from-go"); err != nil {
		return failed("put output: " + err.Error())
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

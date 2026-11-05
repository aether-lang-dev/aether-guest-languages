//go:build !cgo

package rows

// Without cgo Go has no libc: these rows are not expressible (left out in
// containment/lang.sh when CGO_ENABLED=0 or there is no C compiler).

const haveCgo = false

const noCgo = "needs cgo (a C compiler and CGO_ENABLED=1)"

func libcGetenv(Facts) result { return failed("getenv(3): " + noCgo) }
func forkOnce() result        { return failed("fork(2): " + noCgo) }
func load(string) result      { return failed("dlopen(3): " + noCgo) }
func libcSyscall4(nr, a, b, c, d uintptr) (int64, syscallErrno) {
	return -1, syscallErrno(38)
}
func mapGet(Env) result            { return failed("shm_open(3): " + noCgo) }
func mapSet(Env) result            { return failed("shm_open(3): " + noCgo) }
func mapOverwriteInput(Env) result { return failed("shm_open(3): " + noCgo) }

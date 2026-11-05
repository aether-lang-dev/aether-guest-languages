package rows

import (
	"encoding/json"
	"strconv"
	"syscall"
)

type syscallErrno = syscall.Errno

type errString string

func (e errString) Error() string { return string(e) }

func itoa(n int) string { return strconv.Itoa(n) }

func quote(s string) string { return strconv.Quote(s) }

func jsonUnmarshal(text string, v interface{}) error { return json.Unmarshal([]byte(text), v) }

func jsonArgs(a ...string) string {
	b, _ := json.Marshal(a)
	return string(b)
}

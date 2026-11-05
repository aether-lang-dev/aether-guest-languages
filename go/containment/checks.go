// checks: the rows of stunt's catalogue (aether contrib/host/stunt), each
// done the way a Go program would do it, as a program the Go bridge starts.
// Usage:
//
//	checks <row> <expectation>
//
// Prints stunt's line for the row and exits 0 when it matched. The facts the
// host found before the sandbox was in force come in the environment
// (GHL_HOME_SET, GHL_SH, GHL_LIBM, GHL_LIBC_NAME, GHL_LIBC_PATH): the bridge
// gives a child no arguments, so the row and its expectation arrive as
// GHL_ROW and GHL_EXPECT when there is no command line. containment/host.ae
// sets them and calls go.run_sandboxed (or go.run) on this binary; the
// shared map, when the host made one, is AETHER_MAP_SHM.
//
// The rows are package rows, shared with the worker (worker/worker.go), which
// runs the same Go code in a contained child.
package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/aether-lang-dev/aether-guest-languages/go/rows"
)

func main() {
	row, expect := os.Getenv("GHL_ROW"), os.Getenv("GHL_EXPECT")
	if len(os.Args) == 3 {
		row, expect = os.Args[1], os.Args[2]
	}
	if row == "" || expect == "" {
		fmt.Println("usage: checks <row> <expectation>   (or GHL_ROW and GHL_EXPECT)")
		os.Exit(2)
	}
	facts := rows.Facts{
		HomeSet:  os.Getenv("GHL_HOME_SET") == "1",
		Sh:       os.Getenv("GHL_SH") == "1",
		LibmPath: os.Getenv("GHL_LIBM"),
		LibcName: os.Getenv("GHL_LIBC_NAME"),
		LibcPath: os.Getenv("GHL_LIBC_PATH"),
	}
	line := rows.Line(row, expect, rows.Env{Facts: facts})
	fmt.Println(line)
	if !strings.HasSuffix(line, ": PASS") {
		os.Exit(1)
	}
}

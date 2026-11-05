// worker: Go in a contained child (way 4 of README.md): the guest's side of
// std.guest, with package guestchild doing the protocol. It calls the host's
// greet, says what came back, and then tries to read a file it was not
// given.
package main

import (
	"os"

	"github.com/aether-lang-dev/aether-guest-languages/go/guestchild"
)

func main() {
	h, err := guestchild.Open()
	if err != nil {
		println(err.Error())
		os.Exit(2)
	}
	if h.ReadScript() != nil {
		os.Exit(1)
	}
	// h.Source is the script the host sent; this worker ignores it.
	r := h.CallHost("greet", `["Go"]`)
	h.Say("the host answered " + r.Kind + " " + r.Text)
	if _, err := os.ReadFile("/etc/passwd"); err != nil {
		h.Say(err.Error())
	} else {
		h.Say("/etc/passwd: read it")
	}
	h.Done(0)
}

// hello: the Go program the first three ways of README.md start. It says
// what it can reach: the environment it was started with, and a file it was
// not given.
package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Println("hello from Go, HOME =", os.Getenv("HOME"))
	if _, err := os.ReadFile("/etc/passwd"); err != nil {
		fmt.Println("/etc/passwd:", err)
	} else {
		fmt.Println("/etc/passwd: read it")
	}
}

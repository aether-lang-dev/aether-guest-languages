//go:build !linux

package main

// Off Linux the raw-syscall and fork rows are not expressed: they are
// child-only rows in stunt, and a contained child exists only on Linux.

func syscallFnOpenat(facts) result { return failed("not expressed off Linux") }
func syscallOpenat(facts) result   { return failed("not expressed off Linux") }
func syscallSocket(facts) result   { return failed("not expressed off Linux") }
func forkOnce(facts) result        { return failed("not expressed off Linux") }
func manyThreads(facts) result     { return failed("not expressed off Linux") }

//go:build !windows

package main

import "os/exec"

// hideProcess is a no-op on non-Windows platforms.
func hideProcess(cmd *exec.Cmd) {}

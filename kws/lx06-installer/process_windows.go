//go:build windows

package main

import (
	"os/exec"
	"syscall"
)

const createNoWindow = 0x08000000

// hideProcess suppresses the console window that a GUI (windowsgui) host
// spawns when it launches a console binary (pnputil, update.exe, …). The LX04
// installer proved this combo hides both the console flash and any child GUI.
func hideProcess(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{
		HideWindow:    true,
		CreationFlags: createNoWindow,
	}
}

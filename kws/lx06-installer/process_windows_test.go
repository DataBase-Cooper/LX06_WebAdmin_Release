//go:build windows

package main

import (
	"os/exec"
	"testing"
)

func TestHideProcessSuppressesConsole(t *testing.T) {
	cmd := exec.Command("cmd.exe", "/c", "exit", "0")
	hideProcess(cmd)
	if cmd.SysProcAttr == nil {
		t.Fatal("expected SysProcAttr to be set")
	}
	if !cmd.SysProcAttr.HideWindow {
		t.Error("expected HideWindow=true")
	}
	if cmd.SysProcAttr.CreationFlags&createNoWindow == 0 {
		t.Error("expected CREATE_NO_WINDOW flag")
	}
}

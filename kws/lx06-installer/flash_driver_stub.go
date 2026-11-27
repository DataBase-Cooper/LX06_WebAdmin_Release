//go:build !windows

package main

import (
	"errors"
	"os"
	"time"
)

func openDriverRecovery() error { return errors.New("仅支持在 Windows 中打开恢复设置") }

const (
	driverWorkerFlag   = "--driver-worker"
	driverCatName      = "WorldCup_Device.cat"
	driverPnputilToken = "worldcup"
)

var driverInfName = "WorldCup_Device.inf"

// Non-Windows placeholder: driver install requires the Windows PnP stack.
func driverInstalledNow() bool { return false }

func writeDriverStatus(statusFile string, payload map[string]any) {
	_ = os.WriteFile(statusFile, []byte("{}"), 0o644)
}

func driverWorkerMain(driverDir, statusFile string) int {
	return 1
}

func (a *App) installDriver() map[string]any {
	return map[string]any{"ok": false, "error": "驱动安装仅支持 Windows"}
}

func (a *App) refreshDriverStatus() {
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	a.drvInstalled = false
	a.drvCheckedAt = time.Now()
	a.drvMsg = "驱动检测仅支持 Windows"
}

func (a *App) setDriverMsg(m string) {
	a.drvMu.Lock()
	a.drvMsg = m
	a.drvMu.Unlock()
}

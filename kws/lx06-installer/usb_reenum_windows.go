//go:build windows

package main

import (
	"golang.org/x/sys/windows"
)

// usbRescan re-enumerates the whole USB device tree via the root devnode
// (CM_Reenumerate_DevNode with DEVINST 0). Cheap and safe; used to recover
// from half-enumerated Amlogic download-mode devices (identify answers but
// the control pipe fails) without the user replugging the cable.
func usbRescan() {
	_, _, _ = cmReenumerateDevNode.Call(0, 0)
}

var (
	setupapiDLL        = windows.NewLazySystemDLL("setupapi.dll")
	cmReenumerateDevNode = setupapiDLL.NewProc("CM_Reenumerate_DevNode")
)

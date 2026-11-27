//go:build windows

package main

import "syscall"

// enableProcessDPI prevents Windows from bitmap-scaling the embedded WebView2
// on high-DPI monitors. It must run before the first HWND is created.
func enableProcessDPI() {
	user32 := syscall.NewLazyDLL("user32.dll")
	setContext := user32.NewProc("SetProcessDpiAwarenessContext")
	if err := setContext.Find(); err == nil {
		// Per-monitor-v2 awareness so the window and native text stay crisp at
		// non-100% scaling (matches the LX04 tool).
		if ok, _, _ := setContext.Call(^uintptr(3)); ok != 0 {
			return
		}
	}
	// Windows 8.1 and older fallback: system-DPI aware is still crisp rather
	// than the default bitmap-scaled, DPI-unaware application mode.
	setSystemAware := user32.NewProc("SetProcessDPIAware")
	if err := setSystemAware.Find(); err == nil {
		_, _, _ = setSystemAware.Call()
	}
}

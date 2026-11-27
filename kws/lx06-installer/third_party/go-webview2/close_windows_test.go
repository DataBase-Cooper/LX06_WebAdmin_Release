//go:build windows

package webview2

import (
	"github.com/jchv/go-webview2/internal/w32"
	"testing"
)

func TestNativeCloseCallsVetoBeforeDestroy(t *testing.T) {
	const hwnd = uintptr(0x123)
	called := false
	w := &webview{onClose: func(got uintptr) bool {
		called = true
		if got != hwnd {
			t.Fatal(got)
		}
		return false
	}}
	setWindowContext(hwnd, w)
	defer func() { windowContextSync.Lock(); delete(windowContext, hwnd); windowContextSync.Unlock() }()
	if wndproc(hwnd, w32.WMClose, 0, 0) != 0 || !called {
		t.Fatal("WM_CLOSE bypassed veto")
	}
}

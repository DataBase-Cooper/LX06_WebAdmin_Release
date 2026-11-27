//go:build !windows

package main

// usbRescan is a no-op off Windows (re-enumeration only makes sense for the
// desktop USB host where update.exe runs).
func usbRescan() {}

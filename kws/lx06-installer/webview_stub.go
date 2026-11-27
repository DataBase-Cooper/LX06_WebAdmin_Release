//go:build !windows

package main

func runWebView(a *App) error {
	// Non-Windows build fallback: serve the API headless.
	return serveLocal(a, "127.0.0.1:0")
}

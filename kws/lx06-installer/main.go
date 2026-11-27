package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

//go:embed web/index.html
var webFS embed.FS

var version = "0.1.0-dev"
var buildDate = "未标记日期"

func releaseStamp() string {
	return version
}

const windowTitle = "小爱音箱Pro LX06 一键刷机工具"

func appRoot() string {
	if exe, err := os.Executable(); err == nil {
		return filepath.Dir(exe)
	}
	wd, err := os.Getwd()
	if err != nil {
		return "."
	}
	return wd
}

func defaultDataDir() string {
	portable := filepath.Join(appRoot(), "data")
	if os.MkdirAll(portable, 0o755) == nil {
		if probe, err := os.CreateTemp(portable, ".write-check-"); err == nil {
			name := probe.Name()
			_ = probe.Close()
			_ = os.Remove(name)
			return portable
		}
	}
	// A standard user may run a complete package from Program Files or a
	// read-only share. UAC/status/log/firmware work files must remain writable.
	if cache, err := os.UserCacheDir(); err == nil {
		return filepath.Join(cache, "LX06Installer")
	}
	return filepath.Join(os.TempDir(), "LX06Installer")
}

// indexArg returns the index of arg in os.Args, or -1.
func indexArg(arg string) int {
	for i, a := range os.Args {
		if a == arg {
			return i
		}
	}
	return -1
}

func serveLocal(a *App, listen string) error {
	listener, err := net.Listen("tcp", listen)
	if err != nil {
		return err
	}
	server := &http.Server{Handler: a.routes()}
	go func() { _ = server.Serve(listener) }()
	select {}
}

func main() {
	enableProcessDPI()
	startAt := time.Now()

	// Primary elevated driver worker: no WebView or second main application.
	// Only this short-lived worker needs administrator privileges.
	if i := indexArg(driverWorkerFlag); i >= 0 {
		rest := os.Args[i+1:]
		if len(rest) >= 2 {
			code := driverWorkerMain(rest[0], rest[1])
			os.Exit(code)
		}
		fmt.Fprintln(os.Stderr, "driver-worker requires: <driverDir> <statusFile>")
		os.Exit(2)
	}

	listen := flag.String("listen", "127.0.0.1:0", "local API listen address for headless automation")
	dataDir := flag.String("data-dir", defaultDataDir(), "local app data directory")
	headless := flag.Bool("headless", false, "serve JSON API without opening the embedded window")
	showVersion := flag.Bool("version", false, "print version")
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return
	}
	a, err := NewApp(appRoot(), *dataDir)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	a.logf("启动: NewApp 完成，耗时 %d ms（进程启动后）", time.Since(startAt).Milliseconds())
	// 设备日志接收端（0.0.0.0:87）：端口被占用只警告，不阻塞启动与刷机。
	startLogReceiver(a)
	a.logf("启动: 日志接收端就绪，耗时 %d ms（进程启动后）", time.Since(startAt).Milliseconds())
	// Ensure the embedded UI is reachable; fail fast if the asset is missing.
	if _, err := fs.Stat(webFS, "web/index.html"); err != nil {
		fmt.Fprintln(os.Stderr, "embedded UI missing:", err)
		os.Exit(1)
	}
	if *headless {
		if err := serveLocal(a, *listen); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	if err := runWebView(a); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

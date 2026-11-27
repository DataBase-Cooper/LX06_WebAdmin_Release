//go:build windows

package main

import (
	"fmt"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"
	"unsafe"

	webview2 "github.com/jchv/go-webview2"
)

const (
	initialWindowWidth  = 1600
	initialWindowHeight = 987
	minimumWindowWidth  = 600
	minimumWindowHeight = 600
	pageZoomDefault     = 100.0
)

var pageZoomPresets = [...]float64{75, 100, 125, 150, 175, 200}

func normalizeNativePageZoom(percent float64) float64 {
	if math.IsNaN(percent) || math.IsInf(percent, 0) {
		return pageZoomDefault
	}
	nearest := pageZoomPresets[0]
	for _, candidate := range pageZoomPresets[1:] {
		if math.Abs(candidate-percent) < math.Abs(nearest-percent) {
			nearest = candidate
		}
	}
	return nearest
}

// openInDefaultBrowser opens u via the ShellExecute "open" verb
// (cmd /c start, window hidden). Never fails the app — best effort only.
func openInDefaultBrowser(u string) error {
	cmd := exec.Command("cmd.exe", "/c", "start", "", u)
	hideProcess(cmd)
	return cmd.Start()
}

func sigintCh() <-chan struct{} {
	ch := make(chan struct{})
	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
		<-sig
		close(ch)
	}()
	return ch
}

func runWebView(a *App) error {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return err
	}
	server := &http.Server{Handler: a.routes()}
	go func() { _ = server.Serve(listener) }()
	defer server.Close()

	a.logf("启动: WebView2 窗口创建开始")
	tv := time.Now()
	w := webview2.NewWithOptions(webview2.WebViewOptions{
		Debug: false,
		OnClose: func(hwnd uintptr) bool {
			if a.requestWindowClose() {
				return true
			}
			text := "正在刷机，请完成后再切换页面或关闭软件。请保持 USB 和电源连接。"
			a.drvMu.Lock()
			if a.drvBusy {
				text = "驱动安装尚未结束，请处理 Windows 管理员授权窗口，并等待安装结果后再关闭软件。"
			}
			a.drvMu.Unlock()
			message, _ := syscall.UTF16PtrFromString(text)
			title, _ := syscall.UTF16PtrFromString("操作尚未完成")
			_, _, _ = syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW").Call(
				hwnd, uintptr(unsafe.Pointer(message)), uintptr(unsafe.Pointer(title)), 0x40)
			return false
		},
		AutoFocus: true,
		DataPath:  filepath.Join(a.dataDir, "webview2-data"),
		WindowOptions: webview2.WindowOptions{
			Title:  windowTitle,
			Width:  initialWindowWidth,
			Height: initialWindowHeight,
			IconId: 1,
			Center: false,
		},
	})
	a.logf("启动: WebView2 窗口创建完成，耗时 %d ms", time.Since(tv).Milliseconds())
	if w == nil {
		// No WebView2 Runtime (rare on Win11, possible on stock Win10):
		// fall back to the default browser so the tool still works. The
		// headless server keeps running until the user closes the process.
		u := "http://" + listener.Addr().String() + "/?v=" + url.QueryEscape(version)
		fmt.Fprintln(os.Stderr, "未检测到 WebView2 Runtime，已改用系统默认浏览器打开（本机可能为 Win10 未安装 Runtime）。")
		fmt.Fprintln(os.Stderr, "工具在后台继续运行，请勿关闭进程；按 Ctrl+C 或任务管理器结束 LX06Installer.exe 即可退出。")
		fmt.Fprintln(os.Stderr, u)
		_ = openInDefaultBrowser(u)
		<-sigintCh()
		return nil
	}
	defer w.Destroy()
	a.drvMu.Lock()
	a.nativeWindow = uintptr(w.Window())
	a.drvMu.Unlock()
	w.SetSize(minimumWindowWidth, minimumWindowHeight, webview2.HintMin)
	if err := w.Bind("setNativePageZoom", func(percent float64) (float64, error) {
		percent = normalizeNativePageZoom(percent)
		return percent, w.SetZoomFactor(percent / 100)
	}); err != nil {
		return err
	}
	w.Navigate("http://" + listener.Addr().String() + "/?v=" + url.QueryEscape(version))
	w.Run()
	return nil
}

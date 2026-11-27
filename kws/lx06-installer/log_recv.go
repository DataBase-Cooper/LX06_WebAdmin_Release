package main

// 日志接收端：刷机工具运行期间在 0.0.0.0:87 监听，接收 LX06 设备
// （S70 看门狗，88/89 双死异常重启前）POST 上来的系统日志。
// 对应设备端：hooks/robust/lx06-watchdog.sh 的 send_crash_log()。
//
// 端口 87 为系统推荐端口：刷机期把「本机内网 IPv4:87」烘焙进镜像
// （/lx06-log.conf，见 flash_bake.go），设备首启自动播种到
// /data/lx06-log.conf，用户无需任何设置。另外的机器打开本刷机包、
// 刷过的设备异常重启时，日志就发到那台机器。
//
// 端口被占用时接收端不启动：只提示「端口被占用，日志接收受影响，
// 不影响刷机」（用户定案），刷机流程照常。
//
// 收到的日志落盘 data/device-logs/device-<时间>.log，并在工具日志区
// 提示一行；GET / 返回运行状态（浏览器可直接打开验证）。

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const logRecvPort = 87

const logRecvMaxBytes = 4 << 20 // 单条日志上限 4MB（远大于设备端 log_max_kb=1024）

// logReceiver 设备日志接收端（端口 87）。
type logReceiver struct {
	a    *App
	dir  string
	srv  *http.Server
	list net.Listener
}

// startLogReceiver 启动日志接收端；端口被占用只警告不报错。
func startLogReceiver(a *App) {
	d := &logReceiver{a: a, dir: filepath.Join(a.dataDir, "device-logs")}
	if err := os.MkdirAll(d.dir, 0o755); err != nil {
		a.logf("[日志接收] 日志目录创建失败（不影响接收前状态）: %v", err)
	}
	l, err := net.Listen("tcp4", fmt.Sprintf("0.0.0.0:%d", logRecvPort))
	if err != nil {
		a.logf("[日志接收] 端口 %d 被占用，日志接收受影响（不影响刷机）。如需接收设备日志，请关闭占用 %d 端口的程序后重启本工具", logRecvPort, logRecvPort)
		return
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/log", d.handleLog)
	mux.HandleFunc("/", d.handleStatus)
	d.list = l
	d.srv = &http.Server{Handler: mux}
	go func() { _ = d.srv.Serve(l) }()
	// 防火墙放行放后台：netsh 在某些机器（管理员/防火墙规则多/杀软介入）
	// 要好几秒，且它是控制台程序，不 hide 就会在桌面弹出黑窗口
	// （2026-10-18 用户反馈：启动卡很久 + 弹黑窗口）。
	go allowFirewallInbound(a)
	a.logf("[日志接收] 已启动 0.0.0.0:%d（本机内网 IPv4: %s）。刷机后的设备异常重启前会自动把系统日志发到本机", logRecvPort, detectLanIPv4())
}

// allowFirewallInbound 尽力放行 Windows 防火墙 TCP 87 入站（设备在局域网
// 内 POST 到本机，防火墙拦掉就收不到）。先删同名规则再添加（幂等）。
// 非 Windows / 未提权 / netsh 失败：只警告，不影响接收端与刷机。
// 调用方在后台 goroutine 里跑：netsh 是控制台程序（须 hideProcess 防黑
// 窗口），且在某些机器上耗时数秒（2026-10-18 用户反馈：启动卡很久）。
func allowFirewallInbound(a *App) {
	if runtime.GOOS != "windows" {
		return
	}
	runNetsh := func(args ...string) (string, error) {
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		cmd := exec.CommandContext(ctx, "netsh", args...)
		hideProcess(cmd)
		out, err := cmd.CombinedOutput()
		return string(out), err
	}
	_, _ = runNetsh("advfirewall", "firewall", "delete", "rule", "name=LX06 日志接收 87")
	out, err := runNetsh("advfirewall", "firewall", "add", "rule",
		"name=LX06 日志接收 87", "dir=in", "action=allow", "protocol=TCP",
		fmt.Sprintf("localport=%d", logRecvPort), "enable=yes")
	if err != nil {
		a.logf("[日志接收] 防火墙放行未自动添加（%v %s）。若收不到设备日志且本机防火墙开启，请手动放行 TCP %d 入站",
			err, strings.TrimSpace(string(out)), logRecvPort)
		return
	}
	a.logf("[日志接收] Windows 防火墙已放行 TCP %d 入站", logRecvPort)
}

// stopLogReceiver 关闭接收端（进程退出路径用；正常由进程退出兜底）。
func (d *logReceiver) stop() {
	if d.srv != nil {
		_ = d.srv.Close()
	}
}

// handleLog 接收设备 POST 的系统日志（busybox wget --post-file，Content-Type 为
// application/octet-stream 或无头）。
func (d *logReceiver) handleLog(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	if r.Method != http.MethodPost {
		w.WriteHeader(http.StatusMethodNotAllowed)
		fmt.Fprintln(w, "POST /log — LX06 设备日志接收端")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, logRecvMaxBytes))
	if err != nil {
		w.WriteHeader(http.StatusBadRequest)
		fmt.Fprintln(w, "读取日志内容失败")
		return
	}
	name := "device-" + time.Now().Format("20060102-150405") + ".log"
	if err := os.WriteFile(filepath.Join(d.dir, name), body, 0o644); err != nil {
		w.WriteHeader(http.StatusInternalServerError)
		fmt.Fprintln(w, "日志落盘失败")
		return
	}
	d.a.logf("[日志接收] 收到设备系统日志 %d 字节（%s），已存 data/device-logs/%s", len(body), r.RemoteAddr, name)
	fmt.Fprintln(w, "ok")
}

// handleStatus 运行状态页（浏览器打开 http://<刷机机>:87/ 可自检）。
func (d *logReceiver) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	fmt.Fprintf(w, "LX06 日志接收端运行中（端口 %d）。\n", logRecvPort)
	fmt.Fprintf(w, "本机内网 IPv4: %s\n", detectLanIPv4())
	fmt.Fprintf(w, "刷机包会把该地址烘焙进设备；设备异常重启前自动 POST /log。\n")
}

// detectLanIPv4 返回本机首选内网 IPv4（设备走 wlan0 与本机同网段）。
// 优先级：192.168.x > 10.x / 172.16-31.x；取不到返回空串（fail-open：
// 设备端收不到配置就不发送，刷机与系统功能不受影响）。
func detectLanIPv4() string {
	best, bestScore := "", 0
	ifaces, err := net.Interfaces()
	if err != nil {
		return ""
	}
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := ifc.Addrs()
		if err != nil {
			continue
		}
		for _, ad := range addrs {
			ipn, ok := ad.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipn.IP.To4()
			if ip4 == nil {
				continue
			}
			s := ip4.String()
			score := 0
			switch {
			case strings.HasPrefix(s, "192.168."):
				score = 3
			case strings.HasPrefix(s, "10."):
				score = 2
			case ip4[0] == 172 && ip4[1] >= 16 && ip4[1] <= 31:
				score = 2
			default:
				continue // 公网/链路本地等不参与
			}
			if score > bestScore {
				best, bestScore = s, score
			}
		}
	}
	return best
}

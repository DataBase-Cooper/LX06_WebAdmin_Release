package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"time"
)

// FwPackage describes one flashable firmware package directory.
type FwPackage struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Desc     string `json:"desc"`
	SSH      bool   `json:"ssh"`
	Dir      string `json:"-"`
	Firmware string `json:"firmware"`
	Size     int64  `json:"size"`
	SHA256   string `json:"sha256"`
	Present  bool   `json:"present"`
	Version  string `json:"version"` // 固件版本（version.txt，如 user-1.94.13-release）
}

const maxLogLines = 2000

// versionRe 从 SOURCE.txt 首行（如 "LX06 official 1.94.13 (unpatched ...)"）提取版本号。
var versionRe = regexp.MustCompile(`\b(\d+\.\d+\.\d+)\b`)

// App is the in-memory state of the LX06 flasher backend.
type App struct {
	root      string
	dataDir   string
	version   string
	startedAt time.Time

	packages []FwPackage

	fwMu    sync.Mutex
	sshInfo string // dropbear/port note for the SSH package

	flashMu sync.Mutex
	flash   *flashState
	closing bool // guarded by flashMu; closes the start-versus-window-close race

	logMu   sync.Mutex
	logs    []string
	logDone chan struct{}
	logFile *os.File // 落盘日志（data/flasher.log），GUI 关闭后可复盘

	drvMu             sync.Mutex
	drvInstalled      bool
	drvCheckedAt      time.Time
	drvBusy           bool
	drvMsg            string
	drvPhase          string
	drvError          string
	drvStartedAt      time.Time
	nativeWindow      uintptr // guarded by drvMu
	drvCheckInFlight  bool
	drvRebootRequired bool
}

func NewApp(root, dataDir string) (*App, error) {
	// 归一化为绝对路径：刷机烘焙临时目录建在 dataDir 下，而 mksquashfsAppend
	// 会把子进程工作目录切到 tools/（相对 dataDir 就解析不到了，2026-10-08 实测）。
	if abs, err := filepath.Abs(dataDir); err == nil {
		dataDir = abs
	}
	a := &App{
		root:      root,
		dataDir:   dataDir,
		version:   releaseStamp(),
		startedAt: time.Now(),
		logDone:   make(chan struct{}),
	}
	a.logf("LX06 一键刷机工具启动 v%s", a.version)
	a.logf("运行目录: %s", root)

	// 日志落盘：data/flasher.log（追加）。GUI 窗口关掉后也能复盘刷机过程。
	// 必须先建目录：全新解压的发布包没有 data/，否则 OpenFile 失败、
	// 首次运行的启动日志全部丢失（2026-10-18 V20261018.3 实测）。
	_ = os.MkdirAll(dataDir, 0o755)
	if lf, err := os.OpenFile(filepath.Join(dataDir, "flasher.log"),
		os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644); err == nil {
		a.logFile = lf
		a.logf("日志落盘: %s", filepath.Join(dataDir, "flasher.log"))
	}

	// Firmware packages: official + official-ssh (LX04-style package dirs).
	specs := []struct {
		dir  string
		id   string
		name string
		desc string
		ssh  bool
	}{
		{"official", "official", "官方固件", "官方 1.94.13，不含 SSH/Web88", false},
		{"official-ssh", "official-ssh", "官方固件 + SSH", "官方系统 + SSH89/Web88", true},
	}
	for _, s := range specs {
		p := FwPackage{
			ID:       s.id,
			Name:     s.name,
			Desc:     s.desc,
			SSH:      s.ssh,
			Dir:      filepath.Join(root, s.dir),
			Firmware: "root.squashfs",
		}
		if v, err := os.ReadFile(filepath.Join(p.Dir, "version.txt")); err == nil {
			p.Version = strings.TrimSpace(string(v))
		} else if v, err := os.ReadFile(filepath.Join(p.Dir, "SOURCE.txt")); err == nil {
			// version.txt 缺失时回退解析 SOURCE.txt 首行（"LX06 official 1.94.13 ..."）
			if m := versionRe.FindStringSubmatch(string(v)); m != nil {
				p.Version = m[1]
			}
		}
		if fi, err := os.Stat(filepath.Join(p.Dir, p.Firmware)); err == nil {
			p.Present = true
			p.Size = fi.Size()
		}
		a.packages = append(a.packages, p)
		state := "就绪"
		if !p.Present {
			state = "缺失 (root.squashfs 未找到)"
		}
		a.logf("固件包 %s: %s", s.id, state)
		// SHA256 仅供 UI 展示（刷机完整性由 preflightPackage 按 SHA256SUMS.txt
		// 独立重算）。放后台算：慢盘/杀软介入时 70MB 哈希要数秒，不能挡在
		// 窗口创建之前（2026-10-18 用户反馈：启动卡很久）。
		if p.Present {
			idx := len(a.packages) - 1
			go func(idx int, path string) {
				if h, err := sha256File(path); err == nil {
					a.fwMu.Lock()
					a.packages[idx].SHA256 = h
					a.fwMu.Unlock()
				}
			}(idx, filepath.Join(p.Dir, p.Firmware))
		}
	}

	// Flash toolchain sanity check.
	if fi, err := os.Stat(a.updateExe()); err != nil {
		a.logf("警告: 未找到刷机工具 update.exe (%s)", a.updateExe())
	} else {
		a.logf("刷机工具就绪: %s (%d 字节)", filepath.Base(a.updateExe()), fi.Size())
	}

	// Initial driver check (cached).
	// 后台执行：pnputil /enum-drivers 在一些机器上要数秒，不能挡在窗口
	// 创建之前（2026-10-18 用户反馈：启动卡很久）。UI 首帧显示「检测中」，
	// 后续 /api/status 轮询自动拿到结果。
	go a.refreshDriverStatus()
	return a, nil
}

// ---- flash toolchain paths ----
func (a *App) binDir() string    { return filepath.Join(a.root, "flash-tool", "bin") }
func (a *App) updateExe() string { return filepath.Join(a.binDir(), "update.exe") }
func (a *App) driverDir() string { return filepath.Join(a.root, "flash-tool", "drivers") }

// ---- logging ring buffer ----
func (a *App) logf(format string, args ...any) {
	line := fmt.Sprintf("[%s] %s", time.Now().Format("15:04:05"), fmt.Sprintf(format, args...))
	a.logMu.Lock()
	a.logs = append(a.logs, line)
	if len(a.logs) > maxLogLines {
		a.logs = a.logs[len(a.logs)-maxLogLines:]
	}
	if a.logFile != nil {
		a.logFile.WriteString(line + "\n")
		a.logFile.Sync()
	}
	a.logMu.Unlock()
}

func (a *App) recentLogs(n int) []string {
	a.logMu.Lock()
	defer a.logMu.Unlock()
	if n > len(a.logs) {
		n = len(a.logs)
	}
	out := make([]string, n)
	copy(out, a.logs[len(a.logs)-n:])
	return out
}

// ---- driver status (cached, re-checked on demand / after install) ----
func (a *App) driverInstalled() bool {
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	return a.drvInstalled && !a.drvBusy && !a.drvRebootRequired
}

func (a *App) driverSnapshot() map[string]any {
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	elapsed := 0
	if a.drvBusy && !a.drvStartedAt.IsZero() {
		elapsed = int(time.Since(a.drvStartedAt).Seconds())
	}
	return map[string]any{
		"installed":      a.drvInstalled,
		"busy":           a.drvBusy,
		"message":        a.drvMsg,
		"driverDir":      a.driverDir(),
		"phase":          a.drvPhase,
		"error":          a.drvError,
		"elapsedSeconds": elapsed,
		"checking":       a.drvCheckInFlight,
		"rebootRequired": a.drvRebootRequired,
	}
}

// ---- HTTP routes ----
func (a *App) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", a.handleIndex)
	mux.HandleFunc("/api/status", a.handleStatus)
	mux.HandleFunc("/api/packages", a.handlePackages)
	mux.HandleFunc("/api/logs", a.handleLogs)
	mux.HandleFunc("/api/flash/start", a.handleFlashStart)
	mux.HandleFunc("/api/flash/cancel", a.handleFlashCancel)
	mux.HandleFunc("/api/flash/wipe", a.handleFlashWipe)
	mux.HandleFunc("/api/ssh/defaults", a.handleSSHDefaults)
	mux.HandleFunc("/api/ssh/apply", a.handleSSHApply)
	mux.HandleFunc("/api/init/apps", a.handleInitApps)
	mux.HandleFunc("/api/driver/install", a.handleDriverInstall)
	mux.HandleFunc("/api/driver/recovery", a.handleDriverRecovery)
	mux.HandleFunc("/api/driver/status", a.handleDriverStatus)
	return mux
}

func (a *App) writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func (a *App) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	b, err := webFS.ReadFile("web/index.html")
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = w.Write(b)
}

func (a *App) flashSnapshot() map[string]any {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	f := a.flash
	if f == nil {
		return map[string]any{"active": false}
	}
	return map[string]any{
		"active":      f.active,
		"phase":       f.phase,
		"wipeData":    f.wipeData,
		"wipeDecided": f.wipeDecided,
		"wipeResult":  f.wipeResult,
		"wipeNotice":  f.wipeNotice,
		"step":        f.step,
		"stepTotal":   f.stepTotal,
		"stepLabel":   f.stepLabel,
		"progress":    f.progress,
		"message":     f.message,
		"error":       f.err,
		"done":        f.done,
		"packageID":   f.pkgID,
		"startedAt":   f.startedAt.Format(time.RFC3339),
		"finishedAt":  f.finishedAt.Format(time.RFC3339),
	}
}

// maybeRefreshDriver re-probes the driver store at most every 8s, so a status
// poll (every ~2s) doesn't spawn pnputil each time.
func (a *App) maybeRefreshDriver() {
	a.flashMu.Lock()
	active := a.flash != nil && a.flash.active
	a.flashMu.Unlock()
	if active {
		return
	} // avoid PnP enumeration competing with an active flash
	a.drvMu.Lock()
	if time.Since(a.drvCheckedAt) < 8*time.Second {
		a.drvMu.Unlock()
		return
	}
	a.drvMu.Unlock()
	go a.refreshDriverStatus()
}

func (a *App) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	a.maybeRefreshDriver()
	a.writeJSON(w, http.StatusOK, map[string]any{
		"version":   a.version,
		"buildDate": buildDate,
		"startedAt": a.startedAt.Format(time.RFC3339),
		"driver":    a.driverSnapshot(),
		"flash":     a.flashSnapshot(),
		"packages":  a.packageSnapshot(),
		"initApps":  a.loadInitApps(),
	})
}

func (a *App) handlePackages(w http.ResponseWriter, r *http.Request) {
	a.writeJSON(w, http.StatusOK, a.packageSnapshot())
}

// packageSnapshot returns a copy of the package list (fwMu guards it because
// the SHA256 fields are filled in by background goroutines after startup).
func (a *App) packageSnapshot() []FwPackage {
	a.fwMu.Lock()
	defer a.fwMu.Unlock()
	out := make([]FwPackage, len(a.packages))
	copy(out, a.packages)
	return out
}

func (a *App) handleLogs(w http.ResponseWriter, r *http.Request) {
	a.writeJSON(w, http.StatusOK, map[string]any{"lines": a.recentLogs(500)})
}

func (a *App) handleFlashStart(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	var req struct {
		PackageID string `json:"packageID"`
		// V19 零配置：客户端仍可携带 ssh 字段（旧页面兼容），但一律忽略——
		// 烘焙固定走 89 / DB_COOPER / 仅局域网（fixedSSHConfig）。
		WipeData bool `json:"wipeData"` // 清空 DATA 分区（仅设备识别前有效；等待设备步骤也可经 /api/flash/wipe 更改）
		SSH      struct {
			Port     int    `json:"port"`
			Password string `json:"password"`
			LANOnly  bool   `json:"lanOnly"`
		} `json:"ssh"`
	}
	_ = json.NewDecoder(r.Body).Decode(&req)
	pkg, ok := a.findPackage(req.PackageID)
	if !ok {
		a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "未知固件包: " + req.PackageID})
		return
	}
	if !pkg.Present {
		a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "固件文件缺失: " + filepath.Join(pkg.Dir, pkg.Firmware)})
		return
	}
	if _, err := os.Stat(a.updateExe()); err != nil {
		a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "刷机工具缺失，请确认 flash-tool/bin/update.exe 存在"})
		return
	}
	// V19 零配置：SSH 烘焙固定 22 / DB_COOPER / 仅局域网，客户端字段一律不采用。
	// 用户刷完后经 HTTP 控制台(88) 改密，改密即时生效、无需重启任何服务。
	sshCfg := fixedSSHConfig()
	if pkg.SSH {
		a.logf("SSH 零配置烘焙: 端口 89 / 密码 DB_COOPER / 仅局域网（刷后经 http://<音箱IP>:88 改密）")
	}
	initApps := a.loadInitApps()
	a.logInitApps(initApps)
	if pkg.SSH && len(initApps) == 0 {
		a.logf("开机自启: 未发现 init-apps/ 自启程序（可把要开机运行的 .sh/.bin 放入 init-apps/ 后刷机）")
	}
	if req.WipeData {
		a.logf("选项: 清空 DATA 分区（设备识别后执行，出厂数据重置）")
	}
	// 刷机前 preflight：SSH 烘焙依赖的工具/镜像缺失就立刻报错，
	// 不要等用户接好 USB、设备识别完才在烘焙阶段失败（白等几十秒）。
	if pkg.SSH {
		if _, serr := os.Stat(a.mksquashfsExe()); serr != nil {
			a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "mksquashfs 工具缺失（tools/mksquashfs.exe 未随包携带），无法烘焙 SSH 配置。"})
			return
		}
		if _, berr := os.Stat(a.bakedBasePath(pkg)); berr != nil {
			a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "SSH base 镜像缺失（official-ssh/root.squashfs 未随包携带），无法烘焙 SSH 配置。"})
			return
		}
	}
	if !a.beginFlash(pkg.ID, sshCfg, initApps, req.WipeData) {
		a.writeJSON(w, http.StatusConflict, map[string]any{"error": "已有刷机任务在进行中"})
		return
	}
	go a.runFlash(pkg)
	a.writeJSON(w, http.StatusOK, map[string]any{"ok": true, "packageID": pkg.ID, "initApps": len(initApps)})
}

func (a *App) handleFlashCancel(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	if !a.requestFlashCancel() {
		a.writeJSON(w, http.StatusConflict, map[string]any{"error": "没有可取消的刷机任务"})
		return
	}
	a.writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// handleFlashWipe toggles the "wipe DATA partition" option before device
// identification (the waiting-for-device step). Body: {"wipeData": true|false}
func (a *App) handleFlashWipe(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	var req struct {
		WipeData bool `json:"wipeData"`
	}
	_ = json.NewDecoder(r.Body).Decode(&req)
	if err := a.requestFlashWipe(req.WipeData); err != nil {
		a.writeJSON(w, http.StatusConflict, map[string]any{"error": err.Error()})
		return
	}
	a.logf("清空 DATA 选项: %v", req.WipeData)
	a.writeJSON(w, http.StatusOK, map[string]any{"ok": true, "wipeData": req.WipeData})
}

// handleSSHDefaults returns the last used SSH options (or defaults) so the H5
// form can prefill before the user flashes.
func (a *App) handleSSHDefaults(w http.ResponseWriter, _ *http.Request) {
	cfg, ok := a.loadLastSSHConfig()
	resp := map[string]any{"ssh": SSHConfig{Port: sshDefaultPort}, "hasPrevious": ok}
	if ok {
		resp["ssh"] = cfg
	}
	a.writeJSON(w, http.StatusOK, resp)
}

// handleSSHApply applies SSH port/password/LAN-only to an already-flashed
// device right now (without a USB flash) — e.g. correcting a previous run.
// Body: {ip?, old_port?, old_password?, ssh:{ip,port,password,lanOnly}}
func (a *App) handleSSHApply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	var req struct {
		IP      string    `json:"ip"`
		OldPort int       `json:"old_port"`
		OldPass string    `json:"old_password"`
		NewPass string    `json:"new_password"`
		NewPort int       `json:"new_port"`
		LANOnly bool      `json:"lan_only"`
		SSH     SSHConfig `json:"ssh"` // optional nested form (H5 uses this)
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": "请求体不是合法 JSON"})
		return
	}
	cfg := req.SSH
	if cfg.Password == "" {
		cfg = SSHConfig{IP: req.IP, Port: req.NewPort, Password: req.NewPass, LANOnly: req.LANOnly}
	}
	cfg, err := validateSSHConfig(cfg, true)
	if err != nil {
		a.writeJSON(w, http.StatusBadRequest, map[string]any{"error": err.Error()})
		return
	}
	oldPass := strings.TrimSpace(req.OldPass)
	if oldPass == "" {
		oldPass = sshFreshPassword
	}
	oldPort := req.OldPort
	if oldPort == 0 {
		oldPort = sshDefaultPort
	}
	if !a.beginSSHApply(cfg) {
		a.writeJSON(w, http.StatusConflict, map[string]any{"error": "已有刷机任务在进行中"})
		return
	}
	go a.runSSHApply(cfg, oldPort, oldPass)
	a.writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// beginSSHApply starts a non-flash SSH apply run (2 steps, no device write).
func (a *App) beginSSHApply(cfg SSHConfig) bool {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	if a.closing || (a.flash != nil && a.flash.active) {
		return false
	}
	a.flash = &flashState{
		active:    true,
		phase:     phaseSSHWait,
		stepTotal: 3,
		progress:  0,
		cancel:    make(chan struct{}),
		ssh:       cfg,
	}
	a.flash.startedAt = time.Now()
	a.flash.pkgID = "official-ssh"
	return true
}

// runSSHApply connects to an already-running device (no USB involved) and
// applies the SSH options, reusing the same state machine as a flash run.
func (a *App) runSSHApply(cfg SSHConfig, oldPort int, oldPass string) {
	a.logf("===== 手动应用 SSH 配置 → %s =====", cfg.IP)
	a.setFlash(func(f *flashState) {
		f.step, f.stepLabel, f.phase, f.progress = 1, "等待设备上线", phaseSSHWait, 20
	})
	ok := false
	deadline := time.Now().Add(2 * time.Minute)
	for time.Now().Before(deadline) {
		a.flashMu.Lock()
		cancelled := a.flash == nil || a.flash.cancelOK
		a.flashMu.Unlock()
		if cancelled {
			a.finishFlash(phaseCancel, "已取消")
			return
		}
		ok = sshReachable(cfg.IP, oldPort, oldPass)
		if ok {
			break
		}
		time.Sleep(3 * time.Second)
	}
	if !ok {
		a.finishFlash(phaseError, fmt.Sprintf("无法通过 SSH 连到音箱 %s（%d 端口，当前密码 %s）。请确认音箱已开机、在同一局域网，且填写的是它当前的 SSH 端口和密码。", cfg.IP, oldPort, oldPass))
		return
	}
	a.logf("SSH 已连通（%s:%d），正在应用 SSH 配置…", cfg.IP, oldPort)
	a.setFlash(func(f *flashState) {
		f.step, f.stepLabel, f.phase, f.progress = 2, "应用 SSH 配置", phaseSSHApply, 60
	})
	if err := provisionSSH(sshProvisionConfig{
		Host:    cfg.IP,
		OldPass: oldPass,
		NewPass: cfg.Password,
		Port:    cfg.Port,
		LANOnly: cfg.LANOnly,
		OldPort: oldPort,
	}, a.logf); err != nil {
		a.failFlash("SSH 配置应用失败：" + err.Error())
		return
	}
	// 顺带安装/更新开机自启框架（对标 open-xiaoai），使 init-apps/ 程序开机自动运行。
	a.setFlash(func(f *flashState) { f.step, f.stepLabel, f.progress = 3, "安装开机自启", 90 })
	if err := provisionInit(a.root, a.loadInitApps(), cfg.Port, cfg.Password, oldPass, cfg.IP, a.logf); err != nil {
		a.failFlash("开机自启安装失败：" + err.Error())
		return
	}
	a.saveLastSSHConfig(cfg)
	a.setFlash(func(f *flashState) { f.progress = 100 })
	a.logf("===== SSH 配置应用完成 =====")
	a.finishFlash(phaseDone, fmt.Sprintf("SSH 配置已应用：ssh root@%s 端口 %d。", cfg.IP, cfg.Port))
}

// sshReachable checks whether the device accepts an SSH login right now.
func sshReachable(ip string, port int, pass string) bool {
	cl, err := sshDial(ip, port, "root", pass, 2*time.Second)
	if err != nil {
		return false
	}
	cl.Close()
	return true
}

func (a *App) handleDriverInstall(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	res := a.installDriver()
	a.writeJSON(w, http.StatusOK, res)
}

func (a *App) handleDriverStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	a.refreshDriverStatus() // an explicit click must not reuse the eight-second cache
	a.writeJSON(w, http.StatusOK, a.driverSnapshot())
}

func (a *App) handleDriverRecovery(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		a.writeJSON(w, http.StatusMethodNotAllowed, map[string]any{"error": "method not allowed"})
		return
	}
	if err := openDriverRecovery(); err != nil {
		a.writeJSON(w, http.StatusInternalServerError, map[string]any{"error": "无法打开恢复设置，请手动在 Windows 设置中搜索“高级启动”"})
		return
	}
	a.writeJSON(w, http.StatusOK, map[string]any{"ok": true})
}

// handleInitApps 返回 init-apps/ 中将被安装为开机自启的程序列表（供 H5 展示）。
func (a *App) handleInitApps(w http.ResponseWriter, r *http.Request) {
	a.writeJSON(w, http.StatusOK, map[string]any{
		"dir":  initAppLocalDir,
		"apps": a.loadInitApps(),
	})
}

// ---- helpers ----
func (a *App) findPackage(id string) (FwPackage, bool) {
	for _, p := range a.packages {
		if p.ID == id {
			return p, true
		}
	}
	return FwPackage{}, false
}

// sha256File hashes a file in 1 MiB chunks.
func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

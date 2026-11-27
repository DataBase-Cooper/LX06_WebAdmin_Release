package main

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type flashPhase string

const (
	phaseIdle     flashPhase = "idle"
	phaseConnect  flashPhase = "connect"
	phaseEnv      flashPhase = "env"
	phaseBake     flashPhase = "bake"
	phaseWrite    flashPhase = "write"
	phaseReboot   flashPhase = "reboot"   // 写入完成后 update.exe reset 自动重启
	phaseSSHWait  flashPhase = "sshwait"  // 手动应用（已刷机设备回连）
	phaseSSHApply flashPhase = "sshapply" // 手动应用（已刷机设备回连）
	phaseDone     flashPhase = "done"
	phaseError    flashPhase = "error"
	phaseCancel   flashPhase = "cancelled"
)

type flashState struct {
	active     bool
	phase      flashPhase
	step       int
	stepTotal  int
	stepLabel  string
	progress   int // 0..100 overall
	message    string
	err        string
	done       bool
	pkgID      string
	startedAt  time.Time
	finishedAt time.Time

	ssh      SSHConfig // official-ssh：刷机前烘焙进镜像的 SSH 配置（端口/密码/仅局域网，无 IP）
	initApps []InitApp // 开机自启程序（来自 init-apps/，烘焙进镜像 /lx06-init.d/）
	bakedImg string    // 烘焙后待写入 system0 的镜像（official-ssh 才有；空=用包内 root.squashfs）
	bakedTmp string    // 烘焙临时目录（结束后清理）

	// 清空 DATA 分区选项：仅设备识别前（等待设备步骤）可改，
	// 识别后 wipeDecided 置位，runFlash 在写入 system0 前执行擦除。
	wipeData    bool
	wipeDecided bool
	wipeResult  string // pending/running/succeeded/unconfirmed/skipped; never infer success from option locking
	wipeNotice  string // optional preparation/cache warnings never redefine DATA erasure

	cancel   chan struct{}
	cancelOK bool
}

func (a *App) beginFlash(pkgID string, ssh SSHConfig, initApps []InitApp, wipeData bool) bool {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	if a.drvBusy || a.drvRebootRequired {
		return false
	}
	if a.closing || (a.flash != nil && a.flash.active) {
		return false
	}
	stepTotal := 6 // + 自动重启
	if pkgID == "official-ssh" {
		stepTotal = 7 // + 烘焙 SSH 配置进镜像（刷机前，无需 IP）
	}
	a.flash = &flashState{
		active:    true,
		phase:     phaseConnect,
		stepTotal: stepTotal,
		progress:  0,
		cancel:    make(chan struct{}),
	}
	a.flash.startedAt = time.Now()
	a.flash.pkgID = pkgID
	a.flash.ssh = ssh
	a.flash.initApps = initApps
	a.flash.wipeData = wipeData
	a.flash.wipeResult = "skipped"
	if wipeData {
		a.flash.wipeResult = "pending"
	}
	return true
}

func (a *App) requestFlashCancel() bool {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	if a.flash == nil || !a.flash.active {
		return false
	}
	a.flash.cancelOK = true
	select {
	case <-a.flash.cancel:
	default:
		close(a.flash.cancel)
	}
	return true
}

// requestFlashWipe sets the "wipe DATA partition" option. It is only honoured
// while the run is active AND the device has not been identified yet (the
// waiting-for-device step). Once identifyPoll returns, wipeDecided is latched
// and the flag can no longer change the current run.
func (a *App) requestFlashWipe(wipe bool) error {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	f := a.flash
	if f == nil || !f.active {
		return fmt.Errorf("no flash task in progress")
	}
	if f.wipeDecided {
		return fmt.Errorf("device already identified — the wipe-DATA option only takes effect before identification")
	}
	f.wipeData = wipe
	return nil
}

func (a *App) setFlash(f func(*flashState)) {
	a.flashMu.Lock()
	if a.flash != nil {
		previous := a.flash.progress
		f(a.flash)
		if a.flash.active {
			a.flash.progress = min(99, max(previous, a.flash.progress))
		}
	}
	a.flashMu.Unlock()
}

// The native window must check backend state, even if the page is stale.
func (a *App) requestWindowClose() bool {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	if a.drvBusy {
		return false
	}
	if a.flash != nil && a.flash.active {
		return false
	}
	a.closing = true
	return true
}

// updateProgress re-parses a progress line like " 42% / 1234MB" and folds it
// into the overall bar. Reserve 90..99 for reboot; only completion reaches 100.
var progressRe = regexp.MustCompile(`(\d+)\s*%`)

func (a *App) onProgressLine(line string) {
	m := progressRe.FindStringSubmatch(line)
	if m == nil {
		return
	}
	p, err := strconv.Atoi(m[1])
	if err != nil || p > 100 {
		return
	}
	a.setFlash(func(f *flashState) {
		if f.phase == phaseWrite {
			f.progress = 20 + p*70/100
			f.message = fmt.Sprintf("写入 system0 分区：%d%%", p)
		}
	})
}

// runFlash drives the Amlogic WorldCup sequence for one firmware package.
//
// Sequence (per open-xiaoai docs/flash.md + vendor flash script — saveenv
// after EACH setenv):
//  1. identify            (poll until device present; user power-cycles)
//  2. bulkcmd setenv bootdelay 15 + saveenv
//  3. bulkcmd setenv boot_part boot0 + saveenv
//  4. bulkcmd saveenv      (final save)
//  5. [official-ssh] bake SSH config + init apps into the image (~1s)
//  6. partition system0 <image>   (the long write)
//  7. reset               (auto reboot: U-Boot resets, boots system0 after
//     the 15s bootdelay; USB can stay plugged)
//
// official-ssh 镜像在刷机前把 SSH 配置（端口/root 密码/仅局域网）与开机
// 自启程序烘焙进 root.squashfs —— 设备首次开机即按配置启动，全程不需要
// 音箱 IP，也不需要刷后 SSH 回连。
//
// 写入完成后执行 `update.exe reset` 自动重启（步骤 7），设备无需拔线。
func (a *App) runFlash(pkg FwPackage) {
	st := a.currentFlash()
	if st == nil {
		return
	}
	a.logf("===== 开始刷机：%s =====", pkg.Name)
	firmware := filepath.Join(pkg.Dir, pkg.Firmware)
	a.logf("固件：%s (%.1f MB)", firmware, float64(pkg.Size)/1048576)

	if pkg.ID == "official-ssh" {
		a.logf("SSH 配置（烘焙进固件，开机即生效）：端口 %d / root 密码 %s / 仅局域网 %v",
			st.ssh.Port, maskPassword(st.ssh.Password), st.ssh.LANOnly)
	}

	// ---- Step 0: integrity preflight (per-package SHA256SUMS.txt) ----
	a.setFlash(func(f *flashState) {
		f.step, f.stepLabel = 0, "校验固件"
	})
	if err := a.preflightPackage(pkg); err != nil {
		a.finishFlash(phaseError, "固件完整性校验失败："+err.Error())
		return
	}
	a.logf("固件完整性校验通过")
	// official-ssh 的烘焙 base 就是 preflightPackage 刚校验过的那个镜像
	// （official-ssh/root.squashfs），无需二次校验。

	a.setFlash(func(f *flashState) {
		f.step, f.stepLabel = 1, "识别设备"
	})

	// ---- Step 1: identify (poll) ----
	ok, ver := a.identifyPoll()
	if !ok {
		a.finishFlash(phaseCancel, "未能在等待时间内识别到设备")
		return
	}
	a.logf("设备已识别，固件版本: %s", ver)

	// ---- Step 2（可选）：清空 DATA 分区 ----
	// 选项在设备识别前由 UI 决定（等待设备步骤，/api/flash/wipe 或启动参数）。
	// 此处锁存最终选择：识别完成后用户再改也不影响本次刷机。
	a.flashMu.Lock()
	wipeRun := a.flash != nil && a.flash.wipeData
	if a.flash != nil {
		a.flash.wipeDecided = true
	}
	a.flashMu.Unlock()
	w := 0
	wipedOK := true
	var wipeReport dataWipeReport
	if wipeRun {
		w = 1
		a.setFlash(func(f *flashState) {
			f.stepTotal++ // 本步骤占用 step 2，后续步骤号整体后移 1
			f.step, f.stepLabel, f.progress = 2, "清空 DATA 分区", 5
			f.message = "正在擦除音箱 DATA 分区（用户数据 / 网络配置 / 时间等）…"
			f.wipeResult = "running"
		})
		wipeReport = a.wipeDataPartitions()
		wipedOK = wipeReport.DataErased
		a.setFlash(func(f *flashState) {
			f.wipeResult, f.wipeNotice = wipeReport.UIState()
		})
	}

	// ---- Step 2+w: set bootdelay (vendor: setenv + saveenv per setenv) ----
	a.setFlash(func(f *flashState) { f.step, f.stepLabel, f.progress = 2+w, "设置启动延时", 8 })
	if !a.bulkCmd("setenv bootdelay 15", "设置启动延时 15s") {
		return
	}
	if !a.bulkCmd("saveenv", "保存环境变量 (saveenv)") {
		return
	}

	// ---- Step 3+w: set boot_part ----
	a.setFlash(func(f *flashState) { f.step, f.stepLabel, f.progress = 3+w, "设置启动分区 boot0", 12 })
	if !a.bulkCmd("setenv boot_part boot0", "设置启动分区 boot0") {
		return
	}
	if !a.bulkCmd("saveenv", "保存环境变量 (saveenv)") {
		return
	}

	// ---- Step 4+w: final saveenv ----
	a.setFlash(func(f *flashState) { f.step, f.stepLabel, f.progress = 4+w, "保存环境变量", 16 })
	if !a.bulkCmd("saveenv", "保存环境变量 (saveenv)") {
		return
	}

	// ---- Step 5 (official-ssh 镜像): 烘焙 SSH 配置 + 自启程序 + 日志传送目标 ----
	// 必须在写入 system0 之前完成：刷完拔线重启，设备直接按配置起 SSH。
	// 日志传送目标（本机内网 IP:87）随 /lx06-log.conf 一并 append 进镜像。
	if pkg.ID == "official-ssh" {
		a.setFlash(func(f *flashState) {
			f.step, f.stepLabel, f.phase, f.progress = 5+w, "烘焙 SSH 配置", phaseBake, 18
			f.message = "把端口/密码/仅局域网 + 开机自启程序 + 日志传送目标合并进镜像（约 1 秒）…"
		})
		outImg, tmpDir, err := a.bakeSSHConf(pkg, st.ssh, st.initApps, a.logf)
		if err != nil {
			if tmpDir != "" {
				os.RemoveAll(tmpDir)
			}
			a.failFlash("镜像烘焙失败：" + err.Error())
			return
		}
		st.bakedTmp = tmpDir
		st.bakedImg = outImg
		firmware = outImg
		a.logf("镜像烘焙完成，待写入 system0（%d 字节）", fileSize(outImg))
	}

	// ---- partition system0（倒数第二步）----
	a.setFlash(func(f *flashState) {
		f.step, f.stepLabel, f.progress, f.phase = f.stepTotal-1, "写入 system0", 20, phaseWrite
	})
	if !a.partitionSystem0(firmware) {
		return
	}

	a.logf("===== 刷机写入完成 =====")

	if n := len(st.initApps); n > 0 && pkg.ID == "official-ssh" {
		a.logf("开机自启程序 %d 个已烘焙进镜像，设备开机自动安装运行", n)
	}
	a.cleanupBaked()

	// ---- 最后一步：自动重启（update.exe reset，已实测）----
	// 写入前已 setenv bootdelay 15 + boot_part boot0 + saveenv，复位后 U-Boot
	// 等 15s 无刷机信号即从 system0 启动新系统——USB 不用拔。
	a.setFlash(func(f *flashState) {
		f.step, f.stepLabel, f.phase, f.progress = f.stepTotal, "自动重启", phaseReboot, 90
		f.message = "正在让小爱重启（U-Boot 复位），设备将自动进入新系统，无需拔线…"
	})
	rebooted := a.rebootDevice()

	base := "写入完成，设备正在自动重启，音箱将自动进入新系统（首次开机约 1 分钟），无需拔 USB 或电源线。"
	if !rebooted {
		base = "写入完成！自动重启指令未生效，请手动拔掉 USB 数据线和电源线，重新通电（只插电源线、USB 不插）即可正常开机。注意：此时再插 USB 会重新进入刷机模式。"
	}
	if wipeRun {
		if wipedOK {
			base += " DATA 分区已清空（出厂数据重置）：音箱开机后需重新配网，并做首启初始化。"
		} else {
			base += " 注意：未收到 DATA 分区擦除成功确认，旧数据可能保留，请查看刷机日志。"
		}
		if notice := wipeReport.Notice(); notice != "" {
			base += " 附加提示：" + notice + "。"
		}
	}
	if pkg.ID == "official-ssh" {
		msg := fmt.Sprintf("SSH 已烘焙进固件：端口 %d（root 密码即刷机前设置）。", st.ssh.Port)
		if n := len(st.initApps); n > 0 {
			msg += fmt.Sprintf(" 开机自启程序 %d 个已随固件写入。", n)
		}
		if ip := detectLanIPv4(); ip != "" {
			msg += fmt.Sprintf(" 日志传送目标已写入固件：本机 %s:%d（异常重启前设备自动发系统日志；Web 88 X×5 弹窗可改）。", ip, logRecvPort)
		}
		msg += base + " 连上 Wi-Fi 后 SSH 即按配置在线（首次启动约 1 分钟）。"
		a.finishFlash(phaseDone, msg)
		return
	}
	a.finishFlash(phaseDone, base)
}

// rebootDevice 向 U-Boot 发送 `update.exe reset` 让设备复位：退出刷机模式并
// 从 boot0 启动刚写入的 system0。写入前已保存 bootdelay=15，复位后即使 USB
// 插着，U-Boot 等 15s 无人抢刷即正常启动（166 实测：复位后 identify 找不
// 到设备、ping 全通，正常进系统）。
//
// 成功判定：reset 命令退出 0 + 短暂等待后 identify 已找不到刷机模式设备。
// 校验不通过不算刷机失败（system0 已写入完成），返回 false 让调用方提示
// 手动断电重启。
func (a *App) rebootDevice() bool {
	a.logf("执行: update.exe reset（复位 U-Boot，设备自动重启）")
	fail := false
	resetNotice := false
	ok, _ := a.runUpdate([]string{"reset"}, func(line string) {
		low := strings.ToLower(line)
		// Vendor utility prints the same reset acknowledgement twice with only
		// case changed. This suppresses its duplicate display, never a command.
		if strings.EqualFold(strings.TrimSpace(line), "reset worldcup device") {
			if resetNotice {
				return
			}
			resetNotice = true
		}
		a.logf("  %s", line)
		if strings.Contains(low, "success") {
			a.logf("复位指令已送达，设备正在重启…")
		}
		if strings.Contains(low, "err") {
			fail = true
		}
	})
	if !ok || fail {
		a.logf("reset 指令失败，回退手动断电重启")
		return false
	}
	// 等 U-Boot 复位并退出刷机模式（实测几秒内完成 USB 释放），
	// 再单次 identify 校验：找到=仍在刷机模式（复位未生效）。
	for i := 0; i < 3; i++ {
		// Keep the proven USB reset/identify cadence, but expose the wait.
		for second := 1; second <= 10; second++ {
			time.Sleep(time.Second)
			elapsed := i*10 + second
			a.setFlash(func(f *flashState) {
				f.progress = 91 + elapsed*8/30
				f.message = fmt.Sprintf("正在确认自动重启（已等待 %d 秒），请保持连接…", elapsed)
			})
		}
		found := false
		_, _ = a.runUpdate([]string{"identify"}, func(line string) {
			low := strings.ToLower(strings.TrimSpace(line))
			if strings.Contains(low, "firmware version") {
				found = true
			}
		})
		if !found {
			a.logf("自动重启完成：设备已退出刷机模式，正在启动新系统")
			return true
		}
		a.logf("设备仍在刷机模式（第 %d 次确认），等待复位生效…", i+1)
	}
	a.logf("复位后设备仍在刷机模式，回退手动断电重启")
	return false
}

// currentFlash 返回当前活动刷机状态（无则 nil）。
func (a *App) currentFlash() *flashState {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	return a.flash
}

// cleanupBaked 清理烘焙临时目录（成功后与失败后都调用，幂等）。
func (a *App) cleanupBaked() {
	a.flashMu.Lock()
	tmp := ""
	if a.flash != nil {
		tmp = a.flash.bakedTmp
		a.flash.bakedTmp = ""
	}
	a.flashMu.Unlock()
	if tmp != "" {
		os.RemoveAll(tmp)
	}
}

// maskPassword 日志里打码显示密码（保留首字符与长度）。
func maskPassword(pw string) string {
	if pw == "" {
		return "（空）"
	}
	if len(pw) <= 2 {
		return "••"
	}
	return pw[0:1] + "•••"
}

// preflightPackage validates the firmware against the per-package
// SHA256SUMS.txt manifest (LX04 layout: hash  size  name, relative to pkg dir).
func (a *App) preflightPackage(pkg FwPackage) error {
	manifest := filepath.Join(pkg.Dir, "SHA256SUMS.txt")
	data, err := os.ReadFile(manifest)
	if err != nil {
		return fmt.Errorf("缺少完整性清单 SHA256SUMS.txt")
	}
	want := ""
	for _, raw := range strings.Split(string(data), "\n") {
		parts := strings.Fields(raw)
		if len(parts) < 3 {
			continue
		}
		hash, size, name := parts[0], parts[1], strings.Join(parts[2:], " ")
		if len(hash) != 64 {
			continue
		}
		if name == pkg.Firmware || filepath.Base(name) == pkg.Firmware {
			want = strings.ToLower(hash)
			_ = size
		}
	}
	if want == "" {
		return fmt.Errorf("完整性清单中没有 %s 的条目", pkg.Firmware)
	}
	got, err := sha256File(filepath.Join(pkg.Dir, pkg.Firmware))
	if err != nil {
		return fmt.Errorf("读取固件失败: %w", err)
	}
	if got != want {
		return fmt.Errorf("SHA256 不匹配（固件可能损坏或被改动）")
	}
	return nil
}

// finishFlash marks the run finished, recording a terminal phase + message.
func (a *App) finishFlash(phase flashPhase, msg string) {
	a.setFlash(func(f *flashState) {
		f.active = false
		f.done = true
		f.phase = phase
		f.message = msg
		f.finishedAt = time.Now()
		if phase == phaseDone {
			// Only a completed workflow may display 100%.
			f.progress = 100
		}
	})
	a.logf("刷机结束: %s", msg)
}

// runUpdate launches update.exe with args from its bin dir and streams the
// combined stdout+stderr to onLine. Returns (exitOK, errText). On cancellation
// it kills the child and returns (true, "") so the caller can inspect cancelOK.
func (a *App) runUpdate(args []string, onLine func(string)) (bool, string) {
	cmd := exec.Command(a.updateExe(), args...)
	cmd.Dir = a.binDir()
	hideProcess(cmd)

	// Merge stderr into stdout via a pipe so progress + errors share one stream.
	pr, pw := io.Pipe()
	cmd.Stdout = pw
	cmd.Stderr = pw

	if err := cmd.Start(); err != nil {
		a.logf("update.exe 启动失败: %v", err)
		return false, err.Error()
	}

	doneCh := make(chan struct{})
	go func() {
		defer close(doneCh)
		sc := bufio.NewScanner(pr)
		sc.Split(splitUpdateOutput)
		sc.Buffer(make([]byte, 64*1024), 4*1024*1024)
		for sc.Scan() {
			if s := strings.TrimSpace(sc.Text()); s != "" {
				onLine(s)
			}
		}
	}()

	waitErr := a.waitUpdate(cmd)
	pw.Close()
	<-doneCh

	a.flashMu.Lock()
	cancelled := a.flash != nil && a.flash.cancelOK
	a.flashMu.Unlock()
	if cancelled {
		a.logf("update.exe 已按取消结束")
		return true, ""
	}
	if waitErr != nil {
		if ee, ok := waitErr.(*exec.ExitError); ok {
			a.logf("update.exe 退出码 %d", ee.ExitCode())
		} else {
			a.logf("update.exe 出错: %v", waitErr)
		}
		return false, waitErr.Error()
	}
	return true, ""
}

// waitUpdate blocks until cmd exits, killing it if the flash run is cancelled.
func (a *App) waitUpdate(cmd *exec.Cmd) error {
	waitErr := make(chan error, 1)
	go func() { waitErr <- cmd.Wait() }()
	for {
		select {
		case err := <-waitErr:
			return err
		case <-time.After(300 * time.Millisecond):
			a.flashMu.Lock()
			cancelled := a.flash == nil || a.flash.cancelOK
			a.flashMu.Unlock()
			if cancelled {
				if cmd.Process != nil {
					_ = cmd.Process.Kill()
				}
				return nil
			}
		}
	}
}

// identifyPoll runs `update.exe identify` in a loop until the device answers
// or the run is cancelled. Returns (found, firmwareVersion).
func (a *App) identifyPoll() (bool, string) {
	a.setFlash(func(f *flashState) {
		f.phase = phaseConnect
		f.message = "等待设备：① 先把 USB 数据线插到小爱 Type-C 口（电脑端直插主板后置口）② 再给小爱断电、重新通电。U-Boot 检测到 USB 后停进刷机模式"
	})
	// 0.2s 间隔：实测 update.exe identify 单次 84ms，200ms 一轮 ≈ 5 次/秒，
	// 拔电-上电进入下载模式后 ~1s 内即可捕获（原 1.5s 间隔要 1.5s~3s）。
	// 每 25 轮（~5s）触发一次 USB 树重枚举，恢复"半连接"（设备枚举了但
	// control 消息失败）的状态，免拔插重插。
	for i := 0; i < 3000; i++ {
		// cancelled?
		a.flashMu.Lock()
		cancelled := a.flash == nil || a.flash.cancelOK
		a.flashMu.Unlock()
		if cancelled {
			return false, ""
		}
		found := false
		ver := ""
		_, _ = a.runUpdate([]string{"identify"}, func(line string) {
			l := strings.TrimSpace(line)
			if l == "" {
				return
			}
			low := strings.ToLower(l)
			// 官方成功标志：「This firmware version is x-x-x-...」。
			// 失败标志：「can not find device」或「ERR: get info from device failed」
			// （设备在 USB 层出现但 control 消息失败 —— 半连接，不能当已识别，
			//  否则 setenv 第一条 bulkcmd 就撞上同样的 control 失败）。
			if strings.Contains(low, "err") || strings.Contains(low, "can not find device") {
				return
			}
			if strings.Contains(low, "firmware version") {
				found = true
				ver = l
			}
		})
		if found {
			return true, ver
		}
		a.setFlash(func(f *flashState) {
			f.message = fmt.Sprintf("等待设备中…（%d 次探测，0.2s/次）请确保已安装驱动并已拔插 USB", i+1)
		})
		if i%25 == 24 {
			usbRescan()
		}
		time.Sleep(200 * time.Millisecond)
	}
	return false, ""
}

// wipeBulkCmd runs one `update.exe bulkcmd "<cmd>"`, logging all output.
// Returns true only if update.exe exited 0 and the device acknowledged success.
// An empty/unknown response is unconfirmed. Unlike bulkCmd it
// NEVER aborts the flash run — wipe is best-effort.
func (a *App) wipeBulkCmd(command, label string) bool {
	a.logf("执行: update.exe bulkcmd \"%s\"", command)
	var status wipeCommandStatus
	ok, _ := a.runUpdate([]string{"bulkcmd", command}, func(line string) {
		a.logf("  %s", line)
		status.Consume(line)
	})
	if !ok {
		a.logf("%s: update.exe 未正常退出", label)
		return false
	}
	if !status.Confirmed || status.Failed {
		a.logf("%s: 未收到设备成功确认（详见命令输出，将检查备用命令或记录附加提示）", label)
		return false
	}
	a.logf("%s: 成功", label)
	return true
}

// wipeDataPartitions erases the DATA (+ cache) partitions while the device is
// in U-Boot flash mode, so the freshly flashed system boots with a clean /data.
//
// Follows the standard Amlogic "wipe" sequence (aml-flash-tool, non-m8 SoCs;
// this device is AXG):
//
//	setenv firstboot 1 ; saveenv
//	rpmb_reset
//	amlmmc erase data   /  nand erase.part data
//	amlmmc erase cache  /  nand erase.part cache
//
// Try the alternate form only when the first form was not confirmed. DATA and
// optional cache/preparation outcomes remain separate; missing cache support
// must not turn an acknowledged DATA erase into a failed DATA status.
//
// Best-effort: a failure is logged (and reported to the caller) but does NOT
// abort the flash — the system0 write is what the user asked for.
func (a *App) wipeDataPartitions() dataWipeReport {
	a.logf("===== 清空 DATA 分区开始 =====")
	report := performDataWipe(a.wipeBulkCmd)
	if report.DataErased {
		a.logf("===== 清空 DATA 分区完成：设备已确认 data 擦除成功 =====")
	} else {
		a.logf("===== DATA 擦除未确认成功：两种命令都未取得成功确认 =====")
	}
	if notice := report.Notice(); notice != "" {
		a.logf("附加清理/初始化提示（与 DATA 擦除结果分别记录）：%s", notice)
	}
	if !report.RPMBReset {
		a.logf("附加步骤 RPMB 重置未确认；此项不用于判断 DATA 是否已擦除")
	}
	return report
}

// bulkCmd runs one `update.exe bulkcmd "<cmd>"` and checks for failure.
func (a *App) bulkCmd(command, label string) bool {
	a.logf("执行: update.exe bulkcmd \"%s\"", command)
	fail := false
	ok, _ := a.runUpdate([]string{"bulkcmd", command}, func(line string) {
		a.logf("  %s", line)
		if strings.Contains(strings.ToLower(line), "err") && !strings.Contains(line, "no error") {
			fail = true
		}
	})
	if !ok {
		a.logf("%s：失败", label)
		a.failFlash(fmt.Sprintf("%s失败：%s", label, "update.exe 执行出错"))
		return false
	}
	if fail {
		a.logf("%s：检测到 ERR", label)
		a.failFlash(fmt.Sprintf("%s失败：设备返回错误（USB 控制消息不通）。\n请：换 USB 口/换线，确认音箱断电后重新上电进入刷机模式；若经 RDP 远程，请在本机(非远程会话)直插 USB。然后重试。", label))
		return false
	}
	a.logf("%s：成功", label)
	return true
}

// failFlash terminates the run with an error phase + message.
func (a *App) failFlash(msg string) {
	a.setFlash(func(f *flashState) { f.err = msg })
	a.finishFlash(phaseError, msg)
}

// partitionSystem0 writes root.squashfs to the system0 partition.
func (a *App) partitionSystem0(firmware string) bool {
	a.logf("执行: update.exe partition system0 %s", filepath.Base(firmware))
	fail := false
	completed := false
	ok, _ := a.runUpdate([]string{"partition", "system0", firmware}, func(line string) {
		a.logf("  %s", line)
		a.onProgressLine(line)
		if strings.Contains(strings.ToLower(line), "err") && !strings.Contains(line, "no error") {
			fail = true
		}
		if strings.Contains(line, "Transfer Complete") {
			completed = true
		}
	})
	if !ok {
		a.failFlash("system0 分区写入失败：update.exe 执行出错")
		return false
	}
	if fail && !completed {
		a.failFlash("system0 分区写入失败：设备返回错误")
		return false
	}
	if !completed {
		// No explicit "Transfer Complete!" but exit 0 and no ERR — accept.
		a.logf("写入结束（未检测到 Transfer Complete，但进程正常退出）")
	}
	a.setFlash(func(f *flashState) { f.progress = 90 })
	a.logf("system0 分区写入完成")
	return true
}

// update.exe also rewrites percentages with CR without a newline.
func splitUpdateOutput(data []byte, atEOF bool) (int, []byte, error) {
	if i := bytes.IndexAny(data, "\r\n"); i >= 0 {
		return i + 1, data[:i], nil
	}
	if atEOF && len(data) > 0 {
		return len(data), data, nil
	}
	return 0, nil, nil
}

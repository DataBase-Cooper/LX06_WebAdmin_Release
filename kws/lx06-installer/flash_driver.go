//go:build windows

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"golang.org/x/sys/windows"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// Driver installation runs in a short-lived elevated copy of this executable.
// The main UI stays responsive and does not need to run as administrator.
const (
	driverWorkerFlag = "--driver-worker"
	driverCatName    = "WorldCup_Device.cat"
)

// driverInfName is the on-disk INF filename; resolved case-insensitively at
// install time (Windows FS is case-insensitive, but we normalize anyway).
var driverInfName = "WorldCup_Device.inf"

// driverWorkerMain is launched through the Windows runas consent flow.
func driverWorkerMain(driverDir, statusFile string) int {
	if !driverIsElevated() {
		writeDriverStatus(statusFile, map[string]any{"ok": false, "error": "驱动安装进程未取得管理员权限，请完成 Windows 授权后重试"})
		return 1
	}
	env := currentDriverEnvironment()
	if err := env.validate(); err != nil {
		writeDriverStatus(statusFile, map[string]any{"ok": false, "error": err.Error()})
		return 1
	}
	dir, cleanup, err := stageWorldCupDriver(driverDir, env.Arch)
	if err != nil {
		writeDriverStatus(statusFile, map[string]any{"ok": false, "error": err.Error()})
		return 1
	}
	defer cleanup()
	return runDriverInstall(dir, statusFile, env, runDriverStep, bindWorldCupDriver, driverPackagePresent)
}

type driverCommandReport struct {
	Name   string `json:"name"`
	Output string `json:"output"`
}

type driverCommandRunner func(string, string, ...string) (string, bool, error)

// The injected runner keeps regression tests entirely outside Windows' driver
// store. The production worker only trusts the certificate shipped with this INF.
func runDriverInstall(dir, statusFile string, env driverEnvironment, run driverCommandRunner, bind func(string) (bool, error), verify func() bool) int {
	var steps []driverCommandReport
	reboot := false
	failurePhase := "error"
	finish := func(ok bool, message string) int {
		writeDriverStatus(statusFile, map[string]any{"ok": ok, "error": message, "failurePhase": failurePhase, "rebootRequired": reboot, "steps": steps})
		if ok {
			return 0
		}
		return 1
	}
	if err := env.validate(); err != nil {
		return finish(false, err.Error())
	}
	if err := validateDriverPayload(dir, env.Arch); err != nil {
		return finish(false, err.Error())
	}
	command := func(phase, name string, args ...string) error {
		writeDriverStatus(statusFile, map[string]any{"phase": phase})
		out, restart, err := run(dir, name, args...)
		reboot = reboot || restart
		steps = append(steps, driverCommandReport{Name: name + " " + strings.Join(args, " "), Output: tail(out, 6000)})
		return err
	}
	for _, store := range []string{"Root", "TrustedPublisher"} {
		// A clean Windows installation can have no TrustedPublisher store.
		// -f allows certutil to create it for our verified catalog signer.
		if err := command("certificates", "certutil", "-f", "-addstore", store, filepath.Join(dir, driverSignerName)); err != nil {
			return finish(false, "添加驱动签名证书失败（"+store+"）："+err.Error())
		}
	}
	inf := filepath.Join(dir, driverInfName)
	if err := command("pnputil", "pnputil", driverAddArgs(env.Build, inf)...); err != nil {
		output := steps[len(steps)-1].Output
		switch {
		case driverErrorContains(err, output, "0xe000024b", "0x800f024b", "0x80096010"):
			failurePhase = "driver-store"
			return finish(false, "Windows 校验驱动文件失败。请重新下载并完整解压新版刷机包，详细原因见安装日志")
		case driverSignatureDenied(err, output):
			failurePhase = "signature"
			return finish(false, errWorldCupSignature.Error())
		case driverErrorContains(err, output, "0xe0000247", "0x800f0247"):
			failurePhase = "driver-store"
			return finish(false, "Windows 无法将 WorldCup 驱动加入驱动存储区（0xE0000247）。请复制本次安装日志以定位原因")
		}
		return finish(false, "Windows 未接受 WorldCup 驱动："+err.Error()+"。请查看安装日志；签名或安全策略限制不能仅靠重试解决")
	}
	if !reboot {
		writeDriverStatus(statusFile, map[string]any{"phase": "binding"})
		if restart, err := bind(dir); err != nil {
			failurePhase = driverFailurePhase(err)
			return finish(false, err.Error())
		} else {
			reboot = restart
		}
	}
	writeDriverStatus(statusFile, map[string]any{"phase": "verify"})
	if !verify() {
		return finish(false, "安装结束，但驱动商店中未找到 WorldCup 驱动，请查看安装日志")
	}
	return finish(true, "")
}

func driverSignatureDenied(err error, output string) bool {
	if errors.Is(err, syscall.Errno(577)) {
		return true
	}
	return driverErrorContains(err, output, "0x800b0100", "0x800b0101", "0x800b0109", "0xe000022f", "0x800f022f",
		"0xc0000428", "does not contain digital signature information", "无法验证数字签名", "不包含数字签名信息")
}

func driverErrorContains(err error, output string, markers ...string) bool {
	value := strings.ToLower(output)
	if err != nil {
		value += "\n" + strings.ToLower(err.Error())
		var errno syscall.Errno
		if errors.As(err, &errno) {
			value += fmt.Sprintf("\n0x%08x", uint32(errno))
		}
	}
	for _, marker := range markers {
		if strings.Contains(value, marker) {
			return true
		}
	}
	return false
}

func openDriverRecovery() error {
	return windows.ShellExecute(0, windows.StringToUTF16Ptr("open"),
		windows.StringToUTF16Ptr("ms-settings:recovery"), nil, nil, windows.SW_SHOWNORMAL)
}

func runDriverStep(dir, exe string, args ...string) (string, bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	tool := nativeDriverTool(exe)
	if tool == "" {
		return "", false, fmt.Errorf("无法定位 Windows 系统工具 %s", exe)
	}
	cmd := exec.CommandContext(ctx, tool, args...)
	cmd.Dir = dir
	hideProcess(cmd)
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	var setupLog *driverSetupLog
	if exe == "pnputil" {
		setupLog = openDriverSetupLog()
		if setupLog != nil {
			defer setupLog.file.Close()
		}
	}
	err := cmd.Run()
	text := decodeDriverOutput(out.Bytes())
	if success, restart := driverExitSuccess(err); success {
		return text, restart, nil
	}
	if err != nil {
		if diagnostic := setupLog.diagnostic(driverInfName); diagnostic != "" {
			text += "\n\nSetupAPI（本次 WorldCup 安装）\n" + diagnostic
		}
		if ctx.Err() != nil {
			return text, false, fmt.Errorf("%s 执行超时，请查看 Windows 安装日志后重试", exe)
		}
		if exe == "pnputil" && cmd.ProcessState != nil && cmd.ProcessState.ExitCode() == 577 {
			return text, false, fmt.Errorf("%w (输出: %s)", syscall.Errno(577), tail(text, 1200))
		}
		return text, false, fmt.Errorf("%w (输出: %s)", err, tail(text, 1200))
	}
	return text, false, nil
}

func tail(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return "…" + s[len(s)-n:]
}

func writeDriverStatus(statusFile string, payload map[string]any) {
	b, _ := json.Marshal(payload)
	// Readers see either the previous complete phase or the new complete result.
	if os.WriteFile(statusFile+".tmp", b, 0o644) == nil {
		// A reader or scanner can briefly hold the previous file on Windows.
		// Keep a complete JSON result and retry rather than expose partial writes.
		for attempt := 0; attempt < 6; attempt++ {
			if os.Rename(statusFile+".tmp", statusFile) == nil {
				return
			}
			time.Sleep(25 * time.Millisecond)
		}
	}
}

// launchDriverWorker requests elevation directly; ERROR_CANCELLED means the
// user declined UAC. EscapeArg handles spaces and non-ASCII installation paths.
func launchDriverWorker(driverDir, statusFile string, hwnd uintptr) (driverWorkerProcess, error) {
	exe, err := os.Executable()
	if err != nil {
		return nil, err
	}
	params := syscall.EscapeArg(driverWorkerFlag) + " " + syscall.EscapeArg(driverDir) + " " + syscall.EscapeArg(statusFile)
	return startElevatedDriver(exe, params, hwnd)
}

func driverLaunchError(err error) string {
	if errors.Is(err, windows.ERROR_CANCELLED) {
		return "已取消管理员授权，驱动尚未安装。请重新点击“安装驱动”，在 Windows 授权窗口选择“是”；标准用户需输入管理员账号。"
	}
	return "无法启动驱动安装授权：" + err.Error() + "。请右键程序选择“以管理员身份运行”后重试；远程桌面请检查本机的授权窗口。"
}

func (a *App) installDriver() map[string]any {
	a.flashMu.Lock()
	defer a.flashMu.Unlock()
	if a.closing || (a.flash != nil && a.flash.active) {
		return map[string]any{"ok": false, "error": "正在刷机或退出，请完成后再安装驱动"}
	}
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	if a.drvBusy {
		return map[string]any{"ok": false, "error": "驱动安装正在进行，请完成管理员授权并等待结果", "busy": true}
	}
	if a.drvRebootRequired {
		return map[string]any{"ok": false, "error": a.drvMsg}
	}
	env := currentDriverEnvironment()
	preflightErr := env.validate()
	if preflightErr == nil {
		preflightErr = validateDriverPayload(a.driverDir(), env.Arch)
	}
	if preflightErr != nil {
		a.drvError = preflightErr.Error()
		a.drvMsg, a.drvPhase = a.drvError, "error"
		return map[string]any{"ok": false, "error": a.drvError}
	}
	a.drvBusy = true
	a.drvPhase = "authorizing"
	a.drvStartedAt = time.Now()
	a.drvError = ""
	a.drvRebootRequired = false
	a.drvMsg = "正在请求管理员授权，请在 Windows 弹窗中选择“是”；如果没有看到弹窗，请检查任务栏或本机桌面。"
	a.logf("驱动安装/修复：Windows build %d，原生架构 %s", env.Build, env.Arch)
	go a.driveDriverInstall()
	return map[string]any{"ok": true, "started": true, "message": a.drvMsg}
}

func (a *App) driveDriverInstall() {
	a.driveDriverInstallWith(launchDriverWorker, driverInstalledNow)
}

func (a *App) driveDriverInstallWith(launch func(string, string, uintptr) (driverWorkerProcess, error), verify func() bool) {
	defer func() { a.drvMu.Lock(); a.drvBusy = false; a.drvMu.Unlock() }()
	fail := func(message string) {
		a.drvMu.Lock()
		a.drvInstalled = false
		a.drvPhase = "error"
		a.drvError = message
		a.drvMsg = message
		a.drvMu.Unlock()
		a.logf("驱动安装未完成：%s", message)
	}
	dir, err := os.MkdirTemp(a.dataDir, "driver-install-")
	if err != nil {
		fail("无法创建安装状态目录：" + err.Error())
		return
	}
	defer os.RemoveAll(dir)
	statusFile := filepath.Join(dir, "status.json")
	a.drvMu.Lock()
	hwnd := a.nativeWindow
	a.drvMu.Unlock()
	process, err := launch(a.driverDir(), statusFile, hwnd)
	if err != nil {
		fail(driverLaunchError(err))
		return
	}
	if process != nil {
		defer process.Close()
	}
	a.setDriverPhase("starting", "已完成授权，正在启动驱动安装，请稍候…")
	deadline := time.Now().Add(180 * time.Second)
	slowReported := false
	for {
		exited, exitCode := false, uint32(0)
		if process != nil {
			var waitErr error
			exited, exitCode, waitErr = process.Exited()
			if waitErr != nil {
				fail("无法跟踪管理员安装进程：" + waitErr.Error())
				return
			}
		}
		if data, err := os.ReadFile(statusFile); err == nil {
			var status struct {
				OK             *bool                 `json:"ok"`
				Phase          string                `json:"phase"`
				Error          string                `json:"error"`
				FailurePhase   string                `json:"failurePhase"`
				RebootRequired bool                  `json:"rebootRequired"`
				Steps          []driverCommandReport `json:"steps"`
			}
			if json.Unmarshal(data, &status) == nil {
				if status.OK != nil {
					for _, step := range status.Steps {
						a.logf("驱动步骤 %s\n%s", step.Name, step.Output)
					}
					if !*status.OK {
						if status.FailurePhase == "signature" {
							fail(status.Error + "。请按「驱动签名安装帮助」中的步骤处理。")
							a.setDriverPhase("signature", status.Error)
						} else if status.FailurePhase == "driver-store" {
							fail(status.Error)
							a.setDriverPhase("driver-store", status.Error)
						} else {
							fail(status.Error + "。详细原因见下方日志及 Windows SetupAPI 日志。")
						}
						return
					}
					if exited && exitCode != 0 {
						fail(fmt.Sprintf("安装程序异常退出（代码 %d），请查看日志后重试。", exitCode))
						return
					}
					if status.RebootRequired {
						a.drvMu.Lock()
						a.drvInstalled, a.drvRebootRequired = false, true
						a.drvPhase, a.drvMsg, a.drvError = "reboot", "驱动已安装，需要重启 Windows 后重新打开软件才能刷机。", ""
						a.drvMu.Unlock()
						return
					}
					a.setDriverPhase("verify", "安装结束，正在核对 WorldCup 驱动是否就绪…")
					if !verify() {
						fail("安装程序已结束，但未检测到 WorldCup 驱动。请重启电脑后重新检测；仍失败请查看安装日志。")
						return
					}
					a.drvMu.Lock()
					a.drvInstalled = true
					a.drvCheckedAt = time.Now()
					a.drvPhase = "done"
					a.drvError = ""
					a.drvMsg = "驱动安装成功，可以点击“下一步：选择刷机包”。"
					a.drvMu.Unlock()
					a.logf("WorldCup USB 驱动安装并校验成功")
					return
				}
				switch status.Phase {
				case "starting":
					a.setDriverPhase("starting", "管理员安装程序已启动，正在检查驱动包…")
				case "certificates":
					a.setDriverPhase("certificates", "正在安装驱动签名证书，请稍候…")
				case "pnputil":
					a.setDriverPhase("installing", "正在安装 WorldCup USB 驱动，请勿关闭程序…")
				case "binding":
					a.setDriverPhase("binding", "正在检查并修复音箱 USB 设备的驱动绑定…")
				case "verify":
					a.setDriverPhase("verify", "正在检查驱动安装结果…")
				}
			}
		}
		if exited {
			fail(fmt.Sprintf("管理员安装程序已退出（代码 %d），但未返回完整结果。请检查安全软件/Windows 应用控制拦截，以及日志目录写入权限后重试。", exitCode))
			return
		}
		if time.Now().After(deadline) && !slowReported {
			if process == nil {
				fail("等待驱动安装结果超时，请确认管理员授权窗口已处理。")
				return
			}
			// Never release the busy guard while a native installation is still
			// running: retrying here would start overlapping driver operations.
			a.setDriverPhase("installing", "Windows 仍在处理驱动安装，请等待安装进程结束；暂时不能重复安装或关闭软件。")
			slowReported = true
		}
		time.Sleep(500 * time.Millisecond)
	}
}

func (a *App) setDriverPhase(phase, message string) {
	a.drvMu.Lock()
	defer a.drvMu.Unlock()
	a.drvPhase = phase
	a.drvMsg = message
}

// refreshDriverStatus re-probes the driver store. pnputil /enum-drivers can
// take several seconds on some machines, so: (1) never hold drvMu while the
// subprocess runs (the UI polls driverSnapshot every ~2s), and (2) an in-flight
// guard prevents the startup goroutine and a status poll from double-checking.
func (a *App) refreshDriverStatus() {
	a.drvMu.Lock()
	if a.drvCheckInFlight || a.drvBusy || a.drvRebootRequired {
		a.drvMu.Unlock()
		return
	}
	a.drvCheckInFlight = true
	probeStarted := time.Now()
	a.drvMu.Unlock()

	installed, probeError := driverReadyProbe() // slow: no lock held

	a.drvMu.Lock()
	a.drvCheckInFlight = false
	if a.drvBusy || a.drvRebootRequired || a.drvStartedAt.After(probeStarted) {
		a.drvMu.Unlock()
		return
	}
	a.drvInstalled = installed
	a.drvCheckedAt = time.Now()
	if probeError != nil {
		a.drvPhase, a.drvError, a.drvMsg = driverFailurePhase(probeError), probeError.Error(), probeError.Error()
	}
	if installed {
		a.drvPhase = "done"
		a.drvError = ""
		if a.drvMsg == "" || a.drvMsg != "驱动已安装" {
			a.drvMsg = "驱动已安装"
		}
	}
	a.drvMu.Unlock()
}

// driverInstalledNow checks the driver store plus any attached WorldCup device.
// The package probe decodes localized output and has a bounded timeout.
func driverInstalledNow() bool {
	ready, _ := driverReadyProbe()
	return ready
}

func driverReadyProbe() (bool, error) {
	if err := currentDriverEnvironment().validate(); err != nil {
		return false, err
	}
	present, err := driverPackageProbe()
	if err != nil {
		return false, err
	}
	devices, err := connectedWorldCupDevices()
	if err != nil {
		return false, fmt.Errorf("读取音箱 USB 驱动状态失败：%w", err)
	}
	if err := worldCupDeviceError(devices); err != nil {
		return false, err
	}
	return present, nil
}

func driverPackagePresent() bool {
	present, _ := driverPackageProbe()
	return present
}

func driverPackageProbe() (bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	tool := nativeDriverTool("pnputil")
	if tool == "" {
		return false, errors.New("无法定位 Windows PnPUtil 驱动工具")
	}
	env := currentDriverEnvironment()
	cmd := exec.CommandContext(ctx, tool, driverEnumArgs(env.Build)...)
	hideProcess(cmd)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return false, fmt.Errorf("驱动检测失败：%v；%s", err, tail(decodeDriverOutput(out), 600))
	}
	text := strings.ToLower(decodeDriverOutput(out))
	if env.Build < 14393 {
		win, err := windows.GetWindowsDirectory()
		if err != nil {
			return false, err
		}
		return legacyWorldCupPackagePresent(text, filepath.Join(win, "INF"))
	}
	return strings.Contains(text, "worldcup_device.inf") && strings.Contains(text, "libusb-win32"), nil
}

// elevhelper: 计划任务提权执行器（RunLevel=Highest 计划任务以 High IL 运行本程序，
// 之后 medium-IL 的刷机工具通过 schtasks /run 零弹窗调用它）。
//
// 用法: elevhelper.exe <jobdir>
//   jobdir\job.json     {"action":"driver"|"run", "workdir":"...", "exe":"...", "args":[...]}
//   jobdir\result.json  {"ok":true,"elevated":true,"steps":[{name,exit,out}],"ts":"..."}
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

type Job struct {
	Action  string   `json:"action"` // "driver" | "run"
	Workdir string   `json:"workdir"`
	Exe     string   `json:"exe"`
	Args    []string `json:"args"`
}

type Step struct {
	Name string `json:"name"`
	Exit int    `json:"exit"`
	Out  string `json:"out"`
}

type Result struct {
	OK       bool   `json:"ok"`
	Elevated bool   `json:"elevated"`
	Steps    []Step `json:"steps"`
	TS       string `json:"ts"`
	Err      string `json:"err,omitempty"`
}

func main() {
	runtime.LockOSThread()

	jobdir := "."
	if len(os.Args) > 1 {
		jobdir = os.Args[1]
	}

	res := Result{TS: time.Now().Format(time.RFC3339), Elevated: isElevated()}
	writeResult := func() {
		b, _ := json.MarshalIndent(res, "", "  ")
		_ = os.WriteFile(filepath.Join(jobdir, "result.json"), b, 0644)
	}

	data, err := os.ReadFile(filepath.Join(jobdir, "job.json"))
	if err != nil {
		res.Err = "read job.json: " + err.Error()
		writeResult()
		return
	}
	var job Job
	if err := json.Unmarshal(data, &job); err != nil {
		res.Err = "parse job.json: " + err.Error()
		writeResult()
		return
	}

	switch job.Action {
	case "driver":
		ok, steps := doDriver(job.Workdir)
		res.OK = ok
		res.Steps = steps
	case "run":
		ok, steps := doRun(job.Workdir, job.Exe, job.Args)
		res.OK = ok
		res.Steps = steps
	case "gui":
		ok, out := doGUI(job.Workdir, job.Exe, job.Args)
		res.OK = ok
		res.Steps = []Step{{Name: filepath.Base(job.Exe), Exit: 0, Out: out}}
	default:
		res.Err = "unknown action: " + job.Action
	}
	writeResult()
}

func isElevated() bool {
	var h windows.Token
	if err := windows.OpenProcessToken(windows.CurrentProcess(), windows.TOKEN_QUERY, &h); err != nil {
		return false
	}
	defer windows.CloseHandle(windows.Handle(h))
	return h.IsElevated()
}

// runCmd 运行命令并捕获输出（无窗口）。
func runCmd(dir, name string, args ...string) (int, string) {
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	cmd.SysProcAttr = &windows.SysProcAttr{CreationFlags: windows.CREATE_NO_WINDOW}
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &out
	err := cmd.Run()
	code := 0
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			code = exitErr.ExitCode()
		} else {
			code = -1
		}
	}
	return code, trimOut(out.String())
}

func doDriver(workdir string) (bool, []Step) {
	var steps []Step
	ok := true

	// 找 .cer 和 .inf
	cers, _ := filepath.Glob(filepath.Join(workdir, "*.cer"))
	infos, _ := filepath.Glob(filepath.Join(workdir, "*.inf"))
	if len(cers) == 0 || len(infos) == 0 {
		return false, []Step{{Name: "glob", Exit: -1, Out: fmt.Sprintf("cer=%d inf=%d in %s", len(cers), len(infos), workdir)}}
	}

	code, out := runCmd(workdir, "certutil.exe", "-addstore", "Root", filepath.Base(cers[0]))
	steps = append(steps, Step{"certutil-addstore", code, out})
	if code != 0 {
		ok = false
	}
	// 剩余证书同样信任（.cat 的实际签名者可能是 dpscat 自签证书）
	for _, cer := range cers[1:] {
		ccode, cout := runCmd(workdir, "certutil.exe", "-addstore", "Root", filepath.Base(cer))
		steps = append(steps, Step{"certutil-addstore-" + filepath.Base(cer), ccode, cout})
	}

	code, out = runCmd(workdir, "pnputil.exe", "/add-driver", filepath.Base(infos[0]), "/install")
	steps = append(steps, Step{"pnputil-add-driver-install", code, out})
	if code != 0 {
		ok = false
	}

	code, out = runCmd(workdir, "pnputil.exe", "/enum-drivers")
	steps = append(steps, Step{"pnputil-enum-drivers", code, out})
	if code != 0 {
		ok = false
	}
	return ok, steps
}

func doRun(workdir, exe string, args []string) (bool, []Step) {
	code, out := runCmd(workdir, exe, args...)
	return code == 0, []Step{{Name: filepath.Base(exe), Exit: code, Out: out}}
}

// doGUI 用 ShellExecute 启动 GUI 子程序（无 CREATE_NO_WINDOW 限制，不阻塞等待），
// 等待 exe 进程退出后返回。用于 dpinst64.exe 这类 GUI 子系统的安装器。
func doGUI(workdir, exe string, args []string) (bool, string) {
	full := filepath.Join(workdir, exe)
	// 用 ShellExecute 启动（GUI 子系统的安装器不能用 CREATE_NO_WINDOW）
	parts := make([]string, 0, len(args))
	for _, a := range args {
		parts = append(parts, a)
	}
	param := strings.Join(parts, " ")
	wcs := func(s string) *uint16 { return windows.StringToUTF16Ptr(s) }
	verbU := windows.StringToUTF16Ptr("open")
	if err := windows.ShellExecute(0, verbU, wcs(full), wcs(param), wcs(workdir), 1); err != nil && err != windows.ERROR_SUCCESS { // SW_SHOWNORMAL
		return false, "ShellExecute: " + err.Error()
	}
	// 等待该 exe 进程退出（轮询任务列表）
	deadline := time.Now().Add(90 * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(1 * time.Second)
		if !isRunning(exe) {
			return true, "gui exited"
		}
	}
	return false, "gui timeout 90s"
}

func isRunning(exeBase string) bool {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return true // 拿不到快照就当还在跑
	}
	defer windows.CloseHandle(snap)
	var pe windows.ProcessEntry32
	pe.Size = uint32(unsafe.Sizeof(pe))
	if err := windows.Process32First(snap, &pe); err != nil {
		return true
	}
	for {
		name := windows.UTF16ToString(pe.ExeFile[:])
		if strings.EqualFold(filepath.Base(name), exeBase) {
			return true
		}
		if err := windows.Process32Next(snap, &pe); err != nil {
			break
		}
	}
	return false
}

func trimOut(s string) string {
	const max = 16384
	if len(s) > max {
		return s[:max] + "\n...[truncated]"
	}
	return s
}

//go:build windows

package main

import (
	"debug/pe"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"
	"unicode/utf16"
	"unicode/utf8"
	"unsafe"

	"golang.org/x/sys/windows"
)

const worldCupHardwareID = `USB\VID_1B8E&PID_C003`

type driverEnvironment struct {
	Build uint32
	Arch  string
}

func currentDriverEnvironment() driverEnvironment {
	e := driverEnvironment{Build: windows.RtlGetVersion().BuildNumber, Arch: runtime.GOARCH}
	var processMachine, nativeMachine uint16
	if windows.IsWow64Process2(windows.CurrentProcess(), &processMachine, &nativeMachine) == nil {
		switch nativeMachine {
		case pe.IMAGE_FILE_MACHINE_AMD64:
			e.Arch = "amd64"
		case pe.IMAGE_FILE_MACHINE_I386:
			e.Arch = "386"
		case pe.IMAGE_FILE_MACHINE_ARM64:
			e.Arch = "arm64"
		}
	} else if runtime.GOARCH == "386" {
		var wow64 bool
		if windows.IsWow64Process(windows.CurrentProcess(), &wow64) == nil && wow64 {
			e.Arch = "amd64"
		}
	}
	return e
}

func (e driverEnvironment) validate() error {
	if e.Arch != "amd64" && e.Arch != "386" {
		return fmt.Errorf("当前系统为 %s；随包 WorldCup 驱动只支持 Intel/AMD 的 Windows，不能用于 ARM64 Windows", e.Arch)
	}
	if e.Build < 10240 {
		return fmt.Errorf("当前 Windows 内部版本 %d 不受支持，请使用 Windows 10 或 Windows 11", e.Build)
	}
	return nil
}

// PnPUtil's slash-form verbs require Windows 10 1607. The original Win10
// releases only support the legacy verbs, including when the UI runs normally.
func driverAddArgs(build uint32, inf string) []string {
	if build < 14393 {
		return []string{"-i", "-a", inf}
	}
	return []string{"/add-driver", inf, "/install"}
}

func driverEnumArgs(build uint32) []string {
	if build < 14393 {
		return []string{"-e"}
	}
	return []string{"/enum-drivers"}
}

var publishedINFName = regexp.MustCompile(`(?i)\boem[0-9]+\.inf\b`)

// Original Win10 PnPUtil -e omits the original INF name. Check only the
// published OEM files named by Windows, rather than requiring a missing field
// or mistaking an unrelated libusb package for the WorldCup package.
func legacyWorldCupPackagePresent(output, infDirectory string) (bool, error) {
	var readError error
	for _, name := range publishedINFName.FindAllString(strings.ToLower(output), -1) {
		data, err := os.ReadFile(filepath.Join(infDirectory, name))
		if err != nil {
			readError = err
			continue
		}
		text := strings.ToLower(decodeDriverOutput(data))
		if strings.Contains(text, "worldcup_device.cat") &&
			strings.Contains(text, "vid_1b8e&pid_c003") &&
			strings.Contains(text, "libusb-win32") {
			return true, nil
		}
	}
	if readError != nil {
		return false, fmt.Errorf("无法核对 Windows 已发布的驱动 INF：%w", readError)
	}
	return false, nil
}

const driverSignerName = "WorldCup_Signer.cer"

func driverPayloadFiles(arch string) []string {
	files := []string{driverInfName, driverCatName, driverSignerName}
	if arch == "amd64" {
		files = append(files, "amd64/libusb0.sys", "amd64/libusb0.dll", "amd64/libusbK.dll", "x86/libusb0_x86.dll", "x86/libusbK_x86.dll")
	} else {
		files = append(files, "x86/libusb0.sys", "x86/libusb0_x86.dll", "x86/libusbK_x86.dll")
	}
	return files
}

func validateDriverPayload(dir, arch string) error {
	for _, name := range driverPayloadFiles(arch) {
		fi, err := os.Stat(filepath.Join(dir, filepath.FromSlash(name)))
		if err != nil || !fi.Mode().IsRegular() || fi.Size() == 0 {
			return fmt.Errorf("驱动文件缺失或为空：%s。请完整解压刷机包，不要只复制 EXE", name)
		}
	}
	return nil
}

// Resolve native Windows tools directly, never via the current directory/PATH.
// Sysnative avoids redirecting a 32-bit caller to the 32-bit PnPUtil binary.
func nativeDriverTool(name string) string {
	dir, err := windows.GetSystemDirectory()
	if err != nil {
		return "" // fail closed rather than run an executable from the package
	}
	if runtime.GOARCH == "386" && currentDriverEnvironment().Arch != "386" {
		if win, err := windows.GetWindowsDirectory(); err == nil {
			dir = filepath.Join(win, "Sysnative")
		}
	}
	return filepath.Join(dir, name+".exe")
}

func decodeDriverOutput(data []byte) string {
	start := 0
	utf16LE := len(data) >= 2 && data[0] == 0xff && data[1] == 0xfe
	if utf16LE {
		start = 2
	} else {
		// Some Windows versions redirect PnPUtil as UTF-16LE without a BOM.
		zeros, pairs := 0, 0
		for i := 1; i < len(data) && i < 128; i += 2 {
			pairs++
			if data[i] == 0 {
				zeros++
			}
		}
		utf16LE = pairs >= 4 && zeros*2 > pairs
	}
	if utf16LE {
		units := make([]uint16, (len(data)-start)/2)
		for i := range units {
			units[i] = binary.LittleEndian.Uint16(data[start+i*2:])
		}
		return strings.TrimRight(string(utf16.Decode(units)), "\x00")
	}
	if utf8.Valid(data) {
		return strings.TrimPrefix(string(data), "\ufeff")
	}
	if len(data) != 0 {
		proc := windows.NewLazySystemDLL("kernel32.dll").NewProc("MultiByteToWideChar")
		// CP_OEMCP matches redirected Windows console utilities on localized OSes.
		n, _, _ := proc.Call(1, 0, uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)), 0, 0)
		if n != 0 {
			buf := make([]uint16, n)
			if ret, _, _ := proc.Call(1, 0, uintptr(unsafe.Pointer(&data[0])), uintptr(len(data)), uintptr(unsafe.Pointer(&buf[0])), n); ret != 0 {
				return string(utf16.Decode(buf))
			}
		}
	}
	return strings.ToValidUTF8(string(data), "�")
}

type worldCupDevice struct {
	Service string
	Problem uint32
}

// Query only the USB device supported by the shipped INF. Other USB, serial,
// audio and unrelated libusb drivers are never rebound by this installer.
func connectedWorldCupDevices() ([]worldCupDevice, error) {
	set, err := windows.SetupDiGetClassDevsEx(nil, "USB", 0, windows.DIGCF_PRESENT|windows.DIGCF_ALLCLASSES, 0, "")
	if err != nil {
		return nil, err
	}
	defer set.Close()
	var devices []worldCupDevice
	for i := 0; ; i++ {
		info, err := set.EnumDeviceInfo(i)
		if errors.Is(err, windows.ERROR_NO_MORE_ITEMS) {
			return devices, nil
		}
		if err != nil {
			return nil, err
		}
		value, err := set.DeviceRegistryProperty(info, windows.SPDRP_HARDWAREID)
		if err != nil {
			continue // many unrelated USB nodes have no readable hardware ID
		}
		ids, _ := value.([]string)
		match := false
		for _, id := range ids {
			if strings.EqualFold(id, worldCupHardwareID) {
				match = true
			}
		}
		if !match {
			continue
		}
		service, _ := set.DeviceRegistryProperty(info, windows.SPDRP_SERVICE)
		d := worldCupDevice{}
		d.Service, _ = service.(string)
		var status uint32
		ret, _, _ := windows.NewLazySystemDLL("cfgmgr32.dll").NewProc("CM_Get_DevNode_Status").Call(
			uintptr(unsafe.Pointer(&status)), uintptr(unsafe.Pointer(&d.Problem)), uintptr(info.DevInst), 0)
		if ret != 0 {
			return nil, fmt.Errorf("无法读取 WorldCup 设备状态（CM 0x%x）", ret)
		}
		if status&0x400 == 0 { // DN_HAS_PROBLEM
			d.Problem = 0
		}
		devices = append(devices, d)
	}
}

var errWorldCupSignature = errors.New("Windows 拒绝加载 WorldCup 驱动，无法验证数字签名")

func driverFailurePhase(err error) string {
	if errors.Is(err, errWorldCupSignature) {
		return "signature"
	}
	return "error"
}

func worldCupDeviceError(devices []worldCupDevice) error {
	for _, d := range devices {
		if d.Problem != 0 {
			switch d.Problem {
			case 14:
				return errors.New("WorldCup 驱动需要重启 Windows 才能生效，请重启电脑后重新打开软件")
			case 52:
				return fmt.Errorf("%w（设备代码 52）", errWorldCupSignature)
			case 48:
				return errors.New("Windows 阻止了旧版 WorldCup 驱动（设备代码 48）。请检查 Windows 安全中心或联系电脑管理员")
			default:
				return fmt.Errorf("WorldCup USB 设备未就绪（设备管理器代码 %d），请点击修复驱动后重新连接音箱", d.Problem)
			}
		}
		if !strings.EqualFold(d.Service, "libusb0") {
			return errors.New("音箱当前未绑定 WorldCup 驱动，请点击“修复驱动”后按提示重新连接")
		}
	}
	return nil
}

func bindWorldCupDriver(dir string) (bool, error) {
	devices, err := connectedWorldCupDevices()
	if err != nil || len(devices) == 0 {
		return false, err // staging a package before plugging the speaker is valid
	}
	if worldCupDeviceError(devices) == nil {
		return false, nil
	}
	for _, d := range devices {
		if d.Problem == 14 && strings.EqualFold(d.Service, "libusb0") {
			return true, nil
		}
	}
	var reboot int32
	inf, err := filepath.Abs(filepath.Join(dir, driverInfName))
	if err != nil {
		return false, err
	}
	// PnPUtil /install does not replace a higher-ranked existing WinUSB driver.
	// Force only this exact hardware ID, after successfully staging our INF.
	ret, _, callErr := windows.NewLazySystemDLL("newdev.dll").NewProc("UpdateDriverForPlugAndPlayDevicesW").Call(
		0, uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(worldCupHardwareID))),
		uintptr(unsafe.Pointer(windows.StringToUTF16Ptr(inf))), 0x1|0x4, uintptr(unsafe.Pointer(&reboot)))
	if ret == 0 {
		if driverSignatureDenied(callErr, "") {
			return false, fmt.Errorf("%w：%v", errWorldCupSignature, callErr)
		}
		return false, fmt.Errorf("绑定 WorldCup 驱动失败：%w。请检查 Windows SetupAPI 日志及签名/安全策略", callErr)
	}
	if reboot != 0 {
		return true, nil
	}
	devices, err = connectedWorldCupDevices()
	if err != nil {
		return false, err
	}
	return false, worldCupDeviceError(devices)
}

// ShellExecuteEx returns a process handle so an elevated child that crashes or
// is blocked cannot leave the UI waiting three minutes for a missing JSON file.
type driverWorkerProcess interface {
	Exited() (bool, uint32, error)
	Close() error
}

type nativeDriverProcess struct{ handle windows.Handle }

func (p *nativeDriverProcess) Close() error { return windows.CloseHandle(p.handle) }
func (p *nativeDriverProcess) Exited() (bool, uint32, error) {
	state, err := windows.WaitForSingleObject(p.handle, 0)
	if err != nil || state == uint32(windows.WAIT_TIMEOUT) {
		return false, 0, err
	}
	var code uint32
	err = windows.GetExitCodeProcess(p.handle, &code)
	return true, code, err
}

type shellExecuteInfo struct {
	Size                              uint32
	Mask                              uint32
	Window                            windows.Handle
	Verb, File, Parameters, Directory *uint16
	Show                              int32
	Instance                          windows.Handle
	IDList                            unsafe.Pointer
	Class                             *uint16
	ClassKey                          windows.Handle
	HotKey                            uint32
	Icon                              windows.Handle
	Process                           windows.Handle
}

func startElevatedDriver(exe, params string, hwnd uintptr) (driverWorkerProcess, error) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	if err := windows.CoInitializeEx(0, windows.COINIT_APARTMENTTHREADED|windows.COINIT_DISABLE_OLE1DDE); err == nil {
		defer windows.CoUninitialize()
	}
	info := shellExecuteInfo{
		Mask:   0x40 | 0x100 | 0x400, // NOCLOSEPROCESS, NOASYNC, FLAG_NO_UI (UAC still shown)
		Window: windows.Handle(hwnd), Verb: windows.StringToUTF16Ptr("runas"),
		File: windows.StringToUTF16Ptr(exe), Parameters: windows.StringToUTF16Ptr(params),
		Directory: windows.StringToUTF16Ptr(filepath.Dir(exe)),
	}
	info.Size = uint32(unsafe.Sizeof(info))
	ret, _, err := windows.NewLazySystemDLL("shell32.dll").NewProc("ShellExecuteExW").Call(uintptr(unsafe.Pointer(&info)))
	if ret == 0 {
		return nil, err
	}
	if info.Process == 0 {
		return nil, errors.New("Windows 未返回驱动安装进程，无法确认管理员程序已启动")
	}
	return &nativeDriverProcess{info.Process}, nil
}

func driverIsElevated() bool {
	var token windows.Token
	if windows.OpenProcessToken(windows.CurrentProcess(), windows.TOKEN_QUERY, &token) != nil {
		return false
	}
	defer token.Close()
	return token.IsElevated()
}

// Windows reboot-success codes are not installation failures.
func driverExitSuccess(err error) (bool, bool) {
	if err == nil {
		return true, false
	}
	var exit *exec.ExitError
	if errors.As(err, &exit) && (exit.ExitCode() == 3010 || exit.ExitCode() == 1641) {
		return true, true
	}
	return false, false
}

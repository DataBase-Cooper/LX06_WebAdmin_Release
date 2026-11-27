//go:build windows

package main

import (
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows"
)

// Verify the release payload before importing a certificate as administrator.
// The pinned CER is the actual signer of the pinned CAT.
var worldCupSHA256 = map[string]string{
	"WorldCup_Device.inf": "7f8f41819c3f4185f1d0510edc1d130abd4d1b34701240a5a42f21794c7b3b03",
	"WorldCup_Device.cat": "94c6665e14124bb278b4cf0edd065a6dbf3b0d5dc35f3c33a28eb1bc4164a7f8",
	"WorldCup_Signer.cer": "e795251962d2b54048a1b2d41762c985d58881c1ab64d0d1e1828688e4cd13f4",
	"amd64/libusb0.sys":   "8058f2afe6ef96a7d2ded432997fd8655970c9ea75a938ee4557d6a2cb4cc989",
	"amd64/libusb0.dll":   "4f18b5d2c28aa66b648c8683c6d09b52b92cbbee85984bbefad5f38a64bc2a14",
	"amd64/libusbK.dll":   "15a476d769a3d67668475284fc35ee6eb840ad332cef800ba8e3bfb35450e17a",
	"x86/libusb0.sys":     "aa7d75a4d01b405aab7c848674bbed392b64c6e374e20fd72adc3c96294e2f00",
	"x86/libusb0_x86.dll": "00caca07869b19d10b370552ac7cc2f6f2ee246fc15db11650f6cd3f4ef9b666",
	"x86/libusbK_x86.dll": "a90e3d6961d1c8afb5bd6d73a5719177a3565215b7ec9b4c535a5f1347853fe9",
}

func stageWorldCupDriver(source, arch string) (string, func(), error) {
	system, err := windows.GetSystemDirectory()
	if err != nil {
		return "", nil, fmt.Errorf("无法定位 Windows 系统目录：%w", err)
	}
	// Stage only under an administrator-writable local directory. PnPUtil
	// always sees a short local path, even when the ZIP was extracted remotely.
	dir, err := os.MkdirTemp(system, "LX06-WC-")
	if err != nil {
		return "", nil, fmt.Errorf("无法暂存 WorldCup 驱动：%w", err)
	}
	cleanup := func() { _ = os.RemoveAll(dir) }
	if err := copyWorldCupDriver(source, dir, arch); err != nil {
		cleanup()
		return "", nil, err
	}
	return dir, cleanup, nil
}

func copyWorldCupDriver(source, target, arch string) error {
	const maxDriverFile = 1 << 20
	for _, name := range driverPayloadFiles(arch) {
		path := filepath.FromSlash(name)
		file, err := os.Open(filepath.Join(source, path))
		if err != nil {
			return fmt.Errorf("无法读取驱动文件 %s：%w", name, err)
		}
		data, err := io.ReadAll(io.LimitReader(file, maxDriverFile))
		file.Close()
		if err != nil || fmt.Sprintf("%x", sha256.Sum256(data)) != worldCupSHA256[name] {
			return fmt.Errorf("驱动文件校验失败：%s。请重新下载并完整解压新版刷机包", name)
		}
		destination := filepath.Join(target, path)
		if err := os.MkdirAll(filepath.Dir(destination), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(destination, data, 0o644); err != nil {
			return err
		}
	}
	return nil
}

type driverSetupLog struct {
	file   *os.File
	offset int64
}

func openDriverSetupLog() *driverSetupLog {
	dir, err := windows.GetWindowsDirectory()
	if err != nil {
		return nil
	}
	file, err := os.Open(filepath.Join(dir, "INF", "setupapi.dev.log"))
	if err != nil {
		return nil
	}
	info, err := file.Stat()
	if err != nil {
		file.Close()
		return nil
	}
	return &driverSetupLog{file: file, offset: info.Size()}
}

func (log *driverSetupLog) diagnostic(inf string) string {
	if log == nil {
		return ""
	}
	const maxAppend = 256 << 10
	info, err := log.file.Stat()
	if err != nil || info.Size() <= log.offset {
		return ""
	}
	offset := max(log.offset, info.Size()-maxAppend)
	data, _ := io.ReadAll(io.NewSectionReader(log.file, offset, maxAppend))
	return worldCupSetupErrors(decodeDriverOutput(data), inf)
}

func worldCupSetupErrors(appended, inf string) string {
	var failure, lines []string
	var matching bool
	for _, raw := range strings.Split(appended, "\n") {
		line := strings.TrimSpace(raw)
		if strings.HasPrefix(line, ">>>") && !strings.HasPrefix(line, ">>>  Section start") {
			matching = strings.Contains(strings.ToLower(line), strings.ToLower(inf))
			lines = nil
		}
		if !matching {
			continue
		}
		if strings.HasPrefix(line, "!!!") {
			lines = append(lines, line)
		}
		if strings.HasPrefix(line, "<<<") && strings.Contains(line, "[Exit status:") {
			if strings.Contains(line, "[Exit status: SUCCESS]") {
				failure = nil
			} else {
				failure = append(lines, line)
			}
			matching = false
		}
	}
	return tail(strings.Join(failure, "\n"), 4000)
}

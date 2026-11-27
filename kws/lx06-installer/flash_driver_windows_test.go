//go:build windows

package main

import (
	"errors"
	"golang.org/x/sys/windows"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDriverConsentAndInstallResults(t *testing.T) {
	for _, tc := range []struct {
		name      string
		launchErr error
		result    map[string]any
		verified  bool
		want      string
	}{
		{"cancel", windows.ERROR_CANCELLED, nil, false, "已取消管理员授权"},
		{"denied", windows.ERROR_ACCESS_DENIED, nil, false, "以管理员身份运行"},
		{"worker-failure", nil, map[string]any{"ok": false, "error": "签名验证失败"}, false, "签名验证失败"},
		{"verify-failure", nil, map[string]any{"ok": true}, false, "未检测到 WorldCup"},
		{"success", nil, map[string]any{"ok": true}, true, "驱动安装成功"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			a := &App{dataDir: t.TempDir(), drvBusy: true}
			a.driveDriverInstallWith(func(_ string, status string, _ uintptr) (driverWorkerProcess, error) {
				if tc.launchErr != nil {
					return nil, tc.launchErr
				}
				writeDriverStatus(status, tc.result)
				return nil, nil
			}, func() bool { return tc.verified })
			if a.drvBusy || a.drvInstalled != tc.verified || !strings.Contains(a.drvMsg, tc.want) {
				t.Fatalf("%+v", a.driverSnapshot())
			}
			entries, _ := os.ReadDir(a.dataDir)
			if len(entries) != 0 {
				t.Fatal("temporary install status leaked")
			}
		})
	}
	if !strings.Contains(driverLaunchError(errors.Join(errors.New("wrapped"), windows.ERROR_CANCELLED)), "已取消") {
		t.Fatal("wrapped cancellation")
	}
}

func TestDriverInstallRejectsMissingPayloadAndBusyFlash(t *testing.T) {
	a := &App{root: t.TempDir()}
	r := a.installDriver()
	if r["ok"] != false || !strings.Contains(r["error"].(string), "完整解压") {
		t.Fatal(r)
	}
	if a.drvBusy {
		t.Fatal("missing payload launched install")
	}
	a.beginFlash("official", SSHConfig{}, nil, false)
	r = a.installDriver()
	if r["ok"] != false {
		t.Fatal(r)
	}
	if _, err := os.Stat(filepath.Join(a.root, "flash-tool")); !os.IsNotExist(err) {
		t.Fatal("unexpected files")
	}
}

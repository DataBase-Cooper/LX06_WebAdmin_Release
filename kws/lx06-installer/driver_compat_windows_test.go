//go:build windows

package main

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestWorldCupDriverStoreFailureUsesCurrentSetupAPIEvidence(t *testing.T) {
	log := `>>>  [Driver Package Import - C:\Windows\System32\LX06-WC-test\WorldCup_Device.inf]
>>>  Section start 2026/11/27 12:03:45.002
     sig:      First Windows trust verification attempt.
!!!  sig:      Error 0x800b0109: The root certificate is untrusted.
!!!  sto:      Failed to import driver package. Error = 0xe0000247
<<<  Section end 2026/11/27 12:03:45.043
<<<  [Exit status: FAILURE(0xe0000247)]
>>>  [Driver Package Import - C:\Other\Vendor.inf]
!!!  sto:      Error 0x80096010
<<<  [Exit status: FAILURE(0x80096010)]
`
	diagnostic := worldCupSetupErrors(log, driverInfName)
	if !strings.Contains(diagnostic, "0x800b0109") || strings.Contains(diagnostic, "0x80096010") {
		t.Fatalf("unexpected diagnostic: %q", diagnostic)
	}
	for _, tc := range []struct{ output, want string }{
		{"", "driver-store"},
		{diagnostic, "signature"},
		{"!!!  sto: no matching digest, 0xe000024b", "driver-store"},
	} {
		dir := driverFixture(t)
		status := filepath.Join(t.TempDir(), "status.json")
		run := func(_, name string, _ ...string) (string, bool, error) {
			if name == "pnputil" {
				return tc.output, false, errors.New("exit status 0xe0000247")
			}
			return "", false, nil
		}
		code := runDriverInstall(dir, status, driverEnvironment{Build: 26100, Arch: "amd64"}, run,
			func(string) (bool, error) { t.Fatal("bind after driver store failure"); return false, nil },
			func() bool { t.Fatal("verify after driver store failure"); return false })
		data, err := os.ReadFile(status)
		var result struct {
			FailurePhase string `json:"failurePhase"`
		}
		if code != 1 || err != nil || json.Unmarshal(data, &result) != nil || result.FailurePhase != tc.want {
			t.Fatalf("want %s, status=%s err=%v", tc.want, data, err)
		}
	}
}

func TestWorldCupDriverStagingRejectsUnverifiedBytes(t *testing.T) {
	if err := copyWorldCupDriver(driverFixture(t), t.TempDir(), "amd64"); err == nil {
		t.Fatal("driver staging accepted a modified package")
	}
}

func driverFixture(t *testing.T) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "中文 驱动包")
	for _, name := range []string{driverInfName, driverCatName, "WorldCup_Signer.cer", "amd64/libusb0.sys", "amd64/libusb0.dll", "amd64/libusbK.dll", "x86/libusb0.sys", "x86/libusb0_x86.dll", "x86/libusbK_x86.dll"} {
		path := filepath.Join(dir, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte("fixture, never installed"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestDriverCompatibilityAndInstallPipeline(t *testing.T) {
	for _, build := range []uint32{10240, 14393, 19045, 22631, 26100} {
		dir := driverFixture(t)
		status := filepath.Join(t.TempDir(), "状态.json")
		var calls [][]string
		run := func(_ string, name string, args ...string) (string, bool, error) {
			calls = append(calls, append([]string{name}, args...))
			return "安装结果", name == "pnputil", nil
		}
		binds := 0
		code := runDriverInstall(dir, status, driverEnvironment{Build: build, Arch: "amd64"}, run, func(string) (bool, error) { binds++; return false, nil }, func() bool { return true })
		if code != 0 || len(calls) != 3 || binds != 0 {
			t.Fatalf("build %d: code=%d calls=%v binds=%d", build, code, calls, binds)
		}
		if calls[0][1] != "-f" || calls[1][1] != "-f" ||
			calls[0][3] != "Root" || calls[1][3] != "TrustedPublisher" {
			t.Fatalf("stores: %v", calls)
		}
		want := []string{"pnputil", "/add-driver", filepath.Join(dir, driverInfName), "/install"}
		if build < 14393 {
			want = []string{"pnputil", "-i", "-a", filepath.Join(dir, driverInfName)}
		}
		if !reflect.DeepEqual(calls[2], want) {
			t.Fatalf("build %d args=%v", build, calls[2])
		}
		var result struct {
			OK             bool
			RebootRequired bool
			Steps          []driverCommandReport
		}
		data, err := os.ReadFile(status)
		if err != nil {
			t.Fatal(err)
		}
		if err = json.Unmarshal(data, &result); err != nil || !result.OK || !result.RebootRequired || len(result.Steps) != 3 {
			t.Fatalf("result=%s err=%v", data, err)
		}
	}
}

func TestDriverPreflightAndPublisherFailureStopInstallation(t *testing.T) {
	for _, mode := range []string{"arm64", "old-windows", "missing-sys", "publisher", "binding"} {
		t.Run(mode, func(t *testing.T) {
			dir := driverFixture(t)
			status := filepath.Join(t.TempDir(), "status.json")
			env := driverEnvironment{Build: 26100, Arch: "amd64"}
			calls, binds := 0, 0
			if mode == "arm64" {
				env.Arch = "arm64"
			}
			if mode == "old-windows" {
				env.Build = 7601
			}
			if mode == "missing-sys" {
				_ = os.Remove(filepath.Join(dir, "amd64/libusb0.sys"))
			}
			run := func(_ string, name string, args ...string) (string, bool, error) {
				calls++
				if mode == "publisher" && args[2] == "TrustedPublisher" {
					return "拒绝访问", false, errors.New("publisher denied")
				}
				return "output", false, nil
			}
			bind := func(string) (bool, error) {
				binds++
				if mode == "binding" {
					return false, errors.New("wrong device binding")
				}
				return false, nil
			}
			if code := runDriverInstall(dir, status, env, run, bind, func() bool { return true }); code == 0 {
				t.Fatal("accepted failure")
			}
			if mode == "publisher" && (calls != 2 || binds != 0) {
				t.Fatal(calls, binds)
			}
			if mode != "publisher" && mode != "binding" && calls != 0 {
				t.Fatal("system changed before validation")
			}
		})
	}
}

type exitedDriverProcess struct {
	closed bool
	code   uint32
}

func (p *exitedDriverProcess) Exited() (bool, uint32, error) { return true, p.code, nil }
func (p *exitedDriverProcess) Close() error                  { p.closed = true; return nil }

func TestDriverWorkerCrashAndRestartDoNotReportReady(t *testing.T) {
	for _, mode := range []string{"crash", "reboot", "false-success"} {
		t.Run(mode, func(t *testing.T) {
			a := &App{dataDir: t.TempDir(), drvBusy: true}
			p := &exitedDriverProcess{}
			if mode == "crash" {
				p.code = 0xc0000135
			}
			if mode == "false-success" {
				p.code = 1
			}
			start := time.Now()
			a.driveDriverInstallWith(func(_ string, status string, _ uintptr) (driverWorkerProcess, error) {
				if mode == "reboot" {
					writeDriverStatus(status, map[string]any{"ok": true, "rebootRequired": true})
				}
				if mode == "false-success" {
					writeDriverStatus(status, map[string]any{"ok": true})
				}
				return p, nil
			}, func() bool { t.Fatal("unexpected readiness check"); return true })
			if time.Since(start) > time.Second || !p.closed || a.drvBusy || a.driverInstalled() {
				t.Fatal(a.driverSnapshot())
			}
			if mode == "crash" && !strings.Contains(a.drvError, "已退出") {
				t.Fatal(a.drvError)
			}
			if mode == "false-success" && !strings.Contains(a.drvError, "异常退出") {
				t.Fatal(a.drvError)
			}
			if mode == "reboot" && (!a.drvRebootRequired || a.drvPhase != "reboot") {
				t.Fatal(a.driverSnapshot())
			}
		})
	}
}

func TestDriverDeviceReadinessAndOutputEncoding(t *testing.T) {
	for _, tc := range []struct {
		device worldCupDevice
		want   string
	}{
		{worldCupDevice{Service: "libusb0"}, ""},
		{worldCupDevice{Service: "WinUSB"}, "修复驱动"},
		{worldCupDevice{Service: "libusb0", Problem: 52}, "拒绝加载"},
		{worldCupDevice{Service: "libusb0", Problem: 14}, "重启"},
	} {
		err := worldCupDeviceError([]worldCupDevice{tc.device})
		if tc.want == "" && err != nil {
			t.Fatal(err)
		}
		if tc.want != "" && (err == nil || !strings.Contains(err.Error(), tc.want)) {
			t.Fatalf("%+v: %v", tc, err)
		}
	}
	if decodeDriverOutput([]byte{0xff, 0xfe, 0x10, 0x62, 0x9f, 0x52}) != "成功" {
		t.Fatal("UTF-16 console output")
	}
	if decodeDriverOutput([]byte("驱动 installed")) != "驱动 installed" {
		t.Fatal("UTF-8 console output")
	}
	if decodeDriverOutput([]byte{'w', 0, 'o', 0, 'r', 0, 'l', 0, 'd', 0}) != "world" {
		t.Fatal("UTF-16LE without BOM")
	}
	if phase := driverFailurePhase(worldCupDeviceError([]worldCupDevice{{Problem: 52}})); phase != "signature" {
		t.Fatal(phase)
	}
	if phase := driverFailurePhase(worldCupDeviceError([]worldCupDevice{{Problem: 48}})); phase != "error" {
		t.Fatal(phase)
	}
	if !driverSignatureDenied(nil, "SetupAPI 0x800B0100") || driverSignatureDenied(nil, "Access is denied") {
		t.Fatal("driver signing failure must be distinguished from generic installation errors")
	}
	if !filepath.IsAbs(nativeDriverTool("pnputil")) {
		t.Fatal("system tool resolved through PATH")
	}
}

func TestDriverLegacyPublishedINFDetection(t *testing.T) {
	dir := t.TempDir()
	_ = os.WriteFile(filepath.Join(dir, "oem12.inf"), []byte("Provider=libusb-win32\nCatalogFile=Other.cat\nVID_1234&PID_0001"), 0o644)
	_ = os.WriteFile(filepath.Join(dir, "oem13.inf"), []byte("Provider=libusb-win32\nCatalogFile=WorldCup_Device.cat\nDeviceID=VID_1B8E&PID_C003"), 0o644)
	for _, tc := range []struct {
		output string
		want   bool
	}{
		{"Published name: oem12.inf\nDriver provider: libusb-win32", false},
		{"发布名称: OEM13.INF\n驱动程序提供商: libusb-win32", true},
		{"oem12.inf\n\noem13.inf", true},
		{"WorldCup Device (no installed published name)", false},
	} {
		got, err := legacyWorldCupPackagePresent(tc.output, dir)
		if err != nil || got != tc.want {
			t.Fatalf("%q: present=%v err=%v", tc.output, got, err)
		}
	}
	if _, err := legacyWorldCupPackagePresent("oem99.inf", dir); err == nil {
		t.Fatal("unreadable installed INF must not silently report missing package")
	}
}

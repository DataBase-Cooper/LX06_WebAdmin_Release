package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// 固件使用独立 SSH89 和 Web88 服务，共用密码，改密不重启 SSH。
// 两个程序、页面和 init 必须在只读镜像内；LAN-only 同时保护两个端口。
func TestBuildSSHBaseIntegratesWebConsole(t *testing.T) {
	buildScript, err := os.ReadFile(filepath.Join("scripts", "build_ssh_base.sh"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(buildScript)
	for _, want := range []string{
		"build_dropbear_pure.sh",
		"build_webd_standalone.sh",
		"/usr/sbin/lx06webd", // 独立 Web 服务
		"/usr/sbin/dropbear", // 独立 SSH 服务
		"/usr/lib/lx06/web",  // 页面目录
		"index.html",         // 页面
		"init.d/dropbear",    // procd init（S50，wrapper 起 89）
	} {
		if !strings.Contains(s, want) {
			t.Errorf("build_ssh_base.sh 缺少分离服务集成: %q", want)
		}
	}
	// LAN-only 由 ssh-lan.sh 统一管 89+88（webd 本身不碰 iptables）
	lan, err := os.ReadFile(filepath.Join("hooks", "ssh-lan.sh"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(lan), "for _p in 89 88") {
		t.Errorf("ssh-lan.sh 未覆盖 89+88 双端口 LAN-only 过滤")
	}
}

// TestImageContainsWebConsole 从发布镜像里直接读文件断言（真机同源的
// root.squashfs）。验证两个独立服务以及内存中的运行状态路径。
func TestImageContainsWebConsole(t *testing.T) {
	baseImg := filepath.Join("official-ssh", "root.squashfs")
	if _, err := os.Stat(baseImg); err != nil {
		t.Skipf("official-ssh image missing: %s", baseImg)
	}
	root := extractImgRoot(t, baseImg)
	// SSH 与 Web 是两个独立 ELF，由不同 procd 服务管理。
	bin, err := imgReadTree(t, root, "/usr/sbin/dropbear")
	if err != nil {
		t.Fatalf("镜像缺 /usr/sbin/dropbear: %v", err)
	}
	if !strings.HasPrefix(bin, "\x7fELF") {
		t.Fatalf("/usr/sbin/dropbear 不是 ELF 二进制（前4字节 %q）", bin[:4])
	}
	if strings.Contains(bin, "webd(HTTP 88) listening") {
		t.Error("SSH binary unexpectedly contains the Web service")
	}
	webbin, err := imgReadTree(t, root, "/usr/sbin/lx06webd")
	if err != nil || !strings.HasPrefix(webbin, "\x7fELF") || !strings.Contains(webbin, "webd(HTTP 88) listening") {
		t.Fatalf("missing independent Web ELF/service marker: %v", err)
	}
	for _, path := range []string{"/tmp/lx06-logs/.webd_session", "/tmp/lx06-wake.log", "/tmp/lx06-ww-record-state"} {
		if !strings.Contains(webbin, path) {
			t.Errorf("Web ELF missing runtime path %s", path)
		}
	}
	// 页面
	page, err := imgReadTree(t, root, "/usr/lib/lx06/web/index.html")
	if err != nil {
		t.Fatalf("镜像缺 /usr/lib/lx06/web/index.html: %v", err)
	}
	for _, want := range []string{"api/login", "api/password", "api/apps", "api/reboot"} {
		if !strings.Contains(page, want) {
			t.Errorf("页面缺少关键 API 引用: %q", want)
		}
	}
	// procd init 脚本（S50，wrapper 起 89 端口）
	init, err := imgReadTree(t, root, "/etc/init.d/dropbear")
	if err != nil {
		t.Fatalf("镜像缺 /etc/init.d/dropbear: %v", err)
	}
	for _, want := range []string{"USE_PROCD=1", "lx06-db", "LXDB_PORT=89"} {
		if !strings.Contains(init, want) {
			t.Errorf("init.d/dropbear 缺少 %q", want)
		}
	}
	webinit, err := imgReadTree(t, root, "/etc/init.d/lx06webd")
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"USE_PROCD=1", "PROG=/usr/sbin/lx06webd", "procd_set_param respawn"} {
		if !strings.Contains(webinit, want) {
			t.Errorf("Web init missing %s", want)
		}
	}
	entries, err := imgListDir(t, root, "/etc/rc.d")
	if err != nil {
		t.Fatal(err)
	}
	found := false
	for _, name := range entries {
		if name == "S65lx06webd" {
			found = true
		}
	}
	if !found {
		t.Error("independent Web startup link missing")
	}
}

// TestSSHRestartOnlyOnPortChange 断言 ssh.sh A.7：restart 条件只看端口
// （.lx06-ssh-applied），不再把 hash 变化当重启条件 —— 改密即时生效且
// 不断已建立的 SSH 连接。
func TestSSHRestartOnlyOnPortChange(t *testing.T) {
	raw, err := os.ReadFile(filepath.Join("hooks", "ssh.sh"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(raw)
	// A.7 的 restart 触发条件只剩「port changed」
	if !strings.Contains(s, "port changed (applied=") {
		t.Error("A.7 缺少「port changed」restart 分支")
	}
	if strings.Contains(s, "config changed (applied=") {
		t.Error("A.7 仍是旧「config changed（含 hash）」restart 条件 —— 改密会断 SSH")
	}
	// 新状态文件（端口 applied 记录）
	if !strings.Contains(s, "/data/.lx06-ssh-applied") {
		t.Error("A.7 未写 /data/.lx06-ssh-applied 端口状态")
	}
	if !strings.Contains(s, "改密不重启 SSH") {
		t.Error("A.7 缺少「改密不重启 SSH」分支（端口不变 + dropbear 运行中 → 无操作）")
	}
	// 端口不变 + 运行中 → start 而非 restart（未运行才 start）
	if !strings.Contains(s, "port unchanged, dropbear 未运行 → start") {
		t.Error("A.7 缺少「端口不变但未运行 → start」分支")
	}
}

// min 小工具（避免引 strings 里 Go1.21 的 min 语义歧义）
func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

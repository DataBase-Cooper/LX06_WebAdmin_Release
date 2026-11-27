package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// V20 验收断言（用户 2026-10-09 追加需求 + 2026-10-17 铁律修订）：
//
// 1) 单一开机入口铁律：用户程序（sh/C/Go/Rust/ELF）开机启动只能经
//    S60 lx06-user → init-launch.sh →（init 开机自启动开关）→ keeper →
//    /data/init.sh（或它引导的 sh）；boot 钩子只播种、rc.local 中性化，
//    不允许任何其他开机旁路。关开关 = 一次切断全部用户自启。
// 2) web88/ssh89 由 S50 dropbear 统一 ELF 提供，独立于用户 init.sh；
//    S70 看门狗在 88/89 连续 120s 无响应时先关自启动开关再 reboot -f。
// 3) 上传流程：先落 /tmp（tmpfs 运行区）→ 自动部署 /tmp→/data/lx06/apps
//    （持久区，fsync + 原子 rename）→ 成功后删除 /tmp 脚本；接收预算按
//    实机内存动态计算，单文件无固定大小上限，只查目标盘实际剩余空间。
// 4) 构建不得触碰小爱原厂服务（闹钟/定时提醒/cron 等）。

func TestV20BootAutostartEntry(t *testing.T) {
	// a) boot 钩子（S10）：同步播种 /data/init.sh（恢复出厂清空后，重启即重建，
	//    不依赖用户脚本）；用户扩展只允许从 S60 进入 keeper。
	build, err := os.ReadFile(filepath.Join("scripts", "build_ssh_base.sh"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(build)
	for _, want := range []string{
		`[ ! -e /data/init.sh ] && [ ! -L /data/init.sh ] && [ -f /lx06-init.sh ]`, // 缺失且非目录/链接才播种
		`cp /lx06-init.sh /data/init.sh`,                                           // 镜像内模板播种
		"S55lx06-keeper",                                                           // 独立监管服务
		"S60lx06-user",                                                             // 受限扩展入口
		"patch_user_boot.py",                                                       // 删除旧 root 自启旁路
	} {
		if !strings.Contains(s, want) {
			t.Errorf("boot 钩子缺少: %q", want)
		}
	}

	// b) init-launch.sh：模板播种 + legacy 升级链 + init.d 同步 + 启动
	launch, err := os.ReadFile(filepath.Join("hooks", "init-launch.sh"))
	if err != nil {
		t.Fatal(err)
	}
	l := string(launch)
	for _, want := range []string{
		"cp /lx06-init.sh /data/init.sh",  // 首次刷机播种
		"/lx06-init-legacy.sh",            // 旧模板升级链
		"mkdir -p /data/init.d",           // 业务块目录
		"/data/init.d/$(basename \"$f\")", // 烘焙自启程序同步
		"exec /bin/sh /data/init.sh",      // 用户入口 root 段先声明 resident，再进入受限 keeper
	} {
		if !strings.Contains(l, want) {
			t.Errorf("init-launch.sh 缺少: %q", want)
		}
	}

	// c) 模板本身：user_startup 业务块 + /data/init.d 循环
	tpl, err := os.ReadFile(filepath.Join("hooks", "init.sh"))
	if err != nil {
		t.Fatal(err)
	}
	tl := string(tpl)
	for _, want := range []string{
		"user_startup()",         // 常驻程序入口（原子锁保护）
		"/data/init.d",           // 一次性任务目录
		"lx06-user-startup.once", // 每 boot 原子锁
	} {
		if !strings.Contains(tl, want) {
			t.Errorf("init.sh 模板缺少: %q", want)
		}
	}

	// d) 单一开机入口铁律：init-launch.sh 里 keeper 自启调用有且只有最后一条
	//    exec 行，且 init 开机自启动开关检查必须位于它之前——开关关闭时
	//    到开关处就 exit 0，C/Go/Rust/sh 一切用户代码都不加载。
	execCount, execLine, gateLine := 0, -1, -1
	for i, line := range strings.Split(l, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "exec /bin/sh /data/init.sh") {
			execCount++
			execLine = i
		}
		if strings.Contains(line, "auto_on=0") {
			gateLine = i
		}
	}
	if execCount != 1 {
		t.Errorf("init-launch.sh 有 %d 处 keeper start 调用（开机入口必须唯一）", execCount)
	}
	if gateLine < 0 || execLine < 0 || gateLine > execLine {
		t.Errorf("自启动开关检查必须位于 keeper 启动之前（gate=%d exec=%d）", gateLine, execLine)
	}
}

func TestV20WebdUploadToTmpAndSpaceCheck(t *testing.T) {
	src, err := os.ReadFile(filepath.Join("hooks", "webd", "webd.c"))
	if err != nil {
		t.Fatal(err)
	}
	s := string(src)
	for _, want := range []string{
		`APPDIR    "/tmp/lx06-uploads"`, // 上传落 /tmp（tmpfs，不占 /data）
		"MAXBODY",                       // 历史常量：不再作为文件大小上限（预算按实机内存动态计算）
		"http_budget_init",              // 启动时按实机内存计算全连接共享接收预算
		`fs_space_kb("/data",`,          // /data 剩余空间检测（内部 statvfs）
		`fs_space_kb("/tmp",`,           // /tmp 剩余空间（WEB 页显示，重启自动清理）
		"statvfs",                       // 空间检测系统调用
		"MIN_DATA_FREE_KB",              // 空间下限
		"507",                           // Insufficient Storage
		"data_free_kb",                  // status/响应暴露剩余空间
		"tmp_free_kb",                   // status 暴露 /tmp 剩余空间
		"init_entry",                    // status 暴露 /data/init.sh 存在性
	} {
		if !strings.Contains(s, want) {
			t.Errorf("webd.c 缺少: %q", want)
		}
	}
	// 旧 /data 上传路径不得残留
	if strings.Contains(s, `APPDIR    "/data/lx06/apps"`) {
		t.Error("webd.c 仍用 /data/lx06/apps 作为上传目录（应落 /tmp）")
	}

	// 设备端页面：不再硬编码旧上限文案（接收预算由后端按实机内存动态计算）。
	page, err := os.ReadFile(filepath.Join("hooks", "webd", "index.html"))
	if err != nil {
		t.Fatal(err)
	}
	p := string(page)
	if strings.Contains(p, "2MB") || strings.Contains(p, "4MB") {
		t.Error("设备端页面仍硬编码旧上传上限文案")
	}
	// 后端：单文件无固定大小上限——只有目标盘实际剩余空间检查 + 动态共享接收预算。
	for _, want := range []string{"tmp空间不够", "data空间不足"} {
		if !strings.Contains(s, want) {
			t.Errorf("webd 缺少空间提示文案: %q", want)
		}
	}
	// 分阶段流程：先落 /tmp → 部署 /tmp→/data → 成功后删除 /tmp 脚本（顺序不得颠倒）。
	iUpload := strings.Index(s, "static void h_apps_upload")
	iTmp := strings.Index(s, "app_atomic(d, name, (const char *)dcontent, dlen, 0755)")
	iCopy := strings.Index(s, "app_copy_from_tmp(m, name, dlen)")
	iDel := strings.Index(s, "app_remove_at(APPDIR, 0700, name)")
	if iUpload < 0 || iTmp < iUpload || iCopy < iTmp || iDel < iCopy {
		t.Errorf("上传分阶段流程缺失或顺序错误: upload=%d tmp=%d copy=%d del=%d", iUpload, iTmp, iCopy, iDel)
	}

	// 成功前持久化由 webd 完成，init-launch 不再从易失的 /tmp 回迁。
	launch, _ := os.ReadFile(filepath.Join("hooks", "init-launch.sh"))
	l := string(launch)
	if !strings.Contains(l, "/data/lx06/apps") || strings.Contains(l, " -nt ") {
		t.Error("init-launch.sh 必须保留持久目录，且不能再依赖 /tmp 新旧回迁")
	}
	for _, want := range []string{"APPPERSIST", "fsync"} {
		if !strings.Contains(s, want) {
			t.Errorf("webd 缺少上传成功前的持久化步骤: %s", want)
		}
	}
}

// TestV20ImageBootChain 从发布镜像直接验证 boot 链（真机同源 root.squashfs）：
// 单一开机入口 = S60 lx06-user + keeper；rc.local 中性化；boot 钩子只播种。
func TestV20ImageBootChain(t *testing.T) {
	baseImg := filepath.Join("official-ssh", "root.squashfs")
	if _, err := os.Stat(baseImg); err != nil {
		t.Skipf("official-ssh image missing: %s", baseImg)
	}
	root := extractImgRoot(t, baseImg)

	launch, err := imgReadTree(t, root, "/usr/lib/lx06/init-launch.sh")
	if err != nil {
		t.Fatalf("镜像缺 /usr/lib/lx06/init-launch.sh: %v", err)
	}
	if !strings.Contains(launch, "/lx06-init.sh /data/init.sh") &&
		!strings.Contains(launch, "cp /lx06-init.sh /data/init.sh") {
		t.Error("镜像内 init-launch.sh 未含 /data/init.sh 播种")
	}

	// rc.local：用户代码入口已中性化——只剩指向 S60 的注释，不得残留任何
	// 活跃的 init.sh / init-launch 启动行。
	rcLocal, err := imgReadTree(t, root, "/etc/rc.local")
	if err != nil {
		t.Fatalf("镜像缺 /etc/rc.local: %v", err)
	}
	if !strings.Contains(rcLocal, "S60 lx06-user") {
		t.Error("镜像 rc.local 缺少中性化标记（应只留指向 S60 的注释）")
	}
	for _, line := range strings.Split(rcLocal, "\n") {
		s := strings.TrimSpace(line)
		if s == "" || strings.HasPrefix(s, "#") || strings.HasPrefix(s, ":") || s == "exit 0" {
			continue
		}
		if strings.Contains(s, "init.sh") || strings.Contains(s, "init-launch") || strings.Contains(s, "lx06") {
			t.Errorf("镜像 rc.local 残留活跃用户代码启动行: %q", s)
		}
	}

	// boot() 钩子（S10）：只播种 /data/init.sh，不启动用户代码（用户代码只从 S60 进入 keeper）
	boot, err := imgReadTree(t, root, "/etc/init.d/boot")
	if err != nil {
		t.Fatalf("镜像缺 /etc/init.d/boot: %v", err)
	}
	if !strings.Contains(boot, "[ ! -e /data/init.sh ]") {
		t.Error("镜像 boot() 钩子缺少 /data/init.sh 播种守卫")
	}
	if strings.Contains(boot, "init-launch.sh") {
		t.Error("镜像 boot() 钩子直接启动了用户代码（应只经 S60 lx06-user + keeper）")
	}
	// 监管链（系统级，独立于用户 init.sh）：keeper / user / watchdog 全在镜像内。
	// rc.d 条目是符号链接（Windows interop fs 打不开 unsquashfs 的链接），
	// 用目录列出名验证；真实文件按常规路径读内容。
	rcd, err := imgListDir(t, root, "/etc/rc.d")
	if err != nil {
		t.Fatalf("镜像 /etc/rc.d 读取失败: %v", err)
	}
	rcdSet := map[string]bool{}
	for _, n := range rcd {
		rcdSet[n] = true
	}
	for _, n := range []string{"S50dropbear", "S55lx06-keeper", "S60lx06-user", "S70lx06-watchdog"} {
		if !rcdSet[n] {
			t.Errorf("镜像 /etc/rc.d 缺 %s", n)
		}
	}
	for _, p := range []string{
		"/usr/bin/lx06-keeper",
		"/etc/init.d/lx06-keeper",
		"/etc/init.d/lx06-user",
		"/etc/init.d/lx06-watchdog",
		"/usr/lib/lx06/lx06-watchdog.sh",
	} {
		if _, err := imgReadTree(t, root, p); err != nil {
			t.Errorf("镜像缺监管链文件 %s: %v", p, err)
		}
	}
	// 模板在镜像内
	if _, err := imgReadTree(t, root, "/lx06-init.sh"); err != nil {
		t.Errorf("镜像缺 /lx06-init.sh 模板: %v", err)
	}
}

// TestV20ImageRcDMinimalDeltaAndVendorServices（用户 2026-10-17 铁律）：
// 固定系统服务白名单包括分离的 Web/SSH、自愈和被动 MFCC。
// 用户程序仍只能由 S60 → init.sh 进入 keeper；不能增加其他用户入口。
// 小爱原厂闹钟/定时提醒/cron 服务必须原样保留（自启动开关关闭时它们照常运行）。
func TestV20ImageRcDMinimalDeltaAndVendorServices(t *testing.T) {
	stockImg := filepath.Join("official", "root.squashfs")
	baseImg := filepath.Join("official-ssh", "root.squashfs")
	for _, p := range []string{stockImg, baseImg} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("image missing: %s", p)
		}
	}
	stockRoot := extractImgRoot(t, stockImg)
	newRoot := extractImgRoot(t, baseImg)

	stockRc, err := imgListDir(t, stockRoot, "/etc/rc.d")
	if err != nil {
		t.Fatalf("原厂镜像 rc.d 读取失败: %v", err)
	}
	newRc, err := imgListDir(t, newRoot, "/etc/rc.d")
	if err != nil {
		t.Fatalf("新镜像 rc.d 读取失败: %v", err)
	}

	want := map[string]bool{}
	for _, n := range stockRc {
		want[n] = true
	}
	for _, add := range []string{"S54lx06xiaoai", "S55lx06-keeper", "S60lx06-user", "S62lx06wakeword", "S65lx06-portwatch", "S65lx06webd", "S70lx06-watchdog", "S71xiaoai-recovery", "S84lx06officialrouter"} {
		want[add] = true
	}
	newSet := map[string]bool{}
	for _, n := range newRc {
		newSet[n] = true
	}
	for n := range newSet {
		if !want[n] {
			t.Errorf("新镜像 rc.d 出现预期外开机入口 %s（只允许显式系统服务白名单）", n)
		}
	}
	for n := range want {
		if !newSet[n] {
			t.Errorf("新镜像 rc.d 缺失 %s（原厂服务不得被构建移除）", n)
		}
	}
	// rc10's shared official-command router is a firmware service, not another
	// DATA user-script entry. Pin its startup target as well as its rc.d name.
	routerInit, err := imgReadTree(t, newRoot, "/etc/init.d/lx06officialrouter")
	if err != nil || !strings.Contains(routerInit, "procd_set_param command /usr/lib/lx06/xiaoai/official-router") || strings.Contains(routerInit, "/data/") {
		t.Errorf("official router must start the firmware binary: %v", err)
	}

	// 小爱原厂闹钟 / 定时提醒 / cron 链必须可读且在位
	for _, p := range []string{
		"/etc/init.d/alarm",           // S70alarm 闹钟服务
		"/usr/bin/alarmd",             // 闹钟守护进程
		"/etc/init.d/cron",            // S50cron
		"/etc/init.d/mico_ai_crontab", // S96mico_ai_crontab 定时提醒
	} {
		if _, err := imgReadTree(t, newRoot, p); err != nil {
			t.Errorf("原厂服务文件 %s 缺失或不可读: %v", p, err)
		}
	}
}

// TestV20DeviceSideScriptsSyntax 设备端 shell 脚本 dash 语法检查（WSL dash -n）。
func TestV20DeviceSideScriptsSyntax(t *testing.T) {
	scripts := []string{
		"hooks/init-launch.sh",
		"hooks/init.sh",
		"hooks/ssh.sh",
		"hooks/ssh-lan.sh",
	}
	for _, p := range scripts {
		abs, err := filepath.Abs(p)
		if err != nil {
			t.Fatal(err)
		}
		out, err := exec.Command("wsl.exe", "-e", "dash", "-n", imgWslPath(abs)).CombinedOutput()
		if err != nil {
			t.Errorf("dash -n %s 失败: %v\n%s", p, err, out)
		}
	}
}

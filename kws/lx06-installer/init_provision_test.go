package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestBuildInitLauncher 校验开机自启引导脚本的关键语义：
// 纯 ash、遍历 /data/init.d、.sh 用 sh 跑、其余按可执行文件跑、后台+脱离控制终端。
func TestBuildInitLauncher(t *testing.T) {
	s := buildInitLauncher()
	for _, want := range []string{
		"#!/bin/sh",
		initMarker,
		initAppDir,
		`for f in $D/*`,
		`sh "$f"`,      // 脚本分支
		`"$f" </dev/null >/dev/null 2>&1 &`, // 可执行分支 + 脱离
		"ping",         // 联网等待
	} {
		if !strings.Contains(s, want) {
			t.Errorf("launcher 缺少关键片段 %q\n---\n%s", want, s)
		}
	}
	// 设备无 base64：launcher 不得依赖 base64 解码
	if strings.Contains(s, "base64") {
		t.Errorf("launcher 不得依赖 base64（设备无此 applet）")
	}
}

// TestLoadInitApps 校验 init-apps/ 枚举：只收允许的扩展名、跳过目录与文档、
// 超大跳过、按名排序、数量上限。
func TestLoadInitApps(t *testing.T) {
	dir := t.TempDir()
	appDir := filepath.Join(dir, initAppLocalDir)
	if err := os.MkdirAll(appDir, 0o755); err != nil {
		t.Fatal(err)
	}
	// 写入若干文件
	files := map[string]int{
		"b-bin.sh": 100,      // 脚本
		"a-prog":   200,      // 无扩展名程序
		"c.bin":    300,      // 二进制
		"README.txt": 400,    // 文档 → 忽略
		"notes.md":   500,    // 文档 → 忽略
		"z.elf":      600,    // elf → 收
	}
	for name, size := range files {
		if err := os.WriteFile(filepath.Join(appDir, name), make([]byte, size), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	// 子目录 → 忽略
	if err := os.MkdirAll(filepath.Join(appDir, "subdir"), 0o755); err != nil {
		t.Fatal(err)
	}
	a := &App{root: dir}
	apps := a.loadInitApps()

	var names []string
	for _, ap := range apps {
		names = append(names, ap.Name)
	}
	// 期望按名排序，且只含 4 个可执行项
	want := []string{"a-prog", "b-bin.sh", "c.bin", "z.elf"}
	if len(apps) != len(want) {
		t.Fatalf("期望 %v，得到 %v", want, names)
	}
	for i, n := range want {
		if names[i] != n {
			t.Errorf("顺序/集合不符: 索引%d 期望 %s 得到 %s", i, n, names[i])
		}
	}
	// IsScript 标记正确
	for _, ap := range apps {
		if ap.IsScript != strings.HasSuffix(ap.Name, ".sh") {
			t.Errorf("%s 的 IsScript 标记错误: %v", ap.Name, ap.IsScript)
		}
	}
}

// TestLoadInitAppsMax 校验数量上限与超大跳过。
func TestLoadInitAppsMax(t *testing.T) {
	dir := t.TempDir()
	appDir := filepath.Join(dir, initAppLocalDir)
	if err := os.MkdirAll(appDir, 0o755); err != nil {
		t.Fatal(err)
	}
	// 10 个小程序（超过上限 8）
	for i := 0; i < 10; i++ {
		name := string(rune('a'+i)) + ".sh"
		if err := os.WriteFile(filepath.Join(appDir, name), []byte("x"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	a := &App{root: dir}
	apps := a.loadInitApps()
	if len(apps) != initMaxApps {
		t.Errorf("期望截断到上限 %d，得到 %d", initMaxApps, len(apps))
	}
}

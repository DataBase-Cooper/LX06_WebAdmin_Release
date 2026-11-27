package main

// 开机自启（init 框架）——对标 open-xiaoai-bridge 的 LX06 刷机包：
//
// 设备端  /data/init.sh 由系统 /etc/rc.local 在开机自启（official-ssh 固件已含该钩子）。
// 本框架把「开机自启的程序」统一放到设备 /data/init.d/：
//
//	/data/lx06-init/launcher.sh  ← 开机自启引导（本文件生成）
//	/data/lx06-init/launcher.sh 里逐个启动 /data/init.d/* 里的程序
//	/data/init.d/                ← 「以后要开机启动的程序都放这里」
//
// 与 SSH 配置同一次 SSH 会话安装；系统分区(root.squashfs)保持原厂不动。
// launcher 用纯 ash 语法（设备无 base64）。.sh 脚本用 sh 运行，其余按可执行文件直接运行。

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	initMarker        = "#lx06-init-hook"
	initLauncherPath  = "/data/lx06-init/launcher.sh"
	initAppDir        = "/data/init.d"
	initAppLocalDir   = "init-apps"       // 相对 root：用户把要开机自启的程序放到这里
	initAppMaxSize    = 32 << 20          // 单个自启程序 32MB 上限
	initMaxApps       = 8                 // 最多同时安装 8 个自启程序
	initUploadTimeout = 60 * time.Second
)

// initExts 是允许作为开机自启程序的扩展名（arm 静态二进制 / shell 脚本）。
var initExts = map[string]bool{
	"": true, ".sh": true, ".bin": true, ".elf": true,
}

// InitApp 是一个待安装的开机自启程序（来自 root/init-apps/）。
type InitApp struct {
	Name    string `json:"name"`
	Size    int64  `json:"size"`
	IsScript bool  `json:"isScript"`
}

// loadInitApps 枚举 root/init-apps/ 下可安装的自启程序（按文件名排序，静默）。
// 目录不存在或为空时返回空列表（开机自启功能仍可正常安装 launcher）。
// 注意：本方法会被 /api/status 轮询调用，必须无副作用（不写日志/不分配额外状态）。
func (a *App) loadInitApps() []InitApp {
	dir := filepath.Join(a.root, initAppLocalDir)
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var apps []InitApp
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		name := e.Name()
		ext := strings.ToLower(filepath.Ext(name))
		if !initExts[ext] {
			continue // 跳过说明文档(.txt/.md 等)
		}
		if fi, err := e.Info(); err == nil {
			if fi.Size() > initAppMaxSize {
				continue
			}
			apps = append(apps, InitApp{Name: name, Size: fi.Size(), IsScript: ext == ".sh"})
		}
	}
	sort.Slice(apps, func(i, j int) bool { return apps[i].Name < apps[j].Name })
	if len(apps) > initMaxApps {
		apps = apps[:initMaxApps]
	}
	return apps
}

// logInitApps 在刷机开始时一次性记录发现的自启程序。
func (a *App) logInitApps(apps []InitApp) {
	for _, ap := range apps {
		a.logf("init-apps: 开机自启程序 %s (%.1f KB, %s)",
			ap.Name, float64(ap.Size)/1024, func() string { if ap.IsScript { return "脚本" }; return "程序" }())
	}
}

// buildInitLauncher 生成开机自启引导脚本。纯 ash（设备无 base64）。
// 每个 /data/init.d/* 都以「后台 + 脱离控制终端 + 忽略 HUP」方式启动，
// 单程序崩溃不影响其它程序，也不拖慢开机。
func buildInitLauncher() string {
	return `#!/bin/sh
# LX06 一键刷机工具生成（` + initMarker + `）——开机自启引导，请勿手工修改
L=/data/lx06-init
D=/data/init.d
mkdir -p $D
# 让 SSH 配置钩子(probe.sh)先完成 dropbear 重启，再等联网、启动自启程序
sleep 3
i=0
while [ $i -lt 45 ] && ! ping -c 1 -W 2 -q baidu.com >/dev/null 2>&1; do
  i=$((i+1))
  sleep 1
done
if [ -d $D ]; then
  for f in $D/*; do
    [ -e "$f" ] || continue
    case "$f" in
      *.sh) ( sh "$f" </dev/null >/dev/null 2>&1 & ) ;;
      *)    ( "$f" </dev/null >/dev/null 2>&1 & ) ;;
    esac
  done
fi
exit 0
`
}

// installLauncher 上传开机自启引导脚本并在 /data/init.sh 追加其钩子
// （追加式，不覆盖厂商或其它第三方钩子；已有标记则跳过）。
func installLauncher(c *sshClient, logf func(string, ...any)) error {
	if err := c.writeFile(initLauncherPath, []byte(buildInitLauncher()), initUploadTimeout); err != nil {
		return fmt.Errorf("写入 %s 失败: %v", initLauncherPath, err)
	}
	back, err := c.run(sshCmdTimeout, "cat "+shQuote(initLauncherPath))
	if err != nil || !strings.Contains(back, initMarker) {
		return fmt.Errorf("回读校验 %s 失败: %v", initLauncherPath, err)
	}
	if _, err := c.run(sshCmdTimeout, "chmod +x "+shQuote(initLauncherPath)); err != nil {
		logf("launcher chmod 失败(不影响运行): %v", err)
	}
	out, _ := c.run(sshCmdTimeout, "cat /data/init.sh 2>/dev/null")
	if strings.Contains(out, initMarker) {
		logf("开机自启钩子已存在，跳过")
		return nil
	}
	line := fmt.Sprintf("[ -f %s ] && sh %s >/dev/null 2>&1 %s\n", initLauncherPath, initLauncherPath, initMarker)
	var cmd string
	if strings.TrimSpace(out) != "" {
		cmd = "printf '%s' " + shQuote(line) + " >> /data/init.sh"
	} else {
		cmd = "mkdir -p /data && printf '%s' " + shQuote(line) + " > /data/init.sh"
	}
	if out2, err := c.run(sshCmdTimeout, cmd); err != nil {
		return fmt.Errorf("写入 /data/init.sh 开机自启钩子失败: %v (%s)", err, out2)
	}
	out3, _ := c.run(sshCmdTimeout, "cat /data/init.sh")
	if !strings.Contains(out3, initMarker) {
		return fmt.Errorf("/data/init.sh 开机自启钩子写入后校验失败")
	}
	logf("开机自启钩子已追加到 /data/init.sh")
	return nil
}

// uploadInitApps 把 PC 端 init-apps/ 里的程序上传到设备 /data/init.d/，
// 脚本加 +x，二进制保持原样，并逐个回读校验大小。root 是软件运行目录。
func uploadInitApps(c *sshClient, root string, apps []InitApp, logf func(string, ...any)) error {
	if _, err := c.run(sshCmdTimeout, "mkdir -p "+initAppDir); err != nil {
		return fmt.Errorf("创建 %s 失败: %v", initAppDir, err)
	}
	for _, ap := range apps {
		data, err := os.ReadFile(filepath.Join(root, initAppLocalDir, ap.Name))
		if err != nil {
			return fmt.Errorf("读取 init-apps/%s 失败: %v", ap.Name, err)
		}
		dst := initAppDir + "/" + ap.Name
		if err := c.writeFile(dst, data, initUploadTimeout); err != nil {
			return err
		}
		// writeFile 用 umask 077 → 文件落为 0600（无可执行位）。
		// 所有自启程序都要能执行：脚本 0755、二进制 0755。
		if _, err := c.run(sshCmdTimeout, "chmod 0755 "+shQuote(dst)); err != nil {
			logf("chmod %s 失败(运行可能受影响): %v", ap.Name, err)
		}
		sizeOut, _ := c.run(sshCmdTimeout, "wc -c < "+shQuote(dst))
		if strings.TrimSpace(sizeOut) != fmt.Sprint(len(data)) {
			return fmt.Errorf("回读校验 %s 大小不一致（本地 %d / 设备 %s）", dst, len(data), strings.TrimSpace(sizeOut))
		}
		logf("已安装开机自启程序 %s → %s (%d 字节，回读一致)", ap.Name, dst, len(data))
	}
	return nil
}

// connectProvisioned 在「已应用 SSH 配置」的设备上建立新连接：
// 优先旧端口+旧密码（SSH 钩子可能还没重启 dropbear），失败则新端口+新密码。
func connectProvisioned(ip string, newPort int, newPass string, oldPass string) (*sshClient, error) {
	c, err := sshDial(ip, sshDefaultPort, "root", oldPass, 4*time.Second)
	if err == nil {
		return c, nil
	}
	return sshDial(ip, newPort, "root", newPass, 6*time.Second)
}

// provisionInit 安装开机自启框架：上传 init-apps/ 程序 + 安装 launcher + 挂钩子，
// 并立即运行一次（使本次刷完重启后、设备在线期间就生效，同时验证框架可用）。
func provisionInit(root string, apps []InitApp, newPort int, newPass, oldPass string, ip string, logf func(string, ...any)) error {
	logf("开机自启(init): 待安装程序 %d 个", len(apps))
	cl, err := connectProvisioned(ip, newPort, newPass, oldPass)
	if err != nil {
		return fmt.Errorf("连上音箱安装开机自启失败: %v", err)
	}
	defer cl.Close()
	if err := uploadInitApps(cl, root, apps, logf); err != nil {
		return err
	}
	if err := installLauncher(cl, logf); err != nil {
		return err
	}
	logf("立即在后台运行一次开机自启引导（验证 + 本次即生效）…")
	// 双重 fork 脱离 SSH 会话（同 rc.local 的 `sh /data/init.sh &` 手法）：
	// launcher 有 sleep+联网等待(最长约 48s)，若同步执行会超过 SSH 命令超时；
	// 后台化后本命令立即返回，launcher 被 init(pid 1) 收养、跨会话存活。
	if out, err := cl.run(sshCmdTimeout, "sh -c '( sh "+initLauncherPath+" </dev/null >/dev/null 2>&1 & )' </dev/null"); err != nil {
		logf("触发 launcher(会话断开为预期): %v", out)
	}
	// 再次确认钩子仍在（launcher 运行不应改动 /data/init.sh）
	cl2, err := connectProvisioned(ip, newPort, newPass, oldPass)
	if err != nil {
		return fmt.Errorf("开机自启校验连接失败: %v", err)
	}
	defer cl2.Close()
	out, _ := cl2.run(sshCmdTimeout, "cat /data/init.sh 2>/dev/null; ls -1 "+initAppDir+" 2>/dev/null")
	if !strings.Contains(out, initMarker) {
		return fmt.Errorf("开机自启钩子校验失败（/data/init.sh 未见标记）")
	}
	logf("开机自启框架安装完成（%s 内程序将随小爱开机自动运行）", initAppDir)
	return nil
}

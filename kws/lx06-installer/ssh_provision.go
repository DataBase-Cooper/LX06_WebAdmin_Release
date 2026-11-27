package main

// Post-flash SSH provisioning for the official-ssh package.
//
// The patched root.squashfs boots dropbear (port 22, root/DB_COOPER recovery
// password, host key under /data/etc/dropbear) for one-click SSH access. After
// tool connects over SSH and installs, into the writable /data:
//
//   /data/lx06-ssh/shadow        -> bind-mount over /etc/shadow  (root password)
//   /data/lx06-ssh/dropbear.conf -> bind-mount over /etc/config/dropbear (port)
//   /data/lx06-ssh/probe.sh      boot hook: bind mounts + firewall + dropbear
//
// Boot order (verified on device): /etc/rc.d/S95done runs LAST and sources
// /etc/rc.local, which runs `sh /data/init.sh &` AFTER dropbear has already
// started on the stock config. probe.sh therefore re-applies everything and
// restarts dropbear so the bind-mounted shadow/port go live from the first
// boot. The hook APPENDS to /data/init.sh (vendor hooks are never clobbered).
//
// Firewall: LAN-only is enforced by gating ONLY the SSH port, never the
// voice port (8899) or anything else, so device functions are untouched.
// The LAN subnet is re-derived from the current wlan0 lease at every boot
// (DHCP-safe). All on-device scripts are ash-safe.
//
// Every write is idempotent: probe.sh re-applies from /data/lx06-ssh on
// every boot, and the tool can re-provision an already-configured device.

import (
	"fmt"
	"strings"
	"time"
)

type sshProvisionConfig struct {
	Host    string
	OldPass string // current password (DB_COOPER on a fresh SSH flash)
	NewPass string
	Port    int
	LANOnly bool
	OldPort int // 0 = default 22 (fresh flash); a re-provisioned device may already listen elsewhere
}

const (
	sshHookMarker  = "#lx06-ssh-hook"
	sshDataDir     = "/data/lx06-ssh"
	sshProbeScript = sshDataDir + "/probe.sh"
	sshCmdTimeout  = 25 * time.Second
)

// buildProbeScript renders the ash boot hook. The dropbear restart is
// essential: rc.local (S95done) runs after dropbear's rcS start, so without
// the restart the stock port-22/shadow config would stay live forever. The
// iptables gate is applied BEFORE the restart so it is in place the moment
// the (possibly new) port opens.
func buildProbeScript(port int, lanOnly bool) string {
	var sb strings.Builder
	sb.WriteString("#!/bin/sh\n")
	sb.WriteString("# LX06 一键刷机工具生成（" + sshHookMarker + "）——请勿手工修改\n")
	sb.WriteString("D=" + sshDataDir + "\n")
	sb.WriteString("mkdir -p $D\n")
	sb.WriteString("mkdir -p /data/etc/dropbear\n")
	sb.WriteString("mount -o bind,ro $D/shadow /etc/shadow 2>/dev/null\n")
	sb.WriteString("mount -o bind $D/dropbear.conf /etc/config/dropbear 2>/dev/null\n")
	if lanOnly {
		sb.WriteString(fmt.Sprintf("PORT=%d\n", port))
		sb.WriteString("LANIP=$(ip addr show wlan0 2>/dev/null | awk '/inet /{print $2; exit}')\n")
		sb.WriteString("SUBNET=$(echo \"$LANIP\" | sed 's/\\.[0-9]*\\///')\n")
		sb.WriteString("if [ -n \"$SUBNET\" ]; then\n")
		sb.WriteString("  printf '*filter\\n:INPUT ACCEPT [0:0]\\n-A INPUT -i wlan0 -p tcp -m tcp --dport %s -s %s/24 -j ACCEPT\\n-A INPUT -p tcp -m tcp --dport %s -j DROP\\nCOMMIT\\n' \"$PORT\" \"$SUBNET\" \"$PORT\" | iptables-restore 2>/dev/null\n")
		sb.WriteString("fi\n")
	} else {
		sb.WriteString("printf '*filter\\n:INPUT ACCEPT [0:0]\\nCOMMIT\\n' | iptables-restore 2>/dev/null\n")
	}
	sb.WriteString("/etc/init.d/dropbear restart 2>/dev/null\n")
	sb.WriteString("exit 0\n")
	return sb.String()
}

// buildDropbearConf renders the uci dropbear config bind-mounted over
// /etc/config/dropbear. Mirrors the stock file exactly except Port.
func buildDropbearConf(port int) string {
	var sb strings.Builder
	sb.WriteString("config dropbear\n")
	sb.WriteString("\toption PasswordAuth '1'\n")
	sb.WriteString("\toption RootPasswordAuth '1'\n")
	sb.WriteString(fmt.Sprintf("\toption Port         '%d'\n", port))
	sb.WriteString("\toption BannerFile   '/etc/banner'\n")
	sb.WriteString("\toption rsakeyfile '/data/etc/dropbear/dropbear_rsa_host_key'\n")
	return sb.String()
}

// buildShadow renders a full /etc/shadow with only the root hash replaced.
// The other system accounts are fixed in the image (verified byte-for-byte).
func buildShadow(rootHash string) string {
	return "root:" + rootHash + ":18128:0:99999:7:::\n" +
		"daemon:*:0:0:99999:7:::\n" +
		"ftp:*:0:0:99999:7:::\n" +
		"network:*:0:0:99999:7:::\n" +
		"nobody:*:0:0:99999:7:::\n"
}

// ensureHook makes sure /data/init.sh calls the probe script, appending a
// marker line when absent (vendor hooks are never clobbered).
func ensureHook(c *sshClient, logf func(string, ...any)) error {
	out, _ := c.run(sshCmdTimeout, "cat /data/init.sh 2>/dev/null")
	if strings.Contains(out, sshHookMarker) {
		logf("启动钩子已存在，跳过")
		return nil
	}
	exists := strings.TrimSpace(out) != ""
	line := fmt.Sprintf("[ -f %s ] && sh %s >/dev/null 2>&1 %s\n", sshProbeScript, sshProbeScript, sshHookMarker)
	var cmd string
	if exists {
		cmd = "printf '%s' " + shQuote(line) + " >> /data/init.sh"
	} else {
		cmd = "mkdir -p /data && printf '%s' " + shQuote(line) + " > /data/init.sh"
	}
	if out2, err := c.run(sshCmdTimeout, cmd); err != nil {
		return fmt.Errorf("写入 /data/init.sh 钩子失败: %v (%s)", err, out2)
	}
	out3, _ := c.run(sshCmdTimeout, "cat /data/init.sh")
	if !strings.Contains(out3, sshHookMarker) {
		return fmt.Errorf("/data/init.sh 钩子写入后校验失败")
	}
	logf("启动钩子已追加到 /data/init.sh")
	return nil
}

// writeConfigFiles uploads the /data/lx06-ssh config set and verifies each
// file by reading it back.
func writeConfigFiles(c *sshClient, cfg sshProvisionConfig, shadow, probe string, logf func(string, ...any)) error {
	if _, err := c.run(sshCmdTimeout, "mkdir -p "+sshDataDir); err != nil {
		return fmt.Errorf("创建 %s 失败: %v", sshDataDir, err)
	}
	files := []struct {
		name string
		data string
	}{
		{"shadow", shadow},
		{"dropbear.conf", buildDropbearConf(cfg.Port)},
		{"probe.sh", probe},
	}
	for _, f := range files {
		path := sshDataDir + "/" + f.name
		if err := c.writeFile(path, []byte(f.data), sshCmdTimeout); err != nil {
			return err
		}
		back, err := c.run(sshCmdTimeout, "cat "+shQuote(path))
		if err != nil || strings.TrimSpace(back) != strings.TrimSpace(f.data) {
			return fmt.Errorf("回读校验 %s 失败: %v", path, err)
		}
		logf("已写入 %s (%d 字节，回读一致)", path, len(f.data))
	}
	return nil
}

// provisionSSH connects to the freshly-flashed device and applies the SSH
// settings. It is safe to call again on an already-provisioned device
// (idempotent): files are overwritten, the hook is appended once, probe.sh
// is re-run.
func provisionSSH(cfg sshProvisionConfig, logf func(string, ...any)) error {
	logf("SSH 配置: 端口 %d，仅局域网=%v，密码已设置", cfg.Port, cfg.LANOnly)

	// Connect on the current port. Fresh flash → 22 + DB_COOPER recovery
	// password; a re-provisioned device → the previously set port/password.
	probePort := cfg.OldPort
	if probePort == 0 {
		probePort = 22
	}
	cl, err := sshDial(cfg.Host, probePort, "root", cfg.OldPass, 8*time.Second)
	if err != nil && cfg.NewPass != cfg.OldPass {
		logf("初始密码连接失败，尝试已配置密码（设备可能已配置过）")
		cl, err = sshDial(cfg.Host, probePort, "root", cfg.NewPass, 8*time.Second)
	}
	if err != nil {
		return err
	}
	defer cl.Close()

	// Sanity: root + dropbear must be present (guards against the plain
	// official package or a bad boot).
	idOut, err := cl.run(sshCmdTimeout, "id; ls /usr/sbin/dropbear")
	if err != nil || !strings.Contains(idOut, "uid=0(root)") {
		return fmt.Errorf("设备环境检查失败（需要 root 用户与 dropbear）: %v", err)
	}

	shadow := buildShadow(md5CryptHash(cfg.NewPass, randomSalt()))
	probe := buildProbeScript(cfg.Port, cfg.LANOnly)

	if err := writeConfigFiles(cl, cfg, shadow, probe, logf); err != nil {
		return err
	}
	if err := ensureHook(cl, logf); err != nil {
		return err
	}

	// Apply now: bind mounts + firewall + dropbear restart. The restart
	// drops our own session — probe.sh runs detached so the SSH command may
	// die with the daemon; that is not an error.
	logf("应用启动配置（bind mount + 防火墙 + 重启 dropbear）…")
	if out, err := cl.run(sshCmdTimeout, "sh -c 'sh "+sshProbeScript+" >/dev/null 2>&1' </dev/null"); err != nil {
		logf("dropbear 重启导致当前会话断开（预期）: %v", err)
		_ = out
	}

	// Verify login on the NEW port with the NEW password (fresh dial — the
	// old client is dead after the restart).
	logf("验证登录 root@%s:%d（新密码）…", cfg.Host, cfg.Port)
	if err := verifySSHLogin(cfg.Host, cfg.Port, cfg.NewPass); err != nil {
		return err
	}

	// Confirm the firewall gate is in place (LAN-only on → SSH port is
	// subnet-accept + drop; off → clean table).
	cl2, err := sshDial(cfg.Host, cfg.Port, "root", cfg.NewPass, 4*time.Second)
	if err != nil {
		return fmt.Errorf("防火墙校验连接失败: %v", err)
	}
	defer cl2.Close()
	rules, _ := cl2.run(sshCmdTimeout, "iptables-save")
	if cfg.LANOnly {
		if !strings.Contains(rules, fmt.Sprintf("--dport %d -j DROP", cfg.Port)) {
			return fmt.Errorf("防火墙未拦截 SSH 端口 %d（缺少 DROP 规则）: %s", cfg.Port, rules)
		}
		if !strings.Contains(rules, "/24") || !strings.Contains(rules, fmt.Sprintf("--dport %d", cfg.Port)) {
			return fmt.Errorf("防火墙缺少局域网放行规则: %s", rules)
		}
		logf("防火墙校验通过（SSH 端口 %d 仅 %s 放行，其余 DROP）", cfg.Port, subnetOf(rules))
	} else {
		logf("防火墙校验通过（未启用仅局域网，端口 %d 全网可达）", cfg.Port)
	}
	return nil
}

// subnetOf extracts the "-s X/24" source from an iptables-save dump.
func subnetOf(rules string) string {
	for _, line := range strings.Split(rules, "\n") {
		if strings.Contains(line, "-s ") {
			f := strings.Fields(line)
			for i, t := range f {
				if t == "-s" && i+1 < len(f) {
					return f[i+1]
				}
			}
		}
	}
	return "本机网段"
}

// verifySSHLogin dials the new port/password in a retry loop and runs
// `true` once connected.
func verifySSHLogin(host string, port int, password string) error {
	deadline := time.Now().Add(20 * time.Second)
	var lastErr error
	for time.Now().Before(deadline) {
		cl, err := sshDial(host, port, "root", password, 4*time.Second)
		if err == nil {
			_, rerr := cl.run(sshCmdTimeout, "true")
			cl.Close()
			if rerr == nil {
				return nil
			}
			lastErr = rerr
		} else {
			lastErr = err
		}
		time.Sleep(1500 * time.Millisecond)
	}
	return fmt.Errorf("新端口 %d 登录验证失败: %v", port, lastErr)
}

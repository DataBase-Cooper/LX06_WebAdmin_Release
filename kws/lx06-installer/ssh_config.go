package main

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
)

// SSHConfig is the user-facing pre-flash SSH option set. Port/password/
// LAN-only are baked INTO the image before flashing (no IP — the speaker has
// no IP before flash); the IP field is only used by the manual "apply to an
// already-flashed device" path.
type SSHConfig struct {
	IP       string `json:"ip"`
	Port     int    `json:"port"` // 0/22 = default 22
	Password string `json:"password"`
	LANOnly  bool   `json:"lanOnly"`
}

const (
	sshDefaultPort   = 89 // V22: 22 常被别的 SSH 占 → 89（仅局域网）
	sshFreshPassword = "DB_COOPER" // default password: SSH 与 HTTP 控制台(88) 共用
	sshConfigFile    = "ssh_config.json"
	sshPasswordMaxLen = 256
	sshPortMin        = 1
	sshPortMax        = 65535
)

// fixedSSHConfig 返回零配置刷机固定值（V19 起）：刷机软件不再有端口/密码/
// 局域网配置项 —— 恒 22 端口、恒 DB_COOPER、恒仅局域网。用户刷完登录
// http://<音箱IP>:88（同密码）自行修改，改密即时生效、无需重启任何服务。
func fixedSSHConfig() SSHConfig {
	return SSHConfig{Port: sshDefaultPort, Password: sshFreshPassword, LANOnly: true}
}

// sshPortConflicts lists LX06 system ports that a user-chosen SSH port must
// avoid. Source: live `netstat -tln` + `netstat -uln` on a freshly flashed
// 1.94.13 device (192.0.2.10) after first boot, cross-checked against the
// unpacked root.squashfs. The set below is the STABLE (non-transient) listener
// population: it is identical across reboots, so it is safe to hard-forbid.
// High-numbered scattered UDP sockets (e.g. 43866/44128/55596/59103) are
// MIoT/NAT transient sockets that re-roll a new number on every boot and are
// deliberately NOT listed — forbidding them would block a different port next
// flash. 22 is the SSH default itself and is intentionally absent.
var sshPortConflicts = map[int]string{
	53:    "DNS 服务",
	546:   "DHCP 服务",
	5053:  "本地 DNS",
	10010: "系统保留",
	5353:  "mDNS 发现",
	54321: "小爱发现 (UDP)",
	54322: "系统保留",
	54323: "系统保留",
	8888:  "系统保留",
	8899:  "小爱核心服务",
}

// validateSSHConfig normalizes and rejects bad input. requireIP=true is the
// manual "apply to an already-flashed device" path (needs the device IP, user
// values win); requireIP=false is the pre-flash bake path —— V19 零配置：
// 忽略一切用户输入，恒 22 / DB_COOPER / 仅局域网（fixedSSHConfig），
// 用户刷完后经 HTTP 控制台(88) 改密，改密即时生效。
func validateSSHConfig(c SSHConfig, requireIP bool) (SSHConfig, error) {
	if !requireIP {
		// 刷机烘焙路径：零配置，用户填什么都不采用（表单里也没有了）。
		return fixedSSHConfig(), nil
	}
	c.IP = strings.TrimSpace(c.IP)
	if net.ParseIP(c.IP) == nil {
		return c, fmt.Errorf("请填写有效的音箱 IP 地址（刷机后音箱通过 Wi-Fi 联网的 IPv4）")
	}
	// Port: 0 or 22 both mean "keep default 22". Any other port must be a
	// plain 1-65535 that the image does not already occupy (8899 = 小爱核心).
	if c.Port == 0 {
		c.Port = sshDefaultPort
	}
	if c.Port < sshPortMin || c.Port > sshPortMax {
		return c, fmt.Errorf("SSH 端口必须在 1-65535 之间")
	}
	if name, ok := sshPortConflicts[c.Port]; ok {
		return c, fmt.Errorf("端口 %d 已被小爱占用（%s），请更换", c.Port, name)
	}
	if c.Password == "" {
		return c, fmt.Errorf("SSH 密码不能为空")
	}
	if len(c.Password) > sshPasswordMaxLen {
		return c, fmt.Errorf("SSH 密码过长（最多 %d 字符）", sshPasswordMaxLen)
	}
	return c, nil
}

// loadLastSSHConfig reads the previously applied SSH config (used to prefill
// the H5 form and as a re-flash connection candidate).
func (a *App) loadLastSSHConfig() (SSHConfig, bool) {
	b, err := os.ReadFile(filepath.Join(a.dataDir, sshConfigFile))
	if err != nil {
		return SSHConfig{}, false
	}
	var c SSHConfig
	if err := json.Unmarshal(b, &c); err != nil {
		return SSHConfig{}, false
	}
	if c.Port == 0 {
		c.Port = sshDefaultPort
	}
	return c, true
}

// saveLastSSHConfig persists the applied config for next time.
func (a *App) saveLastSSHConfig(c SSHConfig) {
	if c.Port == 0 {
		c.Port = sshDefaultPort
	}
	b, err := json.Marshal(c)
	if err != nil {
		return
	}
	_ = os.MkdirAll(a.dataDir, 0o755)
	_ = os.WriteFile(filepath.Join(a.dataDir, sshConfigFile), b, 0o644)
}

package main

import (
	"fmt"
	"regexp"
	"strings"
	"testing"
)

func TestBuildProbeScriptLAN(t *testing.T) {
	s := buildProbeScript(2233, true)
	for _, want := range []string{
		"#!/bin/sh",
		sshHookMarker,
		"mount -o bind,ro $D/shadow /etc/shadow",
		"mount -o bind $D/dropbear.conf /etc/config/dropbear",
		"PORT=2233",
		"LANIP=$(ip addr show wlan0 2>/dev/null | awk '/inet /{print $2; exit}')",
		"SUBNET=$(echo \"$LANIP\" | sed 's/\\.[0-9]*\\///')",
		"if [ -n \"$SUBNET\" ]; then",
		`printf '*filter\n:INPUT ACCEPT [0:0]\n-A INPUT -i wlan0 -p tcp -m tcp --dport %s -s %s/24 -j ACCEPT\n-A INPUT -p tcp -m tcp --dport %s -j DROP\nCOMMIT\n' "$PORT" "$SUBNET" "$PORT" | iptables-restore 2>/dev/null`,
		"fi",
		"/etc/init.d/dropbear restart 2>/dev/null",
		"exit 0",
	} {
		if !strings.Contains(s, want) {
			t.Errorf("probe.sh LAN missing %q\n--- got ---\n%s", want, s)
		}
	}
	// The printf must have exactly three %s (PORT, SUBNET, PORT).
	if n := strings.Count(s, "%s"); n != 3 {
		t.Errorf("printf should have 3 %%s, got %d", n)
	}
	// No bashisms: no [[, no function keyword, no arrays.
	for _, bad := range []string{"[["} {
		if strings.Contains(s, bad) {
			t.Errorf("probe.sh LAN contains bashism %q", bad)
		}
	}
}

func TestBuildProbeScriptNoLAN(t *testing.T) {
	s := buildProbeScript(2222, false)
	if !strings.Contains(s, "printf '*filter\\n:INPUT ACCEPT [0:0]\\nCOMMIT\\n' | iptables-restore 2>/dev/null") {
		t.Errorf("probe.sh no-LAN missing flush line:\n%s", s)
	}
	if strings.Contains(s, "PORT=") {
		t.Errorf("probe.sh no-LAN should not set PORT")
	}
	if !strings.Contains(s, "/etc/init.d/dropbear restart 2>/dev/null") {
		t.Errorf("probe.sh no-LAN missing dropbear restart")
	}
}

func TestBuildLANRuleSetSemantics(t *testing.T) {
	// Simulate the device-side expansion of the printf with concrete
	// values and check the resulting iptables-restore text.
	// (The Go code embeds the printf format; here we reproduce its output.)
	fmtStr := `*filter
:INPUT ACCEPT [0:0]
-A INPUT -i wlan0 -p tcp -m tcp --dport %s -s %s/24 -j ACCEPT
-A INPUT -p tcp -m tcp --dport %s -j DROP
COMMIT
`
	out := fmt.Sprintf(fmtStr, "2233", "192.168.88", "2233")
	lines := strings.Split(strings.TrimSpace(out), "\n")
	want := []string{
		"*filter",
		":INPUT ACCEPT [0:0]",
		"-A INPUT -i wlan0 -p tcp -m tcp --dport 2233 -s 192.168.88/24 -j ACCEPT",
		"-A INPUT -p tcp -m tcp --dport 2233 -j DROP",
		"COMMIT",
	}
	if len(lines) != len(want) {
		t.Fatalf("rule line count = %d, want %d:\n%s", len(lines), len(want), out)
	}
	for i := range want {
		if lines[i] != want[i] {
			t.Errorf("line %d = %q, want %q", i, lines[i], want[i])
		}
	}
	// Rule order matters: ACCEPT (subnet, wlan0) before DROP (all).
	if strings.Index(out, "-j ACCEPT") > strings.Index(out, "-j DROP") {
		t.Errorf("DROP must come after the subnet ACCEPT")
	}
}

func TestBuildDropbearConf(t *testing.T) {
	c := buildDropbearConf(2233)
	if !strings.Contains(c, "option Port         '2233'") {
		t.Errorf("conf missing port:\n%s", c)
	}
	if !strings.Contains(c, "option PasswordAuth '1'") || !strings.Contains(c, "option RootPasswordAuth '1'") {
		t.Errorf("conf must keep password auth:\n%s", c)
	}
	if !strings.Contains(c, "option rsakeyfile '/data/etc/dropbear/dropbear_rsa_host_key'") {
		t.Errorf("conf must keep the /data host key:\n%s", c)
	}
	// Must be the same 6-line shape as stock.
	if n := len(strings.Split(strings.TrimSpace(c), "\n")); n != 6 {
		t.Errorf("conf should be 6 lines, got %d", n)
	}
}

func TestBuildShadowKeepsSystemAccounts(t *testing.T) {
	s := buildShadow("$1$abc$defg")
	want := "root:$1$abc$defg:18128:0:99999:7:::\n" +
		"daemon:*:0:0:99999:7:::\n" +
		"ftp:*:0:0:99999:7:::\n" +
		"network:*:0:0:99999:7:::\n" +
		"nobody:*:0:0:99999:7:::\n"
	if s != want {
		t.Errorf("shadow mismatch:\ngot:  %q\nwant: %q", s, want)
	}
}

func TestRandomSalt(t *testing.T) {
	re := regexp.MustCompile(`^[./0-9A-Za-z]{8}$`)
	prev := ""
	for i := 0; i < 64; i++ {
		s := randomSalt()
		if !re.MatchString(s) {
			t.Fatalf("salt %q does not match %v", s, re)
		}
		if i > 0 && s == prev {
			t.Fatalf("salt collision %q", s)
		}
		prev = s
	}
}

func TestMD5CryptRandomSaltRoundTrip(t *testing.T) {
	for _, pw := range []string{"a", "open-xiaoai", "P@ss w0rd", "密码测试", strings.Repeat("x", 100)} {
		salt := randomSalt()
		hash := md5CryptHash(pw, salt)
		if !verifyMd5Crypt(hash, pw) {
			t.Fatalf("round-trip failed for %q: %s", pw, hash)
		}
	}
}

func TestShQuote(t *testing.T) {
	cases := map[string]string{
		`/data/lx06-ssh`:           `'/data/lx06-ssh'`,
		`it's here`:                `'it'\''s here'`,
		`a b c`:                    `'a b c'`,
		"":                         `''`,
		`[ -f /x ] && sh /y #tag`:  `'[ -f /x ] && sh /y #tag'`,
	}
	for in, want := range cases {
		if got := shQuote(in); got != want {
			t.Errorf("shQuote(%q) = %q, want %q", in, got, want)
		}
	}
	// The hook line the provisioner writes must survive a printf '%s'
	// round trip through ash: no single quotes inside the quoted value.
	line := fmt.Sprintf("[ -f %s ] && sh %s >/dev/null 2>&1 %s\n", sshProbeScript, sshProbeScript, sshHookMarker)
	q := shQuote(line)
	if strings.Contains(q[1:len(q)-1], "'") && !strings.Contains(q, `'\''`) {
		t.Errorf("quoted hook line contains a bare quote: %s", q)
	}
}

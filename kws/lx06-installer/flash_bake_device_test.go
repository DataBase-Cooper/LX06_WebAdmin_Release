package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"testing"
)

// TestBakeSSHConfZeroConfig pins the V19 zero-config bake: no matter what
// SSH config the caller passes (the flasher no longer collects any), the
// appended image always carries the fixed 22 / DB_COOPER / LAN-only conf.
// Runs the real mksquashfs append pipeline against the shipped image.
func TestBakeSSHConfZeroConfig(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("mksquashfs.exe pipeline is Windows-side (user machine)")
	}
	for _, p := range []string{"official-ssh/root.squashfs", "tools/mksquashfs.exe"} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("package asset missing (%s) — run from the packaged root", p)
		}
	}
	dataDir := filepath.Join(mustAbs(t, "."), "data", "test-bake")
	os.RemoveAll(dataDir)
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(dataDir) })
	a, err := NewApp(mustAbs(t, "."), dataDir)
	if err != nil {
		t.Fatalf("NewApp: %v", err)
	}
	pkg, ok := a.findPackage("official-ssh")
	if !ok || !pkg.Present {
		t.Skipf("official-ssh package not present")
	}
	// varied legacy inputs (what old UIs might still send) — all must bake
	// to the fixed zero-config values.
	for _, cfg := range []SSHConfig{
		{},
		{Port: 22, Password: "DB_COOPER", LANOnly: true},
		{Port: 2222, Password: "legacy-pass", LANOnly: false},
	} {
		cfg, err := validateSSHConfig(cfg, false) // bake path → fixed
		if err != nil {
			t.Fatalf("validate bake: %v", err)
		}
		outImg, tmp, err := a.bakeSSHConf(pkg, cfg, nil, t.Logf)
		if err != nil {
			t.Fatalf("bake: %v", err)
		}
		if fi, e := os.Stat(outImg); e != nil || fi.Size() < 10<<20 {
			os.RemoveAll(tmp)
			t.Fatalf("merged image size %v — append likely dropped the base", fi)
		}
		conf, err := readImgFile(t, outImg, "/lx06-ssh.conf")
		os.RemoveAll(tmp)
		if err != nil {
			t.Fatalf("read baked conf: %v", err)
		}
		for _, want := range []string{
			"ssh_port=89\n",
			"ssh_lan_only=1\n",
		} {
			if !strings.Contains(conf, want) {
				t.Errorf("baked conf missing %q\nconf:\n%s", want, conf)
			}
		}
		// hash must authenticate DB_COOPER (the fixed default password)
		if m := regexp.MustCompile(`ssh_root_hash=(\$1\$[^\s]+)`).FindStringSubmatch(conf); m == nil || !verifyMd5Crypt(m[1], "DB_COOPER") {
			t.Errorf("baked conf root hash does not authenticate DB_COOPER\nconf:\n%s", conf)
		}
	}
}

func TestBuildSSHBaseBakesSSHImplementationAndDBCooperDefault(t *testing.T) {
	buildScript, err := os.ReadFile(filepath.Join("scripts", "build_ssh_base.sh"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(buildScript), "hooks/ssh.sh") {
		t.Fatal("build_ssh_base.sh does not bake hooks/ssh.sh into the read-only image")
	}
	if !strings.Contains(string(buildScript), `sh /usr/lib/lx06/ssh.sh\n`) {
		t.Fatal("boot hook does not call the canonical ssh.sh implementation")
	}
	if strings.Contains(string(buildScript), `sh /usr/lib/lx06/ssh-apply.sh`) {
		t.Fatal("boot hook still routes SSH setup through the legacy wrapper")
	}
	if !strings.Contains(string(buildScript), `ssh_root_hash=$1$`) {
		t.Fatal("build_ssh_base.sh has no MD5-crypt default hash")
	}
	if strings.Contains(string(buildScript), "password open-xiaoai") {
		t.Fatal("legacy open-xiaoai SSH default remains in build_ssh_base.sh")
	}
	hashMatch := regexp.MustCompile(`ssh_root_hash=(\$1\$[^\s]+)`).FindStringSubmatch(string(buildScript))
	if hashMatch == nil || !verifyMd5Crypt(hashMatch[1], "DB_COOPER") {
		t.Fatal("build_ssh_base.sh default root hash does not authenticate DB_COOPER")
	}
	if sshFreshPassword != "DB_COOPER" {
		t.Fatalf("UI fresh-flash default password = %q, want DB_COOPER", sshFreshPassword)
	}
	// V18: 登录路径烤进镜像（open-xiaoai 02-login.patch 同款）——
	// common-auth 构建时切 pam_unix，shadow root hash 烘焙 DB_COOPER，
	// sshd 保持 stock `auth include common-auth`（不再运行时 bind 覆盖）。
	if !strings.Contains(string(buildScript), "lx06-pam-baked") {
		t.Fatal("V18: build_ssh_base.sh no longer bakes common-auth onto pam_unix")
	}
	if !strings.Contains(string(buildScript), "pam_unix.so nullok_secure") {
		t.Fatal("V18: baked common-auth does not enable the stock pam_unix line")
	}
}

// TestOfficialPackageClearsStaleSSHEnableMarkers proves the stock plan disables
// any SSH marker left in persistent /data before the START=50 dropbear service
// reads it, while never installing the custom SSH bring-up hook.
func TestOfficialPackageClearsStaleSSHEnableMarkers(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("firmware image inspection uses WSL unsquashfs")
	}
	if _, err := exec.LookPath("wsl.exe"); err != nil {
		t.Skip("wsl.exe not available")
	}
	image := mustAbs(t, filepath.Join("official", "root.squashfs"))
	if _, err := os.Stat(image); err != nil {
		t.Skipf("official image missing: %v", err)
	}
	boot, err := readImgFile(t, image, "/etc/init.d/boot")
	if err != nil {
		t.Fatalf("read official boot script: %v", err)
	}
	dropbear, err := readImgFile(t, image, "/etc/init.d/dropbear")
	if err != nil {
		t.Fatalf("read official dropbear script: %v", err)
	}
	bootStart := regexp.MustCompile(`(?m)^START=(\d+)\s*$`).FindStringSubmatch(boot)
	dropbearStart := regexp.MustCompile(`(?m)^START=(\d+)\s*$`).FindStringSubmatch(dropbear)
	if bootStart == nil || bootStart[1] != "10" || dropbearStart == nil || dropbearStart[1] != "50" {
		t.Fatalf("unexpected service order: boot START=%v, dropbear START=%v", bootStart, dropbearStart)
	}
	mountAt := strings.Index(boot, "mount -t ubifs /dev/ubi0_0 /data")
	clearAt := strings.Index(boot, "rm -f /data/ssh_en /data/.ssh_en /tmp/ssh_en")
	if mountAt < 0 || clearAt < mountAt {
		t.Fatalf("official boot does not clear stale SSH markers after mounting /data")
	}
	if !strings.Contains(dropbear, "cat /data/ssh_en") || !strings.Contains(dropbear, "cat /data/.ssh_en") || !strings.Contains(dropbear, "cat /tmp/ssh_en") {
		t.Fatal("test assumption changed: dropbear no longer checks all three SSH markers")
	}
	if strings.Contains(boot, "/usr/lib/lx06/ssh-apply.sh") || strings.Contains(boot, "/usr/lib/lx06/ssh.sh") {
		t.Fatal("official package unexpectedly calls the custom SSH bring-up hook")
	}
}

// TestDeviceSideSSHScript simulates the source ssh.sh under WSL dash with a mock
// rootfs (stock dropbear config and shadow plus a mocked bind mount), then
// asserts the device-side SSH configuration and password state.
func TestDeviceSideSSHScript(t *testing.T) {
	if _, err := exec.LookPath("wsl.exe"); err != nil {
		t.Skip("wsl.exe not available")
	}
	baseImg := mustAbs(t, filepath.Join("official-ssh", "root.squashfs"))
	if _, err := os.Stat(baseImg); err != nil {
		t.Skipf("official-ssh image missing: %s", err)
	}
	// Extract the image only for its baked default config; the behavior under
	// test is the current source hooks/ssh.sh (the image is rebuilt separately).
	exdir := t.TempDir()
	os.RemoveAll(exdir)
	// 镜像必须放第一个参数，
	// 且不能带 -no-progress（该版本解析成 <dir>，吞掉后面的 -d 选项）。
	exCmd := exec.Command("wsl.exe", "-e", "unsquashfs",
		"-d", imgWslPath(exdir), imgWslPath(baseImg))
	if out, err := exCmd.CombinedOutput(); err != nil {
		// rc=2 is expected: unsquashfs can't create char devices (e.g.
		// /dev/console) on the /mnt/c interop fs. The regular files we need
		// are still extracted; verify the script below rather than fatal.
		t.Logf("extract rc=%v (tolerated; checking for /usr/lib/lx06/ssh.sh)\n%s", err, out)
	}
	srcScript := filepath.Join("hooks", "ssh.sh")
	raw, err := os.ReadFile(srcScript)
	if err != nil {
		t.Fatalf("read shipped script: %s", err)
	}
	// Single-pass path remap (longest first). Covers every absolute path the
	// script touches; anything else stays as-is and is harmless in the mock.
	// /data/etc/pam.d and /etc/pam.d are matched WITHOUT a trailing slash so
	// "mkdir -p /data/etc/pam.d" remaps too; the "$SVC" suffix follows
	// naturally.
	pathRe := regexp.MustCompile(
		`/data/etc/config/dropbear|/data/etc/shadow|/data/etc/pam\.d|/data/etc/dropbear|/data/etc/config|/data/lx06-ssh\.conf|/data/lx06-ssh-seq|/data/lx06-ssh-reset|/data/ssh_en|/etc/config/dropbear|/etc/shadow|/etc/pam\.d|/lx06-ssh-default\.conf|/lx06-ssh\.conf|/lx06-ssh-seq|/data`)
	mockRel := map[string]string{
		"/data/etc/config/dropbear": "data/etc/config/dropbear",
		"/data/etc/shadow":          "data/etc/shadow",
		"/data/etc/pam.d":           "data/etc/pam.d",
		"/data/etc/dropbear":        "data/etc/dropbear",
		"/data/etc/config":          "data/etc/config",
		"/data/lx06-ssh.conf":       "data/lx06-ssh.conf",
		"/data/lx06-ssh-seq":        "data/lx06-ssh-seq",
		"/data/lx06-ssh-reset":      "data/lx06-ssh-reset",
		"/etc/config/dropbear":      "etc/config/dropbear",
		"/etc/shadow":               "etc/shadow",
		"/etc/pam.d":                "etc/pam.d",
		// default conf ships inside the image; seed it from the extracted tree.
		"/lx06-ssh-default.conf": "lx06-ssh-default.conf",
		"/lx06-ssh.conf":         "lx06-ssh.conf",
		"/lx06-ssh-seq":          "lx06-ssh-seq",
		"/data/ssh_en":           "data/ssh_en",
		"/data":                  "data",
	}
	stockShadow := "root:$1$N0Iz0LLs$kZ5zG/Y2AUWHNE5I3ElWm1:18128:0:99999:7:::\n"
	// The image's baked default conf (fallback when the user fills nothing).
	// Its hash contains '/' — exactly the case the old '/'-delimiter sed
	// silently no-oped on (2026-10-07 regression, fixed with '|' delimiter).
	defaultConf, err := os.ReadFile(filepath.Join(exdir, "lx06-ssh-default.conf"))
	if err != nil {
		t.Skipf("image has no baked default conf (pre-merge image?): %v", err)
	}
	// pull the baked default's root hash out of the conf (for the fallback
	// assertion). Matches  ssh_root_hash=<token>.
	hashRe := regexp.MustCompile(`ssh_root_hash=([^\s]+)`)
	testHash := md5CryptHash("test-only-placeholder", "tst")

	// runApply seeds a mock device root, runs the (path-remapped) script under
	// WSL dash, and asserts the results. userConf="" means NO /lx06-ssh.conf is
	// seeded → the script must fall back to the baked default conf (whose hash
	// contains '/', the exact case the old '/'-delimiter sed no-oped on).
	runApply := func(t *testing.T, tcName string, port int, lanOnly bool, userConf, initialDataConf, initialDataSeq, expectedHash string, resetSentinel bool) {
		t.Helper()
		t.Run(tcName, func(t *testing.T) {
			base := t.TempDir()
			writeFile := func(rel, content string, mode os.FileMode) {
				p := filepath.Join(base, filepath.FromSlash(rel))
				if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(p, []byte(content), mode); err != nil {
					t.Fatal(err)
				}
			}
			// data/ must pre-exist: /data is the persistent partition (boot()
			// tail runs it up already); the script writes /data/ssh_en BEFORE
			// it mkdirs /data/etc/config.
			writeFile("data/.keep", "", 0o644)
			// The baked sequence and persistent sequence match unless a scenario
			// deliberately exercises a new-image reset.
			writeFile("lx06-ssh-seq", "v13-test\n", 0o644)
			writeFile("lx06-ssh-default.conf", string(defaultConf), 0o644)
			dataSeq := initialDataSeq
			if dataSeq == "" {
				dataSeq = "v13-test\n"
			}
			writeFile("data/lx06-ssh-seq", dataSeq, 0o644)
			if initialDataConf != "" {
				writeFile("data/lx06-ssh.conf", initialDataConf, 0o644)
			}
			// R2: factory-reset sentinel (miio_helper config_clean_nosound
			// touches this on 恢复出厂; the miio hook passes SSH_FORCE_DEFAULT=1).
			if resetSentinel {
				writeFile("data/lx06-ssh-reset", "", 0o644)
			}
			// stock PAM dir must exist: the mock bind (cp) of
			// /data/etc/pam.d/$SVC onto /etc/pam.d/$SVC needs a target dir.
			writeFile("etc/pam.d/other", "auth	required	pam_unix.so\n", 0o644)
			// stock dropbear config: Port line space-aligned (worst case)
			writeFile("etc/config/dropbear",
				"config dropbear\n	option PasswordAuth '1'\n	option RootPasswordAuth '1'\n	option Port         '22'\n	option BannerFile   '/etc/banner'\n", 0o644)
			writeFile("etc/shadow", stockShadow, 0o644)
			// mock mount: a real bind would expose the staged file at the
			// target; emulate with a copy (script calls `mount --bind src dst`)
			writeFile("bin/mount",
				"#!/bin/sh\n"+
					"if [ \"$1\" = \"--bind\" ]; then cp \"$2\" \"$3\" 2>/dev/null || true; fi\n"+
					"echo \"mount $@\" >> \"$MOCK_BASE/.mock_calls\"\n"+
					"exit 0\n", 0o755)
			// the user's flash-time conf, if any (otherwise default fallback).
			if userConf != "" {
				writeFile("lx06-ssh.conf", userConf, 0o644)
			}
			// redirect the script's absolute paths into the mock base.
			// SINGLE-PASS regex, longest pattern first: the paths are nested
			// (/data/etc/config/dropbear contains /etc/config/dropbear), so
			// sequential ReplaceAll re-matches inside earlier replacements
			// and doubles the prefix.
			sim := pathRe.ReplaceAllStringFunc(string(raw), func(m string) string {
				return "$MOCK_BASE/" + mockRel[m]
			})
			simPath := filepath.Join(base, "bin", "ssh-apply-sim.sh")
			writeFile("bin/ssh-apply-sim.sh", sim, 0o755)
			// wsl.exe does NOT propagate the parent environment into the
			// Linux instance (verified 2026-09-26: cmd.Env vars never reach
			// `wsl.exe -e env`). Export them from a wrapper script instead.
			// SSH_FORCE_DEFAULT=1 mirrors the miio_register_success hook
			// (配网完成 + reset sentinel → 强制回退默认).
			forceDefault := "0"
			if resetSentinel {
				forceDefault = "1"
			}
			wrap := filepath.Join(base, "bin", "run-sim.sh")
			writeFile("bin/run-sim.sh",
				"#!/bin/sh\n"+
					// The remapped script uses paths relative to the mock base
					// (absolute paths were rewritten to base-relative), so cd
					// INTO the base before running — otherwise the relative
					// writes land in the test process's CWD (project dir).
					"cd "+imgWslPath(base)+"\n"+
					"export MOCK_BASE="+imgWslPath(base)+"\n"+
					"export SSH_SH_TEST=1\n"+
					"export SSH_FORCE_DEFAULT="+forceDefault+"\n"+
					"export PATH="+imgWslPath(filepath.Join(base, "bin"))+":$PATH\n"+
					"exec dash "+imgWslPath(simPath)+"\n", 0o755)
			cmd := exec.Command("wsl.exe", "-e", "sh", imgWslPath(wrap))
			// cmd.Dir must be a Windows path (Go chdirs on the Windows side);
			// the wrapper re-cds to the /mnt/c form inside WSL.
			cmd.Dir = base
			if out, err := cmd.CombinedOutput(); err != nil {
				t.Fatalf("ssh.sh (%s) failed: %v\n%s", tcName, err, out)
			}
			// ---- assertions ----
			db, err := os.ReadFile(filepath.Join(base, "etc", "config", "dropbear"))
			if err != nil {
				t.Fatal(err)
			}
			text := string(db)
			wantPort := fmt.Sprintf("option Port		%d", port)
			if !strings.Contains(text, wantPort+"\n") {
				t.Errorf("%s: UCI Port not applied.\nwant line: %q\ngot:\n%s", tcName, wantPort, text)
			}
			if strings.Count(text, "option Port") != 1 {
				t.Errorf("%s: expected exactly one Port line, got %d\n%s", tcName, strings.Count(text, "option Port"), text)
			}
			if strings.Contains(text, "option Port         '22'") {
				t.Errorf("%s: stock Port line survived delete-then-append\n%s", tcName, text)
			}
			// LAN-only is enforced by ssh-watch.sh (iptables ssh_lan chain), NOT
			// by a UCI Interface: UCI 'lan' is eth0 and never gets an IP on
			// LX06, so Interface 'lan' made dropbear_instance() fail. The
			// dropbear config must therefore carry no Interface line at all.
			if strings.Contains(text, "option Interface") {
				t.Errorf("%s: Interface line present (must stay absent — UCI lan=eth0 has no IP)\n%s", tcName, text)
			}
			// PAM (V18): zero runtime PAM changes. common-auth is baked at
			// build time (build_ssh_base.sh switches it to pam_unix, same as
			// open-xiaoai 02-login.patch); ssh.sh must NOT stage or bind any
			// /data/etc/pam.d/$SVC service files anymore (V17.2's 4-line
			// overrides caused AUTHINFO_UNAVAIL on-device).
			if _, err := os.Stat(filepath.Join(base, "data", "etc", "pam.d")); !os.IsNotExist(err) {
				t.Errorf("%s: V18 must not create /data/etc/pam.d at runtime", tcName)
			}
			// Expected effective root hash: explicit scenario value, or the
			// image's fallback when the scenario does not specify one.
			wantHash := expectedHash
			if wantHash == "" {
				if m := hashRe.FindStringSubmatch(string(defaultConf)); m != nil {
					wantHash = m[1]
				} else {
					t.Fatalf("could not extract hash from baked default conf")
				}
			}
			// /data persistent copy must contain the effective settings.
			dataConf, err := os.ReadFile(filepath.Join(base, "data", "lx06-ssh.conf"))
			if err != nil {
				t.Errorf("%s: /data/lx06-ssh.conf missing: %v", tcName, err)
			} else {
				m := hashRe.FindStringSubmatch(string(dataConf))
				if m == nil || m[1] != wantHash {
					t.Errorf("%s: /data conf has wrong root hash (want %q)\n%s", tcName, wantHash, dataConf)
				}
				if !strings.Contains(string(dataConf), fmt.Sprintf("ssh_port=%d\n", port)) {
					t.Errorf("%s: /data conf has wrong port\n%s", tcName, dataConf)
				}
				wantLAN := "0"
				if lanOnly {
					wantLAN = "1"
				}
				if !strings.Contains(string(dataConf), "ssh_lan_only="+wantLAN+"\n") {
					t.Errorf("%s: /data conf has wrong LAN-only value (want %s)\n%s", tcName, wantLAN, dataConf)
				}
			}
			persistedSeq, seqErr := os.ReadFile(filepath.Join(base, "data", "lx06-ssh-seq"))
			if seqErr != nil || strings.TrimSpace(string(persistedSeq)) != "v13-test" {
				t.Errorf("%s: persistent sequence = %q (want v13-test)", tcName, persistedSeq)
			}
			// R2: when the factory-reset sentinel was present, the script must
			// CONSUME it (rm -f) so the next provisioning doesn't re-trigger.
			if resetSentinel {
				if _, err := os.Stat(filepath.Join(base, "data", "lx06-ssh-reset")); !os.IsNotExist(err) {
					t.Errorf("%s: factory-reset sentinel not consumed (should be rm -f'd)", tcName)
				}
			}
			sshEn, err := os.ReadFile(filepath.Join(base, "data", "ssh_en"))
			if err != nil || strings.TrimSpace(string(sshEn)) != "1" {
				t.Errorf("%s: /data/ssh_en = %q (want 1)", tcName, sshEn)
			}
			shadow, err := os.ReadFile(filepath.Join(base, "etc", "shadow"))
			if err != nil {
				t.Fatal(err)
			}
			if !strings.Contains(string(shadow), "root:"+wantHash+":") {
				t.Errorf("%s: root shadow hash not replaced (want %q)\n%s", tcName, wantHash, shadow)
			}
			if strings.Contains(string(shadow), "$1$N0Iz0LLs$") {
				t.Errorf("%s: stock shadow hash survived (want replaced)\n%s", tcName, shadow)
			}
			calls, _ := os.ReadFile(filepath.Join(base, ".mock_calls"))
			cs := string(calls)
			for _, want := range []string{
				"mount --bind " + imgWslPath(filepath.Join(base, "data", "etc", "config", "dropbear")) + " " + imgWslPath(filepath.Join(base, "etc", "config", "dropbear")),
				"mount --bind " + imgWslPath(filepath.Join(base, "data", "etc", "shadow")) + " " + imgWslPath(filepath.Join(base, "etc", "shadow")),
			} {
				if !strings.Contains(cs, want) {
					t.Errorf("%s: expected bind mount %q\ncalls:\n%s", tcName, want, cs)
				}
			}
		})
	}

	// user-filled conf (each port + LAN-only combo): user values must win.
	for _, tc := range []struct {
		port    int
		lanOnly bool
	}{
		{22, true}, {222, true}, {2222, true}, {22222, true},
		{22, false}, {65535, true},
	} {
		runApply(t, fmt.Sprintf("user_port%05d_lan%v", tc.port, tc.lanOnly),
			tc.port, tc.lanOnly,
			fmt.Sprintf("# test\nssh_port=%d\nssh_root_hash=%s\nssh_lan_only=%s\n",
				tc.port, testHash, func() string {
					if tc.lanOnly {
						return "1"
					}
					return "0"
				}()), "", "", testHash, false)
	}
	// no user conf → must fall back to the baked default (port 89, the '/'
	// hash, LAN-only). Guards the '|' sed delimiter fix.
	runApply(t, "default_fallback", 89, true, "", "", "", "", false)
	runApply(t, "invalid_persistent_conf_reseeded", 89, true, "",
		"# corrupt legacy data\nssh_port=2222\nssh_root_hash=not-a-crypt-hash\nssh_lan_only=0\n", "", "", false)
	persistentHash := md5CryptHash("persistent-test-password", "pdat")
	persistentConf := fmt.Sprintf("ssh_port=2345\nssh_root_hash=%s\nssh_lan_only=0\n", persistentHash)
	runApply(t, "valid_persistent_conf_preserved", 2345, false, "", persistentConf, "", persistentHash, false)
	runApply(t, "sequence_change_reseeds_persistent_conf", 89, true, "", persistentConf, "v12-test\n", "", false)
	runApply(t, "invalid_flash_conf_ignored", 89, true,
		"ssh_port=70000\nssh_root_hash=bad\nssh_lan_only=0\n", "", "", "", false)

	// R2 出厂回退：factory reset（米家恢复出厂）只选择性清 /data，不清
	// /data/lx06-ssh.conf。reset sentinel + SSH_FORCE_DEFAULT=1 命中后，
	// 无论刷机时自定义端口是什么（2222/V16testA1/非LAN-only），配网完成都
	// 强制回退到镜像默认 89/DB_COOPER/仅局域网，并消费 sentinel。
	r2DefaultHash := ""
	if m := hashRe.FindStringSubmatch(string(defaultConf)); m != nil {
		r2DefaultHash = m[1]
	}
	// a) 刷机时自定义 2222 + 非 LAN-only → 出厂后回退 89/LAN-only
	r2Custom := fmt.Sprintf("ssh_port=2222\nssh_root_hash=%s\nssh_lan_only=0\n", testHash)
	runApply(t, "r2_reset_falls_back_to_default", 89, true,
		r2Custom, r2Custom, "", r2DefaultHash, true)
	// b) 出厂后 /data 配置已不存在（部分场景）→ sentinel 仍强制默认
	runApply(t, "r2_reset_fallback_no_persistent_conf", 89, true,
		"", "", "", r2DefaultHash, true)
}

// readImgFile extracts one file from a squashfs image via WSL unsquashfs.
func readImgFile(t *testing.T, img, path string) (string, error) {
	t.Helper()
	dst := t.TempDir()
	os.RemoveAll(dst) // unsquashfs nests squashfs-root/ into pre-existing dirs
	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		// 同 TestDeviceSideSSHApplyScript：-d 选项必须在 FILESYSTEM 之前。
		args := []string{"-e", "unsquashfs", "-d", imgWslPath(dst), imgWslPath(img)}
		cmd = exec.Command("wsl.exe", args...)
	} else {
		args := []string{"-d", dst, img}
		cmd = exec.Command("unsquashfs", args...)
	}
	if out, err := cmd.CombinedOutput(); err != nil {
		// rc=2 tolerated: char devices can't be created on /mnt/c interop fs;
		// regular files are still extracted (verified by the probe below).
		t.Logf("unsquashfs rc=%v (tolerated)\n%s", err, out)
	}
	// unsquashfs 4.x: -d absent dir → direct expansion; -d existing dir →
	// nested squashfs-root/. We RemoveAll(dst) above, but be explicit either
	// way.
	probe := func(p string) bool {
		_, e := os.Stat(filepath.Join(p, filepath.FromSlash(strings.TrimPrefix(path, "/"))))
		return e == nil
	}
	if !probe(dst) {
		t.Logf("not under %s directly; trying squashfs-root/ and nested layouts", dst)
		if probe(filepath.Join(dst, "squashfs-root")) {
			dst = filepath.Join(dst, "squashfs-root")
		} else {
			entries, _ := os.ReadDir(dst)
			for _, e := range entries {
				t.Logf("dst entry: %s", e.Name())
			}
		}
	}
	b, err := os.ReadFile(filepath.Join(dst, filepath.FromSlash(strings.TrimPrefix(path, "/"))))
	if err != nil {
		// Windows interop fs 不能直接 open unsquashfs 解出的符号链接
		//（"file cannot be accessed"）→ 解析到目标再读。
		full := filepath.Join(dst, filepath.FromSlash(strings.TrimPrefix(path, "/")))
		if resolved, e2 := filepath.EvalSymlinks(full); e2 == nil {
			b, err = os.ReadFile(resolved)
		} else if target, e3 := os.Readlink(full); e3 == nil {
			// EvalSymlinks 失败但能读到链接目标 → 按相对/绝对路径手动解析
			if filepath.IsAbs(target) {
				b, err = os.ReadFile(target)
			} else {
				b, err = os.ReadFile(filepath.Join(filepath.Dir(full), target))
			}
		}
	}
	return string(b), err
}

// extractImgRoot 把整镜像解包一次并返回根目录（t 结束时自动清理）。
// 同一镜像的多次文件/目录读取共享这一次解包，避免每次读文件都全量 unsquashfs。
func extractImgRoot(t *testing.T, img string) string {
	t.Helper()
	dst := t.TempDir()
	os.RemoveAll(dst) // unsquashfs nests squashfs-root/ into pre-existing dirs
	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		cmd = exec.Command("wsl.exe", "-e", "unsquashfs", "-d", imgWslPath(dst), imgWslPath(img))
	} else {
		cmd = exec.Command("unsquashfs", "-d", dst, img)
	}
	if out, err := cmd.CombinedOutput(); err != nil {
		// rc=2 tolerated: char devices can't be created on interop fs.
		t.Logf("unsquashfs rc=%v (tolerated)\n%s", err, out)
	}
	probe := func(p string) bool {
		_, e := os.Stat(filepath.Join(p, "etc"))
		return e == nil
	}
	if !probe(dst) {
		if probe(filepath.Join(dst, "squashfs-root")) {
			dst = filepath.Join(dst, "squashfs-root")
		} else {
			t.Fatalf("extractImgRoot: 解包根目录不在 %s 下", dst)
		}
	}
	return dst
}

// imgReadTree 从已解包树读文件（树内符号链接解析，与 readImgFile 同语义）。
func imgReadTree(t *testing.T, root, path string) (string, error) {
	t.Helper()
	full := filepath.Join(root, filepath.FromSlash(strings.TrimPrefix(path, "/")))
	b, err := os.ReadFile(full)
	if err != nil {
		if resolved, e2 := filepath.EvalSymlinks(full); e2 == nil {
			b, err = os.ReadFile(resolved)
		} else if target, e3 := os.Readlink(full); e3 == nil {
			if filepath.IsAbs(target) {
				b, err = os.ReadFile(target)
			} else {
				b, err = os.ReadFile(filepath.Join(filepath.Dir(full), target))
			}
		}
	}
	return string(b), err
}

// imgListDir 列出已解包树中某目录的文件名（排序）。
func imgListDir(t *testing.T, root, dir string) ([]string, error) {
	t.Helper()
	entries, err := os.ReadDir(filepath.Join(root, filepath.FromSlash(strings.TrimPrefix(dir, "/"))))
	if err != nil {
		return nil, err
	}
	var names []string
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	return names, nil
}

// imgWslPath converts a Windows path to its /mnt/c form for WSL commands.
func imgWslPath(p string) string {
	p = filepath.ToSlash(p)
	if runtime.GOOS == "windows" && strings.HasPrefix(p, "C:/") {
		p = "/mnt/c/" + strings.TrimPrefix(p, "C:/")
	}
	return p
}

// mustAbs resolves p against the test working directory.
func mustAbs(t *testing.T, p string) string {
	t.Helper()
	abs, err := filepath.Abs(p)
	if err != nil {
		t.Fatal(err)
	}
	return abs
}

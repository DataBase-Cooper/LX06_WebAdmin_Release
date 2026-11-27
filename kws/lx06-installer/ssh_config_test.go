package main

import (
	"strings"
	"testing"
)

// TestValidateSSHConfigBakePathZeroConfig pins the V19 zero-config contract:
// the bake path (requireIP=false) ignores EVERY user input and always
// returns the fixed config (port 89 / DB_COOPER / LAN-only). No matter what
// the user typed (or old saved values prefill), the flashed image is always
// 89/DB_COOPER/LAN-only; the user changes the password later via the HTTP
// console (port 88), which takes effect immediately.
func TestValidateSSHConfigBakePathZeroConfig(t *testing.T) {
	// a pile of varied / extreme user inputs — all must collapse to fixed
	for _, c := range []SSHConfig{
		{},
		{Port: 22, Password: "DB_COOPER", LANOnly: true},
		{Port: 222, Password: "whatever", LANOnly: false},
		{Port: 65535, Password: "x", LANOnly: true},
		{Port: 8899, Password: "x"},
		{Port: 0, Password: ""},
		{IP: "192.0.2.10", Port: 2345, Password: "custom-pass", LANOnly: false},
	} {
		got, err := validateSSHConfig(c, false)
		if err != nil {
			t.Errorf("bake path: unexpected error %v for %+v", err, c)
			continue
		}
		if got.Port != 89 {
			t.Errorf("bake path: port = %d, want fixed 89 (input %+v)", got.Port, c)
		}
		if got.Password != "DB_COOPER" {
			t.Errorf("bake path: password = %q, want fixed DB_COOPER (input %+v)", got.Password, c)
		}
		if !got.LANOnly {
			t.Errorf("bake path: LANOnly = false, want fixed true (input %+v)", c)
		}
		if got.IP != "" {
			t.Errorf("bake path: IP = %q, want empty (device has none before flash)", got.IP)
		}
	}
}

// TestValidateSSHConfigManualPathRequiresIP pins the manual "apply to an
// already-flashed device" path: a valid IPv4 is required, and the same
// 1-65535 / 8899 rules apply.
func TestValidateSSHConfigManualPathRequiresIP(t *testing.T) {
	if _, err := validateSSHConfig(SSHConfig{Port: 22, Password: "x", IP: "192.0.2.10"}, true); err != nil {
		t.Errorf("manual path valid IP: unexpected error %v", err)
	}
	if _, err := validateSSHConfig(SSHConfig{Port: 22, Password: "x", IP: ""}, true); err == nil {
		t.Error("manual path empty IP: expected error, got none")
	}
	if _, err := validateSSHConfig(SSHConfig{Port: 8899, Password: "x", IP: "192.0.2.10"}, true); err == nil ||
		!strings.Contains(err.Error(), "8899") {
		t.Errorf("manual path occupied port: expected 8899 conflict, got %v", err)
	}
}

// TestFixedSSHConfig pins the canonical zero-config values that every other
// zero-config assertion (bake path, H5 form, image default conf) depends on.
func TestFixedSSHConfig(t *testing.T) {
	c := fixedSSHConfig()
	if c.Port != 89 || c.Password != "DB_COOPER" || !c.LANOnly {
		t.Errorf("fixedSSHConfig = %+v, want {Port:89 Password:DB_COOPER LANOnly:true}", c)
	}
}

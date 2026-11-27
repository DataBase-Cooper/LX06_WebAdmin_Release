package main

import (
	"os"
	"path/filepath"
	"testing"
)

// DumpArtifacts writes the exact provisioner output for a fixed config so
// the device can be tested with byte-identical files. Run:
//   go test -run DumpArtifacts -v (env LX06_DUMP_DIR)
func TestDumpArtifacts(t *testing.T) {
	dir := os.Getenv("LX06_DUMP_DIR")
	if dir == "" {
		t.Skip("LX06_DUMP_DIR not set")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	// Keep the stock password for the device test (open-xiaoai).
	shadow := buildShadow("$1$open-xia$nLuyH2rHYhdlW4Oy4DiqV0")
	probe := buildProbeScript(2233, true)
	conf := buildDropbearConf(2233)
	files := map[string]string{
		"shadow":        shadow,
		"dropbear.conf": conf,
		"probe.sh":      probe,
	}
	for name, data := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(data), 0o644); err != nil {
			t.Fatal(err)
		}
		t.Logf("wrote %s (%d bytes)", name, len(data))
	}
}

package main

import (
	"strings"
	"testing"
)

func TestWipeDataOutcomeIndependentFromCacheAndRPMB(t *testing.T) {
	for _, tc := range []struct {
		name                          string
		mmc, nand, cache, mark, saved bool
		want                          string
	}{
		{"reported-rc19-log", false, true, false, true, true, "succeeded"},
		{"all-data-commands-failed", false, false, true, true, true, "unconfirmed"},
		{"mmc-success-no-nand-retry", true, false, true, true, true, "succeeded"},
		{"firstboot-save-failed", true, false, true, true, false, "succeeded"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var calls []string
			report := performDataWipe(func(command, _ string) bool {
				calls = append(calls, command)
				switch command {
				case "setenv firstboot 1":
					return tc.mark
				case "saveenv":
					return tc.saved
				case "amlmmc erase data":
					return tc.mmc
				case "nand erase.part data":
					if tc.mmc {
						t.Fatal("issued redundant destructive command after successful erase")
					}
					return tc.nand
				case "amlmmc erase cache":
					return tc.cache
				}
				return false // RPMB unsupported; optional cache fallback also fails
			})
			state, notice := report.UIState()
			if state != tc.want || report.DataErased != (tc.mmc || tc.nand) {
				t.Fatalf("report=%+v state=%s calls=%v", report, state, calls)
			}
			if !tc.cache && !strings.Contains(notice, "cache 附加清理未确认") {
				t.Fatal("lost optional cache diagnostic")
			}
			if !tc.saved && !strings.Contains(notice, "首次开机标记未确认保存") {
				t.Fatal("lost saveenv failure")
			}
			a := &App{}
			a.beginFlash("official-ssh", SSHConfig{}, nil, true)
			a.setFlash(func(f *flashState) { f.wipeResult, f.wipeNotice = report.UIState() })
			api := a.flashSnapshot()
			if api["wipeResult"] != state || api["wipeNotice"] != notice {
				t.Fatal("UI API lost separate DATA/optional results", api)
			}
		})
	}
}

func TestWipeRequiresPositiveDeviceAcknowledgement(t *testing.T) {
	for _, tc := range []struct {
		lines []string
		want  bool
	}{
		{[]string{"Read status return success"}, true},
		{[]string{"Read status return failed:", "strncmp != 0", "ERR: AmlUsbBulkCmd failed!"}, false},
		{[]string{"Read status return failed:"}, false},
		{nil, false},
		{[]string{"AmlUsbBulkCmd = nand erase.part data"}, false},
		{[]string{"Read status return success", "ERR: device rejected command"}, false},
	} {
		var status wipeCommandStatus
		for _, line := range tc.lines {
			status.Consume(line)
		}
		if got := status.Confirmed && !status.Failed; got != tc.want {
			t.Fatalf("%q: %+v", tc.lines, status)
		}
	}
}

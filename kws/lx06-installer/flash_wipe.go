package main

import "strings"

// The USB utility can exit zero even when U-Boot rejected a command. A process
// exit code alone does not confirm an erase; require the device status ACK.
type wipeCommandStatus struct {
	Confirmed bool
	Failed    bool
}

func (s *wipeCommandStatus) Consume(line string) {
	text := strings.ToLower(line)
	if strings.Contains(text, "read status return success") {
		s.Confirmed = true
	}
	if strings.Contains(text, "read status return failed") ||
		strings.Contains(text, "strncmp != 0") ||
		(strings.Contains(text, "err") && !strings.Contains(text, "no error")) {
		s.Failed = true
	}
}

// DATA erase is the user's requested operation. firstboot/saveenv, cache and
// RPMB are separate outcomes, never a single misleading all-partitions bool.
type dataWipeReport struct {
	DataErased        bool
	CacheErased       bool
	FirstBootPrepared bool
	RPMBReset         bool
}

func (r dataWipeReport) Notice() string {
	var notes []string
	if !r.CacheErased {
		notes = append(notes, "cache 附加清理未确认，不代表 DATA 擦除失败")
	}
	if !r.FirstBootPrepared {
		notes = append(notes, "首次开机标记未确认保存，请查看日志")
	}
	return strings.Join(notes, "；")
}

func (r dataWipeReport) UIState() (result, notice string) {
	result = "unconfirmed"
	if r.DataErased {
		result = "succeeded"
	}
	return result, r.Notice()
}

// Inject only the command runner so tests never execute real USB/erase calls.
func performDataWipe(run func(command, label string) bool) dataWipeReport {
	marked := run("setenv firstboot 1", "标记首次开机 (firstboot=1)")
	saved := run("saveenv", "保存 firstboot (saveenv)")
	r := dataWipeReport{FirstBootPrepared: marked && saved}
	r.RPMBReset = run("rpmb_reset", "附加步骤：重置 eMMC RPMB (rpmb_reset)")
	erase := func(part string) bool {
		if run("amlmmc erase "+part, "擦除 "+part+" 分区 (eMMC: amlmmc erase)") {
			return true
		}
		return run("nand erase.part "+part, "擦除 "+part+" 分区 (NAND: nand erase.part)")
	}
	r.DataErased = erase("data")
	r.CacheErased = erase("cache")
	return r
}

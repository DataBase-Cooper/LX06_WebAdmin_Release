package main

import (
	"bufio"
	"reflect"
	"strings"
	"testing"
)

func TestFlashProgressStreamingAndCompletion(t *testing.T) {
	a := &App{}
	if !a.beginFlash("official-ssh", SSHConfig{}, nil, true) {
		t.Fatal("start")
	}
	a.setFlash(func(f *flashState) { f.phase = phaseWrite; f.progress = 20 })
	sc := bufio.NewScanner(strings.NewReader("0%\r25%\r\n50%\r49%\r100%\nTransfer Complete!"))
	sc.Split(splitUpdateOutput)
	var values []int
	for sc.Scan() {
		if !strings.Contains(sc.Text(), "%") {
			continue
		}
		a.onProgressLine(sc.Text())
		values = append(values, a.flash.progress)
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(values, []int{20, 37, 55, 55, 90}) {
		t.Fatal(values)
	}
	a.onProgressLine("99999999999999999999999%")
	if a.flash.progress != 90 {
		t.Fatal("invalid percentage changed progress")
	}
	a.setFlash(func(f *flashState) { f.phase = phaseReboot; f.progress = 91 })
	a.setFlash(func(f *flashState) { f.progress = 100 })
	if a.flash.progress != 99 {
		t.Fatal("premature completion")
	}
	a.finishFlash(phaseDone, "done")
	if a.flash.progress != 100 || a.flash.active {
		t.Fatal("final completion")
	}
}

func TestFlashCRProgressDoesNotWaitForEOF(t *testing.T) {
	advance, token, err := splitUpdateOutput([]byte("35%\r"), false)
	if err != nil || advance != 4 || string(token) != "35%" {
		t.Fatalf("%d %q %v", advance, token, err)
	}
}

func TestWindowCloseGuardsActiveWork(t *testing.T) {
	a := &App{}
	a.beginFlash("official", SSHConfig{}, nil, false)
	if a.requestWindowClose() || a.closing {
		t.Fatal("active flash allowed close")
	}
	a.finishFlash(phaseDone, "done")
	a.drvBusy = true
	if a.requestWindowClose() {
		t.Fatal("active driver install allowed close")
	}
	a.drvBusy = false
	if !a.requestWindowClose() {
		t.Fatal("completed work blocked close")
	}
	if a.beginFlash("official", SSHConfig{}, nil, false) || a.beginSSHApply(SSHConfig{}) {
		t.Fatal("started work after close accepted")
	}
}

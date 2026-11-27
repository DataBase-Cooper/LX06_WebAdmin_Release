#!/usr/bin/env python3
"""wakeword_template v2 单元自测（合成音源，验证 DSP/IO/阈值链路）"""
import math, os, struct, subprocess, sys, wave

B = sys.argv[1] if len(sys.argv) > 1 else "build/wakeword_template_x86"
T = "/tmp/wwtest"
os.makedirs(T, exist_ok=True)
SR = 16000
fails = []

def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (" | " + extra if extra else ""))
    if not cond: fails.append(name)

def stream(secs, freq, amp, t_off=0.0):
    n = int(secs * SR)
    out = bytearray()
    for i in range(n):
        if freq:
            v = int(amp * 32767 * math.sin(2 * math.pi * freq * (t_off * SR + i) / SR))
        else:
            v = 0
        out += struct.pack("<h", v)
    return bytes(out)

def wav_of(path, s16):
    w = wave.open(path, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    a = struct.pack("<%dh" % len(s16), *s16)
    w.writeframes(a); w.close()

def read_s16(p):
    d = open(p, "rb").read()
    return list(struct.unpack("<%dh" % (len(d)//2), d))

def run(args, inp=None, timeout=30):
    return subprocess.run([B] + args, input=inp, capture_output=True, timeout=timeout)

# --- 音源 ---
sil = stream(1.0, 0, 0)
word = stream(1.2, 440, 0.5)
other = stream(1.0, 990, 0.5)
full = sil + word + sil
open(f"{T}/full.s16", "wb").write(full)
open(f"{T}/other.s16", "wb").write(other)

# --- capture ---
r = run(["capture", f"{T}/cap.wav", "6"], inp=full)
check("capture rc", r.returncode == 0, r.stderr.decode().strip())
w = wave.open(f"{T}/cap.wav")
fr = w.getnframes()
check("capture wav valid", w.getframerate() == 16000 and w.getnchannels() == 1, f"frames={fr} ({fr/16000:.2f}s)")
check("capture trimmed", 0.8 < fr / 16000 < 2.0, f"duration={fr/16000:.2f}s (原始语音1.2s)")
check("capture no-utt rc3", run(["capture", f"{T}/x.wav", "2"], inp=sil).returncode == 3)

# --- train（3 条相同录音 + 1 条过短）---
_w = wave.open(f"{T}/cap.wav")
cap_pcm = _w.readframes(_w.getnframes())
_w.close()
cap_s16 = list(struct.unpack("<%dh" % (len(cap_pcm)//2), cap_pcm))
for i in range(3):
    wav_of(f"{T}/rec{i}.wav", cap_s16)
short = cap_s16[:2000]  # 0.125s
wav_of(f"{T}/short.wav", short)
r = run(["train", "testword", f"{T}/model.lwt", f"{T}/rec0.wav", f"{T}/rec1.wav", f"{T}/rec2.wav", f"{T}/short.wav"])
check("train rc", r.returncode == 0, r.stderr.decode().strip().replace("\n", " "))
check("train skipped short", "TOOSHORT" in r.stderr.decode(), "")

# --- score：同音应低分，异音应高分 ---
r = run(["score", f"{T}/rec0.wav", f"testword={T}/model.lwt"])
sc_same = float(r.stdout.decode().split()[2])
wav_of(f"{T}/other.wav", read_s16(f"{T}/other.s16"))
r = run(["score", f"{T}/other.wav", f"testword={T}/model.lwt"])
sc_other = float(r.stdout.decode().split()[2])
check("score same<0.30", sc_same < 0.30, f"same={sc_same:.4f}")
check("score other>same*3", sc_other > sc_same * 3, f"other={sc_other:.4f}")

# --- detect：逐词阈值 + 全局阈值 ---
r = run(["detect", "testword=" + f"{T}/model.lwt:0.9"], inp=full)
check("detect WAKE with per-model th", "WAKE testword" in r.stdout.decode(), r.stdout.decode().strip()[:80])
# 异音 0.52 分 + 全局阈值 0.01 → 有 SCORE 但无 WAKE（阈值门控）
full_other = stream(0.5, 0, 0) + other + stream(1.5, 0, 0)
r = run(["detect", "--threshold", "0.01", "testword=" + f"{T}/model.lwt"], inp=full_other)
check("detect no WAKE above-threshold", "SCORE testword" in r.stderr.decode() and "WAKE" not in r.stdout.decode(),
      r.stderr.decode().strip()[-80:])
# 双词歧义抑制：同模型挂两个标签，分差 0 → 应 AMBIGUOUS 不唤醒
r = run(["detect", "w1=" + f"{T}/model.lwt:0.9", "w2=" + f"{T}/model.lwt:0.9"], inp=full)
check("detect ambiguity suppressed", "WAKE" not in r.stdout.decode(), r.stderr.decode().strip()[-120:])

# --- train 单条录音（边界：最少可训 1 条，webd 侧再限 ≥3）---
r = run(["train", "one", f"{T}/one.lwt", f"{T}/rec0.wav"])
check("train single rc", r.returncode == 0, r.stderr.decode().strip().replace("\n", " "))
r = run(["score", f"{T}/rec0.wav", f"one={T}/one.lwt"])
check("single-template score", r.returncode == 0, r.stdout.decode().strip())
check("saved model accepted by check", run(["check",f"{T}/one.lwt"]).returncode == 0)
# Natural four-character utterances may be under one second. Verify all three
# commands share the feature context and do not silently discard a short word.
with wave.open(f"{T}/brief.wav","wb") as w:
    w.setparams((1,2,SR,0,'NONE','not compressed')); w.writeframes(stream(0.9,440,0.5))
r=run(["train","brief",f"{T}/brief.lwt",f"{T}/brief.wav"])
check("0.9s word trains",r.returncode==0,r.stderr.decode().strip())
r=run(["score",f"{T}/brief.wav",f"brief={T}/brief.lwt"])
check("0.9s word scores",r.returncode==0,r.stdout.decode().strip())
r=run(["detect",f"brief={T}/brief.lwt:0.18"],inp=sil+stream(0.9,440,0.5)+stream(1.2,0,0))
check("0.9s word detected",b'WAKE brief' in r.stdout,r.stderr.decode().strip()[-120:])

# Short noise bursts spread over a long segment must not become an enrollment.
noise = sil + stream(.06,160,.03) + stream(.34,0,0) + stream(.02,140,.03) + sil
check("sparse noise rejected",run(["capture",f"{T}/noise.wav","4"],inp=noise).returncode==3)

print()
print("FAILURES:", fails if fails else "none — all pass")
sys.exit(1 if fails else 0)

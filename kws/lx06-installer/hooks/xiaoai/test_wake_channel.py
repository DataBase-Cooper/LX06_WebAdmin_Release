"""WT1 offline checks. The router half needs root because installed scripts must be uid 0."""
import ctypes
import os
import time
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
XIAO = ROOT / "hooks/xiaoai"
COMMON = ROOT / "runtime/common"


class WakeChannelTest(unittest.TestCase):
    def test_selftest_matcher_window_bind_and_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / "self"
            subprocess.run(
                ["gcc", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-I", str(XIAO), "-I", str(ROOT / "hooks/robust"),
                 str(XIAO / "wake-channel-selftest.c"), "-o", str(exe)],
                check=True)
            subprocess.run([str(exe)], check=True)

    def test_window_profile_reader_and_quiet_tail(self):
        headers = subprocess.run(
            ["gcc", "-E", "-x", "c", "-"], input="#include <opus/opus.h>\n",
            text=True, capture_output=True)
        if headers.returncode:
            self.skipTest("libopus-dev headers unavailable")
        import sys
        sys.path.insert(0, str(COMMON / "tests"))
        from fake_asr import SpeakerASR
        with tempfile.TemporaryDirectory() as folder:
            library = Path(folder) / "asr.so"
            subprocess.run(
                ["gcc", "-shared", "-fPIC", "-O2",
                 str(COMMON / "control_asr.c"),
                 str(ROOT / "runtime/doubao/src/json.c"),
                 "-lopus", "-o", str(library)],
                check=True)
            lib = ctypes.CDLL(str(library))
            lib.lx06_control_asr_open_profile.argtypes = [ctypes.c_char_p, ctypes.c_int]
            lib.lx06_control_asr_open_profile.restype = ctypes.c_void_p
            lib.lx06_control_asr_feed.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64]
            lib.lx06_control_asr_poll.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64]
            lib.lx06_control_asr_close.argtypes = [ctypes.c_void_p]
            lib.lx06_control_asr_window_readers.restype = ctypes.c_int
            lib.lx06_control_asr_window_timing.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
            lib.lx06_control_asr_window_ms.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            lib.lx06_control_asr_speech_seen.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)]
            self.assertFalse(lib.lx06_control_asr_open_profile(b"/unused", 99))
            self.assertEqual(lib.lx06_control_asr_window_readers(), 0)
            with SpeakerASR() as speaker:
                legacy = lib.lx06_control_asr_open_profile(speaker.path.encode(), 0)
                self.assertTrue(legacy)
                self.assertEqual(lib.lx06_control_asr_window_readers(), 0)
                buf = ctypes.create_string_buffer(32)
                quiet0 = ctypes.create_string_buffer(b"\0\0" * 320)
                self.assertEqual(lib.lx06_control_asr_feed(legacy, 1, quiet0, 640, 1000), 0)
                for _ in range(50):
                    lib.lx06_control_asr_poll(legacy, buf, 32, 1000)
                    if speaker.dialog:
                        break
                    time.sleep(0.01)
                lib.lx06_control_asr_close(legacy)
            with SpeakerASR() as speaker:
                state = lib.lx06_control_asr_open_profile(speaker.path.encode(), 2)
                self.assertTrue(state)
                self.assertEqual(lib.lx06_control_asr_window_readers(), 1)
                out = ctypes.create_string_buffer(128)
                quiet = ctypes.create_string_buffer(b"\0\0" * 320)
                loud = ctypes.create_string_buffer((800).to_bytes(2, "little", signed=True) * 320)
                self.assertEqual(lib.lx06_control_asr_feed(state, 1, quiet, 640, 0), 0)
                for _ in range(100):
                    lib.lx06_control_asr_poll(state, out, 128, 0)
                    if speaker.dialog:
                        break
                    time.sleep(0.01)
                self.assertTrue(speaker.dialog)
                for at in range(20, 400, 20):
                    self.assertEqual(lib.lx06_control_asr_feed(state, 1, quiet, 640, at), 0)
                    self.assertEqual(lib.lx06_control_asr_poll(state, out, 128, at), 0)
                self.assertEqual(speaker.end_packets, 0)
                for at in range(400, 520, 20):
                    self.assertEqual(lib.lx06_control_asr_feed(state, 1, loud, 640, at), 0)
                lib.lx06_control_asr_poll(state, out, 128, 520)
                speaker.emit("我要听晴天", False)
                for at in range(540, 1200, 20):
                    self.assertEqual(lib.lx06_control_asr_feed(state, 1, quiet, 640, at), 0)
                    lib.lx06_control_asr_poll(state, out, 128, at)
                self.assertGreaterEqual(speaker.end_packets, 1)
                self.assertEqual(speaker.start_packets[0][0], b"V")
                lib.lx06_control_asr_close(state)
                self.assertEqual(lib.lx06_control_asr_window_readers(), 0)

            # Virtual clocks test both deadlines without waiting real seconds.
            # None = defaults; legacy setters must leave start timeout alone.
            cases = [
                (None, None, None, 5000),
                (None, None, 2000, 12000),
                (None, None, 4900, 14900),
                (None, None, 0, 10000),
                ((500, 2000), None, None, 1000),
                ((500, 2000), None, 500, 3500),
                ((20000, 20000), None, 14000, 29000),
                ((2000, 6000), 2000, None, 2000),
                ((2000, 6000), 2000, 1000, 4000),
                ((2000, 6000), 0, 1000, 11000),
            ]
            for timing, legacy_ms, speech_at, deadline in cases:
                with self.subTest(timing=timing, legacy=legacy_ms, speech=speech_at):
                    with SpeakerASR() as speaker:
                        state = lib.lx06_control_asr_open_profile(speaker.path.encode(), 2)
                        self.assertTrue(state)
                        try:
                            if timing is not None:
                                lib.lx06_control_asr_window_timing(state, *timing)
                            if legacy_ms is not None:
                                lib.lx06_control_asr_window_ms(state, legacy_ms)
                            self.assertEqual(lib.lx06_control_asr_feed(state, 1, quiet, 640, 0), 0)
                            for _ in range(100):
                                lib.lx06_control_asr_poll(state, out, 128, 0)
                                if speaker.dialog:
                                    break
                                time.sleep(0.005)
                            self.assertTrue(speaker.dialog)
                            stamp = ctypes.c_uint64()
                            self.assertEqual(lib.lx06_control_asr_speech_seen(state, ctypes.byref(stamp)), 0)
                            if speech_at is not None:
                                self.assertEqual(lib.lx06_control_asr_feed(state, 1, loud, 640, speech_at), 0)
                                self.assertEqual(lib.lx06_control_asr_speech_seen(state, ctypes.byref(stamp)), 1)
                                self.assertEqual(stamp.value, speech_at)
                            for at in sorted({min(4999, deadline - 1), min(8000, deadline - 1), deadline - 1}):
                                if speech_at is not None and at < speech_at:
                                    continue
                                self.assertEqual(lib.lx06_control_asr_poll(state, out, 128, at), 0)
                            # Wait until prior audio is consumed before inspecting the socket fixture.
                            expected_audio = 1 + (speech_at is not None)
                            for _ in range(100):
                                if speaker.audio_packets >= expected_audio:
                                    break
                                time.sleep(0.005)
                            self.assertGreaterEqual(speaker.audio_packets, expected_audio)
                            self.assertEqual(speaker.end_packets, 0)
                            self.assertEqual(lib.lx06_control_asr_poll(state, out, 128, deadline), 0)
                            for _ in range(100):
                                if speaker.end_packets:
                                    break
                                time.sleep(0.005)
                            self.assertEqual(speaker.end_packets, 1)
                        finally:
                            lib.lx06_control_asr_close(state)
                        self.assertEqual(lib.lx06_control_asr_window_readers(), 0)

    def test_official_router_templates_keep_exact_words(self):
        if os.geteuid() != 0:
            self.skipTest("router registry requires root-owned scripts")
        with tempfile.TemporaryDirectory(prefix="lx06-router-tpl-") as folder:
            p = Path(folder)
            (p / "apps").mkdir()
            source = r'''
#define OR_TEST 1
#include "official-router.c"
#include <assert.h>
static void write_file(const char *path,const char *s){FILE *f=fopen(path,"w");assert(f);fputs(s,f);fclose(f);}
static void event(const char *id,const char *text){
 char s[2048];snprintf(s,sizeof(s),"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"RecognizeResult\",\"dialog_id\":\"%s\"},\"payload\":{\"is_final\":true,\"results\":[{\"text\":\"%s\"}]}}",id,text);
 instruction(s,strlen(s));
}
int main(void){
 char pid[96];snprintf(pid,sizeof(pid),"%u %llu",(unsigned)getpid(),(unsigned long long)lx06_process_ticks(getpid()));
 write_file(OR_GATE_PID,pid);
 mkdir(LX06_SCRIPT_POLICY_DIR,0755);
 write_file(LX06_SCRIPT_POLICY_DIR "/autostart","on\n");
 write_file(OR_APPS "/xz.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# 小爱通道唤醒词=召唤小智,我要听{query}\n");
 registry();
 assert(match("召唤小智"));
 assert(!match("我要听晴天"));
 event("exact","召唤小智");
 assert(dialog_count==1 && !strcmp(matched_name,"xz.sh"));
 event("tpl","我要听晴天");
 assert(dialog_count==2 && !strcmp(matched_name,"xz.sh"));
 event("neg","不要召唤小智");
 assert(dialog_count==2);
 write_file(OR_APPS "/other.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# 小爱通道唤醒词=播放{query}\n");
 write_file(OR_APPS "/xz.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# 小爱通道唤醒词=播放{query},召唤小智\n");
 registry();
 assert(match("召唤小智"));
 event("song","播放晴天");
 assert(!strcmp(prompt_kind,"reject"));
 assert(dialog_count==3);
 write_file(OR_APPS "/plain.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# 小爱通道唤醒词=召唤小智\n");
 unlink(OR_APPS "/xz.sh"); unlink(OR_APPS "/other.sh");
 registry();
 assert(match("召唤小智") && !template_count);
 event("plain","我要听周杰伦的晴天");
 assert(dialog_count==3);
 puts("router template layer passed");
 return 0;
}
'''
            (p / "test.c").write_text(source)
            flags = [f'-D{k}="{p / v}"' for k, v in {
                "OR_APPS": "apps", "OR_LOG": "instruction.log", "OR_NATIVE": "native",
                "LX06_SCRIPT_POLICY_DIR": "policy", "OR_GATE_PID": "gate.pid",
                "OR_CANCELED": "canceled", "LX06_SETTINGS_DIR": "settings",
                "LX06_OFFICIAL_BIND_PATH": "official-bind",
                "LX06_OFFICIAL_SWITCH_PATH": "official-switch",
            }.items()]
            subprocess.run(
                ["gcc", "-O1", "-g", "-I", str(XIAO), *flags, str(p / "test.c"),
                 str(ROOT / "runtime/doubao/src/json.c"), "-o", str(p / "test")],
                check=True)
            subprocess.run([str(p / "test")], check=True)

    def test_native_bind_routes_only_the_stock_arbitration_winner(self):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / "apps").mkdir()
            for name in ("doubao.sh", "xz.sh"):
                (p / "apps" / name).write_text("#!/bin/sh\n# LX06-OFFICIALWAKEUP: on\n")
            defines = [
                f'-DLX06_OFFICIAL_FILE="{p / "official"}"',
                f'-DLX06_NATIVE_REQUEST="{p / "request"}"',
                f'-DLX06_NATIVE_EVENT="{p / "event"}"',
                f'-DLX06_IDLE_EPOCH_FILE="{p / "epoch"}"',
                f'-DLX06_OFFICIAL_BIND_PATH="{p / "bind"}"',
                f'-DLX06_OFFICIAL_BIND_EVENT="{p / "bind-wake"}"',
                f'-DLX06_OFFICIAL_SWITCH_PATH="{p / "official"}"',
                f'-DLX06_BIND_APPS="{p / "apps"}"',
                f'-DLX06_SCRIPT_POLICY_DIR="{p / "policy"}"',
            ]
            subprocess.run(
                ["gcc", "-O2", "-Wall", "-Wextra", "-I", str(XIAO), *defines,
                 str(XIAO / "native-bind-selftest.c"), "-ldl", "-lpthread", "-o", str(p / "test")],
                check=True)
            subprocess.run([str(p / "test")], check=True)


if __name__ == "__main__":
    unittest.main()

"""Complete queued PCM through the real seqpacket and Opus transport.

The fake SDK controls FINAL latency; it does not replace the queue or codec.
Comparing every encoded packet with a fresh uninterrupted encoder detects
missing/reordered samples, including PCM frames split at ring-bank boundaries.
"""
import contextlib
import ctypes
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from fake_asr import SpeakerASR

ROOT=Path(__file__).resolve().parents[3]


class PendingPCMTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();directory=Path(cls.tmp.name)
        shim=directory/'allocation.c'
        # Only the optional 64,000-byte QUESTION bank is fault-injected. Opus
        # uses the actual shared library, and no production test hooks exist.
        shim.write_text('''#include <stddef.h>
#include <errno.h>
void *__real_malloc(size_t); void __real_free(void *);
static int fail,attempts,live; static void *bank;
void test_fail_growth(int enabled) { fail=enabled; attempts=0; }
int test_growth_attempts(void) { return attempts; }
int test_live_banks(void) { return live; }
void *__wrap_malloc(size_t n) {
 if(n==64000) {
  attempts++;
  if(fail) { errno=ENOMEM; return NULL; }
  void *p=__real_malloc(n); if(p) { bank=p; live++; } return p;
 }
 return __real_malloc(n);
}
void __wrap_free(void *p) {
 if(p && p==bank) { bank=NULL; live--; }
 __real_free(p);
}
''')
        library=directory/'asr.so'
        subprocess.run(['gcc','-shared','-fPIC','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        str(ROOT/'runtime/common/control_asr.c'),str(ROOT/'runtime/doubao/src/json.c'),
                        str(shim),'-Wl,--wrap=malloc,--wrap=free','-lopus','-o',str(library)],check=True)
        cls.lib=ctypes.CDLL(str(library))
        for name,args,result in [
            ('lx06_control_asr_open_profile',[ctypes.c_char_p,ctypes.c_int],ctypes.c_void_p),
            ('lx06_control_asr_feed',[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64],ctypes.c_int),
            ('lx06_control_asr_poll',[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64],ctypes.c_int),
            ('lx06_control_asr_close',[ctypes.c_void_p],None),
            ('test_fail_growth',[ctypes.c_int],None),('test_growth_attempts',[],ctypes.c_int),
            ('test_live_banks',[],ctypes.c_int)]:
            fn=getattr(cls.lib,name);fn.argtypes=args;fn.restype=result

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def setUp(self):
        self.assertEqual(self.lib.test_live_banks(),0)
        self.lib.test_fail_growth(0)

    def tearDown(self):self.assertEqual(self.lib.test_live_banks(),0)

    @staticmethod
    def await_condition(predicate):
        deadline=time.monotonic()+3
        while not predicate():
            if time.monotonic()>deadline:raise AssertionError('fixture packet not delivered')
            time.sleep(.002)

    @staticmethod
    def pcm(length):
        # A varying, non-silent sample sequence makes the oldest/newest and
        # ordering checks meaningful rather than comparing repeated silence.
        return b''.join(((i*73+(i//17)*911)%24001-12000).to_bytes(2,'little',signed=True)
                        for i in range(length//2))

    @contextlib.contextmanager
    def client(self,profile=1):
        with SpeakerASR() as speaker:
            state=self.lib.lx06_control_asr_open_profile(speaker.path.encode(),profile)
            self.assertTrue(state);out=ctypes.create_string_buffer(2048)
            try:
                self.assertEqual(self.feed(state,b'',1000),0)
                self.await_condition(lambda:bool(speaker.dialog));time.sleep(.005)
                self.assertEqual(self.poll(state,out,1050),0)
                yield state,speaker,out
            finally:
                speaker.stopping=True
                self.lib.lx06_control_asr_close(state)

    def feed(self,state,data,now,protect=1):
        buf=ctypes.create_string_buffer(data) if data else None
        return self.lib.lx06_control_asr_feed(state,protect,buf,len(data),now)

    def poll(self,state,out,now):return self.lib.lx06_control_asr_poll(state,out,2048,now)

    def segments(self,state,data,start=1120,end=4080):
        at=0;part=0;pieces=(31998,202,1002,78,16000,2)
        while at<len(data):
            n=min(pieces[part%len(pieces)],len(data)-at)
            self.assertEqual(self.feed(state,data[at:at+n],start+(end-start)*at//len(data)),0)
            at+=n;part+=1

    def end_turn(self,state,speaker,out,now=1100):
        old=speaker.dialog
        speaker.send({'header':{'namespace':'SpeechRecognizer','name':'StopCapture','dialog_id':old},'payload':{}})
        self.assertEqual(self.poll(state,out,now),0)
        self.await_condition(lambda:speaker.end_packets==1)
        return old

    def assert_encoded(self,speaker,reference,expected):
        self.assertEqual(len(expected)%640,0)
        count=len(expected)//640
        self.await_condition(lambda:len(speaker.start_packets)==2 and len(speaker.audio_by_dialog[speaker.dialog])==count)
        self.await_condition(lambda:reference.audio_packets==count)
        self.assertEqual(speaker.audio_by_dialog[speaker.dialog],reference.audio_by_dialog[reference.dialog])

    def test_three_second_final_wait_and_restart_delay_preserve_first_syllable_and_partial_frame(self):
        data=self.pcm(103040)
        with self.client() as (a,speaker,out),self.client() as (baseline,reference,_):
            old=self.end_turn(a,speaker,out)
            self.segments(a,data[:96002])
            self.assertEqual(speaker.audio_packets,0)
            speaker.emit('等一下',dialog=old)
            self.assertEqual(self.poll(a,out,4100),1) # FINAL arrives at the existing 3 s limit.
            self.assertEqual(out.value.decode(),'等一下')
            self.segments(a,data[96002:102400],4100,4280) # Existing 200 ms ready_after.
            self.assertEqual(self.feed(a,data[102400:102718],4300),0)
            self.assertEqual(self.feed(a,data[102718:],4320),0)
            self.segments(baseline,data)
            self.assert_encoded(speaker,reference,data)
            self.assertEqual(speaker.cancel_packets,0)
            self.assertEqual(self.lib.test_growth_attempts(),1)
            self.assertEqual(self.lib.test_live_banks(),1)

    def test_final_timeout_restarts_with_complete_pending_audio_without_changing_endpoint(self):
        data=self.pcm(96640)
        with self.client() as (a,speaker,out),self.client() as (baseline,reference,_):
            self.end_turn(a,speaker,out)
            self.segments(a,data[:96002])
            self.assertEqual(self.poll(a,out,4099),0);self.assertEqual(speaker.cancel_packets,0)
            self.assertEqual(self.poll(a,out,4100),0)
            self.await_condition(lambda:speaker.cancel_packets==1)
            self.assertEqual(self.feed(a,data[96002:],4100),0)
            self.segments(baseline,data)
            self.assert_encoded(speaker,reference,data)

    def test_overflow_keeps_latest_four_seconds_and_control_keeps_two_seconds(self):
        data=self.pcm(161242);tail=self.pcm(640)
        for profile,capacity in ((1,128000),(0,64000)):
            with self.subTest(profile=profile):
                self.lib.test_fail_growth(0)
                with self.client(profile) as (a,speaker,out),self.client(profile) as (baseline,reference,_):
                    old=self.end_turn(a,speaker,out);self.segments(a,data)
                    speaker.emit('上一句话',dialog=old);self.assertEqual(self.poll(a,out,4100),1)
                    self.assertEqual(self.feed(a,tail[:200],4300),0)
                    self.assertEqual(self.feed(a,tail[200:],4320),0)
                    expected=data[-capacity:]+tail;self.segments(baseline,expected)
                    self.assert_encoded(speaker,reference,expected)
                    self.assertEqual(self.lib.test_growth_attempts(),1 if profile==1 else 0)
                self.assertEqual(self.lib.test_live_banks(),0)

    def test_optional_bank_oom_keeps_newest_bounded_audio_and_does_not_retry_every_frame(self):
        self.lib.test_fail_growth(1)
        data=self.pcm(161242);tail=self.pcm(640)
        with self.client() as (a,speaker,out),self.client() as (baseline,reference,_):
            old=self.end_turn(a,speaker,out);self.segments(a,data)
            self.assertEqual(self.lib.test_growth_attempts(),1)
            self.assertEqual(self.lib.test_live_banks(),0)
            speaker.emit('上一句话',dialog=old);self.assertEqual(self.poll(a,out,4100),1)
            self.assertEqual(self.feed(a,tail,4300),0)
            expected=data[-64000:]+tail;self.segments(baseline,expected)
            self.assert_encoded(speaker,reference,expected)

    def test_explicit_cancel_discards_pending_without_replaying_an_old_turn(self):
        data=self.pcm(96002);fresh=self.pcm(640)
        with self.client() as (a,speaker,out),self.client() as (baseline,reference,_):
            self.end_turn(a,speaker,out);self.segments(a,data)
            self.assertEqual(self.feed(a,b'',4100,protect=0),0)
            self.assertEqual(self.feed(a,fresh,4300),0)
            self.assertEqual(self.feed(baseline,fresh,4300),0)
            self.assert_encoded(speaker,reference,fresh)


if __name__=='__main__':unittest.main()

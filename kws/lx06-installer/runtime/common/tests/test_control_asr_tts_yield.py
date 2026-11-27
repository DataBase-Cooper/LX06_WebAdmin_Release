"""Opt-in QUESTION turn yield: real seqpacket/Opus, no clouds or microphones."""
import contextlib
import ctypes
import math
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from fake_asr import SpeakerASR

ROOT=Path(__file__).resolve().parents[3]


class TTSYieldTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory()
        library=Path(cls.tmp.name)/'asr.so'
        subprocess.run(['gcc','-shared','-fPIC','-std=c11','-O2','-Wall','-Wextra','-Werror',
                        str(ROOT/'runtime/common/control_asr.c'),str(ROOT/'runtime/doubao/src/json.c'),
                        '-lopus','-o',str(library)],check=True)
        cls.lib=ctypes.CDLL(str(library))
        for name,args,result in [
            ('open_profile',[ctypes.c_char_p,ctypes.c_int],ctypes.c_void_p),
            ('feed',[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64],ctypes.c_int),
            ('poll',[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64],ctypes.c_int),
            ('tts_yield',[ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint,ctypes.c_uint64],ctypes.c_int),
            ('tts_resume',[ctypes.c_void_p,ctypes.c_uint64],ctypes.c_int),
            ('tts_priority_enable',[ctypes.c_void_p],ctypes.c_int),
            ('tts_priority',[ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint],ctypes.c_int),
            ('control_hint',[ctypes.c_void_p,ctypes.c_int],None),
            ('reply',[ctypes.c_void_p,ctypes.c_uint64],None),
            ('result_reply',[ctypes.c_void_p],ctypes.c_uint64),
            ('close',[ctypes.c_void_p],None)]:
            function=getattr(cls.lib,'lx06_control_asr_'+name)
            function.argtypes=args;function.restype=result
        cls.voice=struct.pack('<320h',*(round(math.sin(i*math.tau/40)*5000) for i in range(320)))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    @staticmethod
    def await_condition(predicate):
        end=time.monotonic()+2
        while not predicate():
            if time.monotonic()>end:raise AssertionError('fixture packet not delivered')
            time.sleep(.002)

    @contextlib.contextmanager
    def client(self,profile=1,priority=False):
        with SpeakerASR() as speaker:
            a=self.lib.lx06_control_asr_open_profile(speaker.path.encode(),profile)
            self.assertTrue(a);out=ctypes.create_string_buffer(2048)
            try:
                if priority:self.assertEqual(self.lib.lx06_control_asr_tts_priority_enable(a),1)
                self.assertEqual(self.feed(a,b'\0'*640,1000),0)
                self.await_condition(lambda:bool(speaker.dialog))
                self.assertEqual(self.lib.lx06_control_asr_poll(a,out,2048,1050),0)
                yield a,speaker,out
            finally:
                speaker.stopping=True  # Closing our fixture client is expected.
                self.lib.lx06_control_asr_close(a)

    def feed(self,a,data,now,protect=1):
        buf=ctypes.create_string_buffer(data) if data else None
        return self.lib.lx06_control_asr_feed(a,protect,buf,len(data),now)

    def poll(self,a,out,now):
        return self.lib.lx06_control_asr_poll(a,out,2048,now)

    def test_control_profile_and_unopted_question_keep_existing_stream(self):
        for profile in (0,1):
            with self.subTest(profile=profile),self.client(profile) as (a,speaker,out):
                if profile==0:
                    self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1400,1000,900),0)
                # QUESTION without an explicit yield call stays continuous too.
                self.assertEqual(self.feed(a,self.voice,1420),0)
                self.await_condition(lambda:speaker.audio_packets==2)
                self.assertEqual(speaker.end_packets,0);self.assertEqual(speaker.cancel_packets,0)
                self.assertEqual(len(speaker.start_packets),1)

    def test_guards_refuse_short_silence_old_turn_partial_and_invalid_hold(self):
        with self.client() as (a,speaker,out):
            for now,hold,quiet in [(1200,1000,1000),(1400,1001,900),(1400,0,900),
                                   (1400,1000,1100),(1400,1000,0),(1400,1000,1500)]:
                self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,now,hold,quiet),0)
            speaker.emit('一条尚未完成的用户话语',False,dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,1420),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1500,1000,900),0)
            self.assertEqual(speaker.end_packets,0)

    def test_new_voice_replays_window_pcm_and_controls_survive_late_old_final(self):
        for control in ('等一下','结束对话'):
            with self.subTest(control=control),self.client() as (a,speaker,out):
                old=speaker.dialog;self.lib.lx06_control_asr_reply(a,7)
                self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1300,1000,900),1)
                self.await_condition(lambda:speaker.end_packets==1)
                before=speaker.audio_packets
                for frame in range(15):
                    self.assertEqual(self.feed(a,b'\0'*640,1320+20*frame),0)
                self.assertEqual(speaker.audio_packets,before)
                # Actual new voice is reported by the caller before this PCM
                # frame is fed; the held silence plus voice is sent once.
                self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1620),0)
                self.assertEqual(self.feed(a,self.voice,1620),0)
                self.await_condition(lambda:len(speaker.start_packets)==2 and speaker.audio_packets==before+16)
                new=speaker.dialog;self.assertNotEqual(new,old)
                self.assertEqual(len(speaker.audio_by_dialog[new]),16)
                self.assertEqual(speaker.cancel_packets,1)
                self.assertEqual(self.poll(a,out,1630),0)
                speaker.emit('这条旧结束语必须失效',dialog=old)
                self.assertEqual(self.poll(a,out,1640),0)
                speaker.emit(control,False,dialog=new)
                self.assertEqual(self.poll(a,out,1650),0)
                self.lib.lx06_control_asr_reply(a,8)
                speaker.emit(control,dialog=new)
                self.assertEqual(self.poll(a,out,1660),1);self.assertEqual(out.value.decode(),control)
                self.assertEqual(self.lib.lx06_control_asr_result_reply(a),7)
                speaker.emit(control,dialog=old)
                self.assertEqual(self.poll(a,out,1670),0)

    def test_deadline_and_explicit_synthesis_completion_restart_immediately(self):
        for explicit in (False,True):
            with self.subTest(explicit=explicit),self.client() as (a,speaker,out):
                self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1400,1000,900),1)
                self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1500,1000,900),0)
                self.assertEqual(self.feed(a,b'\0'*640,1600),0)
                now=1700 if explicit else 2400
                if explicit:self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,now),0)
                self.assertEqual(self.feed(a,self.voice,now),0)
                self.await_condition(lambda:len(speaker.start_packets)==2)
                self.assertEqual(self.poll(a,out,now+10),0)
                self.await_condition(lambda:len(speaker.audio_by_dialog[speaker.dialog])==2)
                speaker.emit('等一下',dialog=speaker.dialog)
                self.assertEqual(self.poll(a,out,now+20),1)

    def test_late_partial_aborts_yield_and_preserves_original_final(self):
        with self.client() as (a,speaker,out):
            old=speaker.dialog
            self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1400,1000,900),1)
            speaker.emit('结束',False,dialog=old)
            self.assertEqual(self.poll(a,out,1450),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1500),0)
            self.assertEqual(self.feed(a,self.voice,1520),0)
            self.await_condition(lambda:speaker.end_packets==1)
            self.assertEqual(speaker.cancel_packets,0);self.assertEqual(len(speaker.start_packets),1)
            speaker.emit('结束对话',dialog=old)
            self.assertEqual(self.poll(a,out,1600),1)
            self.assertEqual(out.value.decode(),'结束对话')
            self.assertEqual(self.feed(a,b'\0'*640,1820),0)
            self.await_condition(lambda:len(speaker.start_packets)==2)
            self.await_condition(lambda:len(speaker.audio_by_dialog[speaker.dialog])==2)

    def test_empty_final_during_hold_keeps_new_pcm_and_ignores_old_ready(self):
        with self.client() as (a,speaker,out):
            old=speaker.dialog
            self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1400,1000,900),1)
            speaker.emit('',dialog=old)
            self.assertEqual(self.poll(a,out,1450),0)
            self.assertEqual(self.feed(a,b'\0'*640,1500),0)
            self.assertEqual(self.feed(a,b'\0'*640,1520),0)
            time.sleep(.01);self.assertEqual(len(speaker.start_packets),1)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1600),0)
            self.assertEqual(self.feed(a,self.voice,1600),0)
            self.await_condition(lambda:len(speaker.start_packets)==2)
            new=speaker.dialog
            speaker.send({'type':'ready','dialog':old,'epoch':1})
            self.assertEqual(self.poll(a,out,1610),0)
            self.await_condition(lambda:len(speaker.audio_by_dialog[new])==3)
            self.assertEqual(speaker.cancel_packets,0)
            speaker.emit('结束对话',dialog=new)
            self.assertEqual(self.poll(a,out,1620),1)
            self.assertEqual(out.value.decode(),'结束对话')

    def test_hold_pending_audio_is_bounded_and_stop_or_disconnect_cleans_it(self):
        with self.client() as (a,speaker,out):
            self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1400,1000,900),1)
            # QUESTION retains at most four seconds, even if a stalled caller
            # presents more PCM in a burst while the turn is held.
            for _ in range(9):self.assertEqual(self.feed(a,self.voice*25,1500),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1600),0)
            self.assertEqual(self.feed(a,b'',1600),0)
            self.await_condition(lambda:len(speaker.start_packets)==2 and speaker.audio_packets==201)
            self.assertEqual(len(speaker.audio_by_dialog[speaker.dialog]),200)
            self.assertEqual(self.feed(a,b'',1620,protect=0),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1640),0)
        with self.client() as (a,speaker,out):
            self.assertEqual(self.lib.lx06_control_asr_tts_yield(a,1400,1000,900),1)
            speaker.stopping=True;speaker.conn.shutdown(socket.SHUT_RDWR)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1500),-1)

    def test_priority_replays_weak_voice_without_silence_or_cloud_hypothesis(self):
        with self.client(priority=True) as (a,speaker,out):
            weak=struct.pack('<320h',*([12,-12]*160))
            self.assertEqual(self.feed(a,weak,1100),0)
            self.assertEqual(self.feed(a,self.voice,1120),0)
            old=speaker.dialog
            self.await_condition(lambda:len(speaker.audio_by_dialog[old])==3)
            packets=list(speaker.audio_by_dialog[old])
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1400,500),1)
            self.await_condition(lambda:speaker.cancel_packets==1)
            speaker.emit('旧识别结束对话',dialog=old)
            self.assertEqual(self.poll(a,out,1450),0)
            for i in range(10):self.assertEqual(self.feed(a,weak,1460+i*20),0)
            self.assertEqual(speaker.audio_packets,3)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1700),0)
            self.assertEqual(self.feed(a,self.voice,1700),0)
            self.await_condition(lambda:len(speaker.start_packets)==2 and len(speaker.audio_by_dialog[speaker.dialog])==14)
            self.assertEqual(speaker.audio_by_dialog[speaker.dialog][:3],packets)
            self.assertEqual(self.poll(a,out,1710),0)
            speaker.emit('等一下',dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,1720),1)
            self.assertEqual(out.value.decode(),'等一下')

    def test_priority_batches_replay_and_preserves_continuation_until_deadline(self):
        with self.client(priority=True) as (a,speaker,out):
            for i in range(120):self.assertEqual(self.feed(a,self.voice,1100+i*20),0)
            old=speaker.dialog
            self.await_condition(lambda:len(speaker.audio_by_dialog[old])==121)
            packets=list(speaker.audio_by_dialog[old])
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,4000,500),1)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,4050,500),0)
            self.assertEqual(self.feed(a,self.voice,4100),0)
            for i in range(10):
                self.assertEqual(self.feed(a,b'',4500+i*20),0)
                self.assertEqual(self.poll(a,out,4500+i*20),0)
                time.sleep(.002)
            self.await_condition(lambda:len(speaker.start_packets)==2 and len(speaker.audio_by_dialog[speaker.dialog])==122)
            self.assertEqual(speaker.audio_by_dialog[speaker.dialog][:121],packets)
            speaker.emit('结束对话',dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,4800),1)
            self.assertEqual(out.value.decode(),'结束对话')

    def test_priority_replays_known_partial_with_original_reply_and_leaves_control_profile(self):
        with self.client(profile=0) as (a,speaker,out):
            self.assertEqual(self.lib.lx06_control_asr_tts_priority_enable(a),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1400,500),0)
        with self.client(priority=True) as (a,speaker,out):
            self.lib.lx06_control_asr_reply(a,17)
            self.assertEqual(self.feed(a,self.voice,1200),0)
            old=speaker.dialog
            speaker.emit('等一',False,dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,1400),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1500,500),1)
            self.await_condition(lambda:speaker.cancel_packets==1)
            self.lib.lx06_control_asr_reply(a,18)
            speaker.emit('结束对话',dialog=old)
            self.assertEqual(self.poll(a,out,1600),0)
            self.assertEqual(self.feed(a,self.voice,2000),0)
            self.await_condition(lambda:len(speaker.start_packets)==2 and len(speaker.audio_by_dialog[speaker.dialog])==3)
            self.assertEqual(self.poll(a,out,2010),0)
            speaker.emit('等一下',dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,2020),1)
            self.assertEqual(out.value.decode(),'等一下')
            self.assertEqual(self.lib.lx06_control_asr_result_reply(a),17)

    def test_prefetch_reserves_remaining_window_without_extra_cancel_or_lost_audio(self):
        with self.client(priority=True) as (a,speaker,out):
            weak=struct.pack('<320h',*([12,-12]*160))
            self.assertEqual(self.feed(a,self.voice[:200],1100),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1400,500),1)
            self.await_condition(lambda:speaker.cancel_packets==1)
            self.assertEqual(self.feed(a,weak,1450),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1500),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1500,500),1)
            self.assertEqual(self.feed(a,weak,1600),0)
            self.assertEqual(len(speaker.start_packets),1)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1700),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1890,500),1)
            # Repeated prefetch must not push recognition beyond the first
            # reservation's 1900ms deadline, even with a weak new syllable.
            self.assertEqual(self.feed(a,self.voice[200:],1900),0)
            self.await_condition(lambda:len(speaker.start_packets)==2 and speaker.audio_packets==5)
            self.assertEqual(speaker.cancel_packets,1)
            self.assertEqual(len(speaker.audio_by_dialog[speaker.dialog]),4)
            self.assertEqual(self.poll(a,out,1910),0)
            speaker.emit('结束对话',dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,1920),1)
            self.assertEqual(out.value.decode(),'结束对话')

    def test_priority_declines_when_existing_pending_audio_needs_buffer_headroom(self):
        with self.client(priority=True) as (a,speaker,out):
            speaker.emit('上一句话',dialog=speaker.dialog)
            self.assertEqual(self.poll(a,out,1400),1)
            for _ in range(3):self.assertEqual(self.feed(a,self.voice*25,1450),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1500,500),0)
            self.assertEqual(speaker.cancel_packets,0)
            self.assertEqual(self.feed(a,b'',1700),0)
            self.await_condition(lambda:len(speaker.start_packets)==2 and len(speaker.audio_by_dialog[speaker.dialog])==75)

    def test_priority_codec_continuation_matches_uninterrupted_encoder_exactly(self):
        with self.client(priority=True) as (a,speaker,out), self.client() as (baseline,reference,_):
            weak=struct.pack('<320h',*([12,-12]*160))
            before=[self.voice,self.voice[:200]]
            during=[weak,self.voice,weak]
            after=[self.voice[200:],weak]
            for data in before:
                self.assertEqual(self.feed(a,data,1200),0)
                self.assertEqual(self.feed(baseline,data,1200),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_priority(a,1400,500),1)
            for data in during:
                self.assertEqual(self.feed(a,data,1500),0)
                self.assertEqual(self.feed(baseline,data,1500),0)
            self.assertEqual(self.lib.lx06_control_asr_tts_resume(a,1600),0)
            for data in after:
                self.assertEqual(self.feed(a,data,1700),0)
                self.assertEqual(self.feed(baseline,data,1700),0)
            self.await_condition(lambda:len(speaker.start_packets)==2 and len(speaker.audio_by_dialog[speaker.dialog])==7)
            self.await_condition(lambda:reference.audio_packets==7)
            # Whole packet equality includes the first packet spanning the
            # partial PCM frame, and encoder state after the replay boundary.
            self.assertEqual(speaker.audio_by_dialog[speaker.dialog],reference.audio_by_dialog[reference.dialog])

    def test_configured_controls_request_final_despite_noise_without_accepting_partial(self):
        for words in ('等一下','结束对话'):
            with self.subTest(words=words),self.client() as (a,speaker,out):
                dialog=speaker.emit(words,False,dialog=speaker.dialog)
                self.assertEqual(self.poll(a,out,1100),0)
                self.lib.lx06_control_asr_control_hint(a,1)
                self.assertEqual(self.feed(a,self.voice,2500),0)
                speaker.emit(words,False,dialog=dialog)
                self.assertEqual(self.poll(a,out,2600),0)
                self.lib.lx06_control_asr_control_hint(a,1)
                self.assertEqual(self.poll(a,out,2700),0)
                self.await_condition(lambda:speaker.end_packets==1)
                self.assertEqual(out.value,b'')
                speaker.emit(words,dialog=dialog)
                self.assertEqual(self.poll(a,out,2750),1)
                self.assertEqual(out.value.decode(),words)
    def test_ordinary_question_and_control_profile_ignore_hint_changes(self):
        for profile in (0,1):
            with self.subTest(profile=profile),self.client(profile) as (a,speaker,out):
                speaker.emit('请介绍故宫',False,dialog=speaker.dialog)
                self.assertEqual(self.poll(a,out,1100),0)
                self.lib.lx06_control_asr_control_hint(a,1 if profile==0 else 0)
                self.assertEqual(self.feed(a,self.voice,2700),0)
                self.assertEqual(self.poll(a,out,2700),0)
                time.sleep(.02)
                self.assertEqual(speaker.end_packets,1 if profile==0 else 0)
    def test_ipc_backpressure_keeps_all_encoded_packets_in_order(self):
        for profile in (1,):
            with self.subTest(profile=profile),self.client(profile=profile) as (a,speaker,out),self.client(profile=profile) as (baseline,reference,refout):
                speaker.read_gate.clear()
                # Exceed the kernel's seqpacket packet-count capacity while
                # the fake native SDK is deliberately not reading. The old
                # send()==n implementation fails here with EAGAIN.
                for i in range(600):
                    self.assertEqual(self.feed(a,self.voice,1100+i*20),0)
                    self.assertEqual(self.feed(baseline,self.voice,1100+i*20),0)
                speaker.read_gate.set()
                for i in range(1000):
                    self.assertEqual(self.poll(a,out,15000+i),0)
                    if speaker.audio_packets==601:break
                    time.sleep(.002)
                self.assertEqual(speaker.audio_packets,601)
                self.await_condition(lambda:reference.audio_packets==601)
                self.assertEqual(speaker.audio_by_dialog[speaker.dialog],reference.audio_by_dialog[reference.dialog])


if __name__=='__main__':unittest.main()

import ctypes
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
from fake_asr import SpeakerASR

ROOT=Path(__file__).resolve().parents[3]

class ControlASRTest(unittest.TestCase):
    def test_opt_in_question_endpoint_retains_pauses_and_requires_final(self):
        with tempfile.TemporaryDirectory() as temp:
            library=Path(temp)/'asr.so'
            subprocess.run(['gcc','-shared','-fPIC','-O2',str(ROOT/'runtime/common/control_asr.c'),str(ROOT/'runtime/doubao/src/json.c'),'-lopus','-o',str(library)],check=True)
            lib=ctypes.CDLL(str(library))
            lib.lx06_control_asr_open_profile.argtypes=[ctypes.c_char_p,ctypes.c_int];lib.lx06_control_asr_open_profile.restype=ctypes.c_void_p
            lib.lx06_control_asr_feed.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
            lib.lx06_control_asr_poll.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
            lib.lx06_control_asr_close.argtypes=[ctypes.c_void_p]
            old=os.environ.get('LX06_QUESTION_STABLE_MS');os.environ['LX06_QUESTION_STABLE_MS']='1000'
            try:
                for profile in [0,1]:
                    with SpeakerASR() as speaker:
                        state=lib.lx06_control_asr_open_profile(speaker.path.encode(),profile)
                        out=ctypes.create_string_buffer(2048)
                        try:
                            self.assertEqual(lib.lx06_control_asr_feed(state,1,None,0,1000),0)
                            for _ in range(100):
                                if speaker.dialog:break
                                time.sleep(.005)
                            lib.lx06_control_asr_poll(state,out,2048,1050)
                            loud=ctypes.create_string_buffer((500).to_bytes(2,'little',signed=True)*320)
                            quiet=ctypes.create_string_buffer(b'\0\0'*320)
                            self.assertEqual(lib.lx06_control_asr_feed(state,1,loud,640,1060),0)
                            dialog=speaker.emit('请告诉我',False)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,1100),0)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,2000),0)
                            time.sleep(.02);self.assertEqual(speaker.end_packets,0)
                            speaker.emit('请告诉我明天的天气',False,dialog=dialog)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,2050),0)
                            if profile==1:
                                for at in range(2080,3101,20):
                                    self.assertEqual(lib.lx06_control_asr_feed(state,1,quiet,640,at),0)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,3100),0)
                            time.sleep(.02);self.assertEqual(speaker.end_packets,1 if profile==1 else 0)
                            self.assertEqual(out.value,b'')
                            speaker.emit('请告诉我明天的天气。',dialog=dialog)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,3150),1)
                        finally:lib.lx06_control_asr_close(state)
            finally:
                if old is None:os.environ.pop('LX06_QUESTION_STABLE_MS',None)
                else:os.environ['LX06_QUESTION_STABLE_MS']=old
    def test_question_profile_keeps_long_speech_and_legacy_endpoint(self):
        with tempfile.TemporaryDirectory() as temp:
            library=Path(temp)/'asr.so'
            subprocess.run(['gcc','-shared','-fPIC','-std=c11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'runtime/common/control_asr.c'),str(ROOT/'runtime/doubao/src/json.c'),'-lopus','-o',str(library)],check=True)
            lib=ctypes.CDLL(str(library))
            lib.lx06_control_asr_open_profile.argtypes=[ctypes.c_char_p,ctypes.c_int]
            lib.lx06_control_asr_open_profile.restype=ctypes.c_void_p
            lib.lx06_control_asr_feed.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
            lib.lx06_control_asr_poll.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
            lib.lx06_control_asr_close.argtypes=[ctypes.c_void_p]
            self.assertFalse(lib.lx06_control_asr_open_profile(b'/unused',99))
            for profile in [0,1]:
                with SpeakerASR() as speaker:
                    state=lib.lx06_control_asr_open_profile(speaker.path.encode(),profile)
                    self.assertTrue(state);out=ctypes.create_string_buffer(2048)
                    try:
                        self.assertEqual(lib.lx06_control_asr_feed(state,1,None,0,1000),0)
                        for _ in range(100):
                            if speaker.dialog:break
                            time.sleep(.005)
                        self.assertTrue(speaker.dialog)
                        self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,1050),0)
                        loud=ctypes.create_string_buffer((500).to_bytes(2,'little',signed=True)*320)
                        for n in range(6):
                            self.assertEqual(lib.lx06_control_asr_feed(state,1,loud,640,1090+n*1000),0)
                            speaker.emit('这是一个完整的长问题'+str(n),False,dialog=speaker.dialog)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,1100+n*1000),0)
                        time.sleep(.02)
                        self.assertEqual(speaker.end_packets,1 if profile==0 else 0)
                        if profile==1:
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,7700),0)
                            time.sleep(.02);self.assertEqual(speaker.end_packets,0)
                            # QUESTION still has a hard 30 s utterance bound
                            # when no reliable quiet window was observed.
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,32000),0)
                            time.sleep(.02);self.assertEqual(speaker.end_packets,1)
                            speaker.emit('这是超过四秒仍然完整保留的问句。',dialog=speaker.dialog)
                            self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,32100),1)
                            self.assertEqual(out.value.decode(),'这是超过四秒仍然完整保留的问句。')
                    finally:lib.lx06_control_asr_close(state)

    def test_question_repeated_partial_refreshes_stable_clock(self):
        with tempfile.TemporaryDirectory() as temp:
            library=Path(temp)/'asr.so'
            subprocess.run(['gcc','-shared','-fPIC','-std=c11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'runtime/common/control_asr.c'),str(ROOT/'runtime/doubao/src/json.c'),'-lopus','-o',str(library)],check=True)
            lib=ctypes.CDLL(str(library));lib.lx06_control_asr_open_profile.argtypes=[ctypes.c_char_p,ctypes.c_int];lib.lx06_control_asr_open_profile.restype=ctypes.c_void_p
            lib.lx06_control_asr_feed.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64];lib.lx06_control_asr_poll.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64];lib.lx06_control_asr_close.argtypes=[ctypes.c_void_p]
            old=os.environ.get('LX06_QUESTION_STABLE_MS');os.environ['LX06_QUESTION_STABLE_MS']='1000'
            try:
                with SpeakerASR() as speaker:
                    state=lib.lx06_control_asr_open_profile(speaker.path.encode(),1);out=ctypes.create_string_buffer(2048);loud=ctypes.create_string_buffer((500).to_bytes(2,'little',signed=True)*320);quiet=ctypes.create_string_buffer(b'\0\0'*320)
                    lib.lx06_control_asr_feed(state,1,loud,640,1000)
                    while not speaker.dialog:time.sleep(.005)
                    lib.lx06_control_asr_poll(state,out,2048,1050);dialog=speaker.emit('请用两句话',False);lib.lx06_control_asr_poll(state,out,2048,1100)
                    # Same text is a fresh server hypothesis, so it postpones
                    # the stable endpoint even though the visible text repeats.
                    lib.lx06_control_asr_feed(state,1,loud,640,1900);speaker.emit('请用两句话',False,dialog=dialog);lib.lx06_control_asr_poll(state,out,2048,1900)
                    self.assertEqual(lib.lx06_control_asr_poll(state,out,2048,2800),0);self.assertEqual(speaker.end_packets,0)
                    for at in range(2820,3141,20):lib.lx06_control_asr_feed(state,1,quiet,640,at)
                    lib.lx06_control_asr_poll(state,out,2048,3150);time.sleep(.02);self.assertEqual(speaker.end_packets,1)
                    lib.lx06_control_asr_close(state)
            finally:
                if old is None:os.environ.pop('LX06_QUESTION_STABLE_MS',None)
                else:os.environ['LX06_QUESTION_STABLE_MS']=old

    def test_final_correction_duplicates_boundaries_and_disconnect(self):
        with tempfile.TemporaryDirectory() as temp:
            library=Path(temp)/'asr.so'
            subprocess.run(['gcc','-shared','-fPIC','-std=c11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'runtime/common/control_asr.c'),str(ROOT/'runtime/doubao/src/json.c'),'-lopus','-o',str(library)],check=True)
            lib=ctypes.CDLL(str(library))
            lib.lx06_control_asr_open.argtypes=[ctypes.c_char_p];lib.lx06_control_asr_open.restype=ctypes.c_void_p
            lib.lx06_control_asr_feed.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
            lib.lx06_control_asr_poll.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
            lib.lx06_control_asr_close.argtypes=[ctypes.c_void_p]
            lib.lx06_control_asr_reply.argtypes=[ctypes.c_void_p,ctypes.c_uint64]
            lib.lx06_control_asr_result_reply.argtypes=[ctypes.c_void_p]
            lib.lx06_control_asr_result_reply.restype=ctypes.c_uint64
            self.assertFalse(lib.lx06_control_asr_open(str(Path(temp)/'missing').encode()))
            with SpeakerASR() as speaker:
                state=lib.lx06_control_asr_open(speaker.path.encode());self.assertTrue(state)
                out=ctypes.create_string_buffer(1024);pcm=ctypes.create_string_buffer(b'\x01\x02'*320)
                now=[1000]
                def feed(protect=True):
                    now[0]+=250
                    self.assertEqual(lib.lx06_control_asr_feed(state,protect,pcm,640,now[0]),0)
                def poll():return lib.lx06_control_asr_poll(state,out,1024,now[0])
                def ready():
                    end=time.monotonic()+2
                    while not speaker.dialog:
                        self.assertLess(time.monotonic(),end);time.sleep(.005)
                    time.sleep(.01);self.assertEqual(poll(),0)
                try:
                    feed();ready()
                    lib.lx06_control_asr_reply(state,7)
                    dialog=speaker.emit('等一下，这是一条临时识别',False)
                    self.assertEqual(poll(),0)
                    lib.lx06_control_asr_reply(state,8)
                    speaker.emit('灯亮了',dialog=dialog)
                    self.assertEqual(poll(),1);self.assertEqual(out.value.decode(),'灯亮了')
                    self.assertEqual(lib.lx06_control_asr_result_reply(state),7)
                    speaker.emit('等一下',dialog=dialog)
                    self.assertEqual(poll(),0)
                    feed();time.sleep(.02);self.assertEqual(poll(),0)
                    speaker.emit('再见豆包',dialog=dialog);self.assertEqual(poll(),0)
                    speaker.emit('等等')
                    self.assertEqual(poll(),1);self.assertEqual(out.value.decode(),'等等')
                    feed(False);speaker.emit('等一下',dialog=dialog);self.assertEqual(poll(),0)
                    # Audio arriving during cloud endpoint/final latency is
                    # retained for the next utterance, not silently dropped.
                    feed();time.sleep(.02);self.assertEqual(poll(),0)
                    current=speaker.dialog
                    speaker.send({'header':{'namespace':'SpeechRecognizer','name':'StopCapture','dialog_id':current},'payload':{}})
                    self.assertEqual(poll(),0)
                    time.sleep(.02);before=speaker.audio_packets
                    feed();feed();time.sleep(.02)
                    self.assertEqual(speaker.audio_packets,before)
                    speaker.emit('',dialog=current);self.assertEqual(poll(),0)
                    feed();time.sleep(.03);self.assertEqual(poll(),0)
                    self.assertEqual(speaker.audio_packets,before+3)
                    self.assertGreater(speaker.audio_packets,0)
                    speaker.conn.shutdown(socket.SHUT_RDWR)
                    self.assertEqual(poll(),-1)
                finally:lib.lx06_control_asr_close(state)

if __name__=='__main__':unittest.main()

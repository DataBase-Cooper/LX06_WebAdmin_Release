"""Real question reader against the SDK IPC fixture; never calls a platform."""
import json,os,subprocess,sys,tempfile,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent/'common/tests'))
from fake_asr import SpeakerASR

class QuestionReaderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.tool=Path(cls.tmp.name)/'model-asr'
        subprocess.run(['gcc','-O2','-Werror=implicit-function-declaration',str(ROOT/'model-asr.c'),str(ROOT.parent/'common/control_asr.c'),str(ROOT.parent/'doubao/src/json.c'),'-lopus','-o',str(cls.tool)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def test_only_final_text_is_emitted(self):
        with SpeakerASR() as speaker:
            proc=subprocess.Popen([str(self.tool),'--once'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env={**os.environ,'LX06_CONTROL_ASR_SOCKET':speaker.path})
            try:
                proc.stdin.write(b'\0'*640);proc.stdin.flush()
                end=time.monotonic()+2
                while not speaker.dialog:
                    self.assertLess(time.monotonic(),end);time.sleep(.01)
                dialog=speaker.emit('不要返回这条临时结果',False)
                time.sleep(.05)
                speaker.emit('最终问题包含“引号”和\n换行。',dialog=dialog)
                out,err=proc.communicate(timeout=2)
                self.assertEqual(proc.returncode,0,err)
                self.assertEqual(json.loads(out),{'text':'最终问题包含“引号”和\n换行。'})
                self.assertNotIn('问题'.encode(),err)
            finally:
                if proc.poll() is None:proc.kill();proc.communicate()
    def test_termination_exits_without_a_partial_question(self):
        with SpeakerASR() as speaker:
            proc=subprocess.Popen([str(self.tool),'--once'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env={**os.environ,'LX06_CONTROL_ASR_SOCKET':speaker.path})
            try:
                self.assertIn(b'ready',proc.stderr.readline())
                speaker.stopping=True  # Cancel may reset a socket with unread pong/ready.
                proc.terminate();out,_=proc.communicate(timeout=2)
                self.assertEqual(proc.returncode,130);self.assertEqual(out,b'')
            finally:
                if proc.poll() is None:proc.kill();proc.communicate()

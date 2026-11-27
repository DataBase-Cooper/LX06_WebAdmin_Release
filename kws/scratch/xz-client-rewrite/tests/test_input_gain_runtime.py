"""Measure actual client Opus uplink, rather than only the gain helper."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import unittest
from websockets.sync.server import serve
from test_runtime_integration import OpusCodec

ROOT = Path(__file__).resolve().parents[1]


class InputGainRuntimeTest(unittest.TestCase):
    def measure(self, preset, gain_db=None):
        with tempfile.TemporaryDirectory() as td:
            fifo = Path(td)/'mic'; os.mkfifo(fifo)
            stop = threading.Event(); errors = []; levels = []
            tone = b''.join(struct.pack('<h', round(2000*math.sin(2*math.pi*300*i/16000))) for i in range(960))
            def microphone():
                fd = os.open(fifo, os.O_RDWR)
                try:
                    while not stop.wait(.06): os.write(fd, tone)
                finally: os.close(fd)
            def handler(ws):
                try:
                    self.assertEqual(json.loads(ws.recv())['type'], 'hello')
                    ws.send(json.dumps(dict(type='hello',version=1,transport='websocket',session_id='gain',audio_params=dict(format='opus',sample_rate=24000,channels=1,frame_duration=60))))
                    codec = OpusCodec()
                    while len(levels)<30:
                        data=ws.recv(timeout=3)
                        if isinstance(data,bytes): levels.append(codec.decode_rms(data,16000,960))
                    ws.send(json.dumps(dict(type='goodbye')))
                    try:
                        while True: ws.recv(timeout=2)
                    except Exception: pass
                except BaseException as exc: errors.append(exc)
            with serve(handler,'127.0.0.1',0) as server:
                thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
                writer=threading.Thread(target=microphone,daemon=True);writer.start()
                env=dict(os.environ,XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',XZ_DEVICE_ID='02:00:00:00:00:01',XZ_CLIENT_ID='00000000-0000-4000-8000-000000000001',XZ_ACCESS_TOKEN='fixture',XZ_MIC_FIFO=str(fifo),XZ_PLAYBACK='0',XZ_BARGE_ANY='1',XZ_LOCAL_AEC='0',XZ_INPUT_GAIN=preset)
                if gain_db is not None:env['XZ_INPUT_GAIN_DB']=gain_db
                else:env.pop('XZ_INPUT_GAIN_DB',None)
                try: result=subprocess.run([str(ROOT/'build/xz_client')],env=env,capture_output=True,text=True,timeout=6)
                finally: stop.set();writer.join(1)
                server.shutdown();thread.join(2)
            self.assertFalse(errors,repr(errors))
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            return sum(levels[-8:])/8

    def test_four_presets_reach_encoded_uplink(self):
        levels=[self.measure(p) for p in ['无增益','低增益','中增益','高增益']]
        self.assertGreater(levels[0],500,levels)
        for actual,expected in zip(levels,[1,1.5,2,3]):
            # Lossy speech coding is not amplitude-linear for a pure tone.
            # Allow 15% codec variation while checking every actual preset.
            self.assertAlmostEqual(actual/levels[0],expected,delta=.15*expected,msg=str(levels))

    def test_signed_decimal_gain_reaches_real_encoded_cloud_audio(self):
        base=self.measure('无增益','0.00')
        for db in ['-6.00','3.25','6.00']:
            level=self.measure('高增益',db) # Explicit dB must beat a legacy preset.
            ratio=10**(float(db)/20)
            self.assertAlmostEqual(level/base,ratio,delta=.18*ratio,msg=(db,level/base))


if __name__=='__main__': unittest.main()

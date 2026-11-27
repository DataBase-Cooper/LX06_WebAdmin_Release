"""Real client/Opus: native uplink tail starts at player exit, then resumes."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from websockets.sync.server import serve
from test_runtime_integration import OpusCodec

ROOT = Path(__file__).resolve().parents[1]


class PlaybackTailTest(unittest.TestCase):
    def exercise(self, tail_ms, new_reply=False):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td); fifo = p/'mic'; os.mkfifo(fifo)
            marker = p/'player-ended'
            player = p/'aplay'
            player.write_text('#!/usr/bin/python3\nimport sys,time\nfrom pathlib import Path\n'
                              'sys.stdin.buffer.read()\ntime.sleep(.35)\n'
                              'Path('+repr(str(marker))+').write_text(str(time.monotonic()))\n')
            player.chmod(0o755)
            stop = threading.Event(); errors = []; levels = []
            tone = b''.join(struct.pack('<h', round(3000*math.sin(2*math.pi*300*i/16000))) for i in range(960))
            def microphone():
                fd = os.open(fifo, os.O_RDWR)
                try:
                    while not stop.wait(.06): os.write(fd, tone)
                finally: os.close(fd)
            def handler(ws):
                try:
                    self.assertEqual(json.loads(ws.recv())['type'], 'hello')
                    ws.send(json.dumps(dict(type='hello',version=1,transport='websocket',session_id='tail',audio_params=dict(format='opus',sample_rate=24000,channels=1,frame_duration=60))))
                    while True:
                        data = ws.recv(timeout=3)
                        if isinstance(data,str) and json.loads(data).get('state')=='start': break
                    ws.send(json.dumps(dict(type='tts',state='start')))
                    ws.send(OpusCodec().encode_constant(24000,1440,2000))
                    ws.send(json.dumps(dict(type='tts',state='stop')))
                    codec = OpusCodec(); changed = False
                    deadline = time.monotonic()+2
                    while time.monotonic()<deadline:
                        data = ws.recv(timeout=2)
                        if not isinstance(data,bytes): continue
                        if marker.exists():
                            elapsed = time.monotonic()-float(marker.read_text())
                            levels.append((elapsed,codec.decode_rms(data,16000,960)))
                            if new_reply and elapsed>.2 and not changed:
                                ws.send(json.dumps(dict(type='tts',state='start')))
                                changed=True
                    ws.send(json.dumps(dict(type='goodbye')))
                    try:
                        while True: ws.recv(timeout=2)
                    except Exception: pass
                except BaseException as exc: errors.append(exc)
            with serve(handler,'127.0.0.1',0) as server:
                thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
                writer=threading.Thread(target=microphone,daemon=True);writer.start()
                env=dict(os.environ,XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',XZ_DEVICE_ID='02:00:00:00:00:01',XZ_CLIENT_ID='00000000-0000-4000-8000-000000000001',XZ_ACCESS_TOKEN='fixture',XZ_MIC_FIFO=str(fifo),XZ_PLAYBACK='1',XZ_APLAY=str(player),XZ_BARGE_ANY='1',XZ_LOCAL_AEC='0',XZ_PLAYBACK_TAIL_MUTE_MS=str(tail_ms),XZ_TIMELINE_FILE='-')
                try: result=subprocess.run([str(ROOT/'build/xz_client')],env=env,capture_output=True,text=True,timeout=6)
                finally: stop.set();writer.join(1)
                server.shutdown();thread.join(2)
            self.assertFalse(errors,repr(errors))
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            early=[v for t,v in levels if .38<t<.53]
            late=[v for t,v in levels if .95<t<1.4]
            self.assertTrue(early and late,levels)
            if tail_ms and not new_reply: self.assertLess(max(early),10,levels)
            else: self.assertGreater(min(early),100,levels)
            self.assertGreater(min(late),100,levels)
            self.assertIn('event=PLAYBACK_TAIL_START duration_ms='+str(tail_ms),result.stdout+result.stderr)

    def test_native_tail_mutes_after_actual_player_exit_and_resumes(self): self.exercise(600)
    def test_zero_disables_tail(self): self.exercise(0)
    def test_new_reply_clears_previous_tail(self): self.exercise(600,True)


if __name__=='__main__': unittest.main()

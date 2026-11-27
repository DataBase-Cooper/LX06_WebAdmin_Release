import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from websockets.sync.server import serve
from test_runtime_integration import OpusCodec

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT.parent.parent/'lx06-installer/runtime/common/tests'))
from fake_asr import SpeakerASR

class DualBargeTest(unittest.TestCase):
    def exercise(self,mode,asr=None):
        errors=[];observed={};codec=OpusCodec()
        def send(ws,obj):ws.send(json.dumps(obj,ensure_ascii=False))
        def receive(ws,kind):
            end=time.monotonic()+4
            while time.monotonic()<end:
                data=ws.recv(timeout=2)
                if isinstance(data,str):
                    obj=json.loads(data)
                    if obj['type']==kind:return obj
            raise AssertionError('missing '+kind)
        def mic(ws,count=3):
            packets=[]
            while len(packets)<count:
                data=ws.recv(timeout=2)
                self.assertIsInstance(data,bytes,'unexpected control message '+repr(data))
                packets.append(data)
            return packets
        def handler(ws):
            try:
                hello=receive(ws,'hello');observed['aec']=hello.get('features',{}).get('aec',False)
                send(ws,{'type':'hello','version':1,'transport':'websocket','session_id':'dual-mode-session','audio_params':{'format':'opus','sample_rate':24000,'channels':1,'frame_duration':60}})
                receive(ws,'listen');start=receive(ws,'listen');observed['mode']=start['mode']
                send(ws,{'type':'tts','state':'start'})
                packets=mic(ws,6)
                levels=[codec.decode_rms(p,16000,960) for p in packets]
                if mode==0:
                    self.assertLess(max(levels),1)
                    asr.emit('电视里有普通说话声');mic(ws,8)
                    partial=asr.emit('等一下，这是临时文本',False)
                    asr.emit('灯亮了',dialog=partial);mic(ws,8)
                    for word in ['等一下','等等','停一下','你说的不对','你说得不对']:
                        asr.emit((word+'，你说得不对，等等，停一下，')*8);self.assertEqual(receive(ws,'abort')['session_id'],'dual-mode-session')
                        self.assertEqual(receive(ws,'listen')['mode'],'auto')
                        ws.send(b'invalid stale audio from canceled reply')
                        send(ws,{'type':'tts','state':'stop'});mic(ws,8)
                        asr.emit(('你说得不对，'+word+'，等等，')*8)
                        packets=mic(ws,55)
                        self.assertGreater(max(codec.decode_rms(p,16000,960) for p in packets),100)
                        send(ws,{'type':'stt','text':word})
                        mic(ws,4)
                        late=asr.emit(word+'，今天的天气怎么样',False);mic(ws,16)
                        send(ws,{'type':'tts','state':'start'});mic(ws,4)
                        asr.emit(word+'，今天的天气怎么样',dialog=late);mic(ws,16)
                    ident=asr.emit('小智',False)
                    send(ws,{'type':'tts','state':'stop'})
                    self.assertEqual(receive(ws,'listen')['mode'],'auto')
                    self.assertEqual(asr.dialog,ident,'playback boundary reset auxiliary recognition')
                    asr.emit('等等，小智再见，再见小智，拜拜了哦，'*8,dialog=ident);self.assertEqual(receive(ws,'goodbye')['session_id'],'dual-mode-session')
                else:
                    self.assertGreater(max(levels),100)
                    send(ws,{'type':'abort','reason':'speech_detected'})
                    self.assertEqual(receive(ws,'listen')['mode'],'realtime')
                    for word in ['等一下，等一下','等一下']:
                        send(ws,{'type':'stt','text':word});mic(ws,8)
                    send(ws,{'type':'tts','state':'start'});mic(ws,4)
                    send(ws,{'type':'stt','text':'等一下，等一下'})
                    receive(ws,'abort');receive(ws,'listen')
                    send(ws,{'type':'stt','text':'等一下'});mic(ws,8)
                    send(ws,{'type':'stt','text':'等等，小智再见，再见小智，拜拜了哦，'*8})
                    receive(ws,'goodbye')
            except BaseException as exc:errors.append(exc)
        with tempfile.TemporaryDirectory() as temp,serve(handler,'127.0.0.1',0) as server:
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            tone=b''.join(struct.pack('<h',round(3000*math.sin(2*math.pi*300*i/16000))) for i in range(16000))
            micfile=Path(temp)/'mic.pcm';micfile.write_bytes(tone*120)
            env=dict(os.environ,XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',XZ_DEVICE_ID='02:00:00:00:00:01',XZ_CLIENT_ID='00000000-0000-4000-8000-000000000001',XZ_ACCESS_TOKEN='fixture',XZ_MIC_FIFO=str(micfile),XZ_PLAYBACK='0',XZ_TIMELINE_FILE='-',XZ_BARGE_ANY=str(mode),XZ_INTERRUPT_TEXT='等一下,等等,停一下,你说的不对,你说得不对',XZ_GOODBYE_TEXT='小智再见,再见小智,拜拜了哦',LX06_CONTROL_ASR_SOCKET=asr.path if asr else '/does-not-exist')
            result=subprocess.run([str(ROOT/'build/xz_client')],env=env,capture_output=True,timeout=12)
            server.shutdown();thread.join(2)
        text=(result.stdout+result.stderr).decode()
        if errors:raise AssertionError(str(errors[0])+'\n'+text[-4000:]) from errors[0]
        self.assertEqual(result.returncode,0,text)
        self.assertEqual(observed['mode'],'auto' if mode==0 else 'realtime')
        self.assertEqual(observed['aec'],mode==1)
        if mode==0:
            self.assertEqual(text.count('event=CONTROL_NON_WORD_IGNORED'),2)
            self.assertEqual(text.count('event=INTERRUPT_WORD'),5)
            self.assertEqual(text.count('event=REPEATED_INTERRUPT_IGNORED'),10)
            self.assertIn('event=GOODBYE_WORD_DETECTED',text)
        else:self.assertIn('event=SERVER_ABORT',text)
    def test_keyword_mode_isolates_primary_and_ends_from_auxiliary(self):
        with SpeakerASR() as asr:self.exercise(0,asr)
    def test_human_mode_checks_playback_and_threshold_before_replaying_speech(self):
        for threshold, interrupts in ((None, True), (8000, False)):
            with self.subTest(threshold=threshold), SpeakerASR() as asr:
                self.check_human_interruption(asr, threshold, interrupts)

    def check_human_interruption(self, asr, threshold, interrupts):
        errors = []
        codec = OpusCodec()

        def wave(rate, frequency, amplitude):
            return [round(amplitude * math.sin(2 * math.pi * frequency * i / rate))
                    for i in range(rate * 60 // 1000)]

        echo = struct.pack('<960h', *wave(16000, 400, 6000))
        voice = struct.pack('<960h', *wave(16000, 1300, 3100))
        reply = codec.encode_pcm(24000, wave(24000, 400, 8500))

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fifo = root / 'mic.fifo'
            os.mkfifo(fifo)
            player = root / 'player'
            player.write_text('#!/bin/sh\ncat >/dev/null\n')
            player.chmod(0o700)

            def handler(ws):
                try:
                    def send(**message):
                        ws.send(json.dumps(message))

                    self.assertEqual(json.loads(ws.recv(timeout=3))['type'], 'hello')
                    send(type='hello', version=1, session_id='human-fixture', transport='websocket',
                         audio_params=dict(format='opus', sample_rate=24000, channels=1, frame_duration=60))
                    self.assertEqual(json.loads(ws.recv(timeout=3))['mode'], 'auto')
                    with fifo.open('wb', buffering=0) as mic:
                        send(type='tts', state='start')
                        send(type='tts', state='sentence_start', text='这是小智正在播放的天气预报')
                        ws.send(reply)
                        time.sleep(.08)
                        for _ in range(10):
                            ws.send(reply)
                            mic.write(echo)
                            packet = ws.recv(timeout=2)
                            self.assertIsInstance(packet, bytes)
                            self.assertLess(codec.decode_rms(packet, 16000, 960), 1)
                            time.sleep(.05)
                        # Even a stray recognizer transcript must be backed by
                        # independent PCM rather than the speaker's loud echo.
                        dialog = asr.emit('请问今天星期几', final=False)
                        for _ in range(4):
                            mic.write(echo)
                            self.assertIsInstance(ws.recv(timeout=2), bytes)
                            time.sleep(.045)
                        asr.emit('这是小智正在播放的天气预报', final=False, dialog=dialog)
                        for _ in range(7):
                            mic.write(voice)
                            self.assertIsInstance(ws.recv(timeout=2), bytes)
                            time.sleep(.045)
                        asr.emit('请问今天星期几', final=False, dialog=dialog)
                        if interrupts:
                            self.assertEqual(json.loads(ws.recv(timeout=2))['type'], 'abort')
                            self.assertEqual(json.loads(ws.recv(timeout=2))['state'], 'start')
                            replayed = [ws.recv(timeout=2) for _ in range(3)]
                            self.assertTrue(all(isinstance(packet, bytes) for packet in replayed))
                            for _ in range(8):
                                mic.write(voice)
                            resumed = replayed + [ws.recv(timeout=2) for _ in range(8)]
                            self.assertGreater(max(codec.decode_rms(p, 16000, 960) for p in resumed), 300)
                            send(type='abort', reason='speech_detected')
                        else:
                            for _ in range(5):
                                mic.write(voice)
                                self.assertIsInstance(ws.recv(timeout=2), bytes)
                            with self.assertRaises(TimeoutError):
                                ws.recv(timeout=.15)
                        send(type='goodbye')
                except BaseException as exc:
                    errors.append(exc)

            with serve(handler, '127.0.0.1', 0) as server:
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                env = dict(os.environ, XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',
                           XZ_DEVICE_ID='02:00:00:00:00:01', XZ_CLIENT_ID='human-fixture',
                           XZ_ACCESS_TOKEN='fixture', XZ_MIC_FIFO=str(fifo), XZ_APLAY=str(player),
                           XZ_WAKE_TEXT='', XZ_TIMELINE_FILE='-', XZ_TALK_LOG='0',
                           XZ_BARGE_ANY='1',
                           LX06_CONTROL_ASR_SOCKET=asr.path)
                if threshold is None:
                    env.pop('XZ_BARGE_RMS', None)
                else:
                    env['XZ_BARGE_RMS'] = str(threshold)
                result = subprocess.run([str(ROOT / 'build/xz_client')], env=env, capture_output=True,
                                        text=True, timeout=12)
                server.shutdown()
                thread.join(2)
        self.assertEqual(errors, [], result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'event=HUMAN_BARGE_READY rms={threshold or 1400}', result.stderr)
        self.assertEqual(result.stderr.count('event=HUMAN_BARGE '), int(interrupts))

    def test_manual_listen_waits_for_the_configured_silence(self):
        frame = struct.pack('<960h', *(
            round(3000 * math.sin(2 * math.pi * 300 * i / 16000)) for i in range(960)))
        silence = bytes(len(frame))
        # Initial silence, two speech segments separated by 1.08 s, and
        # enough trailing silence to trigger the configured 1.5 s stop.
        audio = silence * 20 + frame * 6 + silence * 18 + frame * 6 + silence * 34
        errors = []

        def handler(ws):
            try:
                self.assertEqual(json.loads(ws.recv(timeout=3))['type'], 'hello')
                ws.send(json.dumps({'type': 'hello', 'version': 1, 'transport': 'websocket',
                    'session_id': 'manual-test', 'audio_params': {'format': 'opus',
                    'sample_rate': 24000, 'channels': 1, 'frame_duration': 60}}))
                self.assertEqual(json.loads(ws.recv(timeout=3))['mode'], 'manual')
                for _ in range(75):
                    self.assertIsInstance(ws.recv(timeout=3), bytes)
                stop = json.loads(ws.recv(timeout=3))
                self.assertEqual(stop, {'type': 'listen', 'state': 'stop', 'session_id': 'manual-test'})
                with self.assertRaises(TimeoutError):
                    ws.recv(timeout=.2)
                for state in ('start', 'stop'):
                    ws.send(json.dumps({'type': 'tts', 'state': state}))
                self.assertEqual(json.loads(ws.recv(timeout=3))['mode'], 'manual')
                ws.send(json.dumps({'type': 'goodbye'}))
            except Exception as exc:
                errors.append(exc)

        with tempfile.TemporaryDirectory() as temp, serve(handler, '127.0.0.1', 0) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            micfile = Path(temp) / 'mic.pcm'
            micfile.write_bytes(audio)
            env = dict(os.environ, XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',
                       XZ_DEVICE_ID='02:00:00:00:00:01', XZ_CLIENT_ID='pause-fixture',
                       XZ_ACCESS_TOKEN='fixture', XZ_MIC_FIFO=str(micfile), XZ_WAKE_TEXT='',
                       XZ_PLAYBACK='0', XZ_TALK_LOG='0', XZ_TIMELINE_FILE='-',
                       XZ_LISTEN_PAUSE_MS='1500', XZ_BARGE_ANY='')
            result = subprocess.run([str(ROOT / 'build/xz_client')], env=env,
                                    capture_output=True, text=True, timeout=8)
            server.shutdown()
            thread.join(2)
        self.assertEqual(errors, [], result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr.count('event=LISTEN_STOP '), 1)

    def test_end_word_stops_early_without_sending_its_audio(self):
        for word, recognized in (('Over', 'OVER！'), ('发送', '发送！')):
            with self.subTest(word=word):
                self.check_end_word(word, recognized)

    def check_end_word(self, word, recognized):
        def tone(level):
            return struct.pack('<960h', *(round(level * math.sin(2 * math.pi * 300 * i / 16000))
                                          for i in range(960)))
        silence = bytes(1920)
        preceding = [silence] * 3 + [tone(1700)] * 8 + [silence] * 7
        marker = [tone(11000)] * 8
        errors = []
        observed = {}
        marker_spoken = threading.Event()
        stop_writer = threading.Event()

        with tempfile.TemporaryDirectory() as temp, SpeakerASR() as asr:
            fifo = Path(temp) / 'mic'
            os.mkfifo(fifo)

            def microphone():
                with fifo.open('wb', buffering=0) as stream:
                    for frame in preceding + marker:
                        stream.write(frame)
                        time.sleep(.060)
                    observed['marker_end'] = time.monotonic()
                    marker_spoken.set()
                    while not stop_writer.wait(.060):
                        try:
                            stream.write(silence)
                        except BrokenPipeError:
                            break

            def handler(ws):
                try:
                    self.assertEqual(json.loads(ws.recv(timeout=3))['type'], 'hello')
                    ws.send(json.dumps({'type': 'hello', 'version': 1, 'transport': 'websocket',
                        'session_id': 'cue-test', 'audio_params': {'format': 'opus',
                        'sample_rate': 24000, 'channels': 1, 'frame_duration': 60}}))
                    self.assertEqual(json.loads(ws.recv(timeout=3))['mode'], 'manual')
                    self.assertTrue(marker_spoken.wait(3))
                    self.assertGreaterEqual(len(asr.start_packets), 2)
                    # A result from the previous speech segment must not act.
                    asr.emit(word, dialog=asr.start_packets[0][1])
                    current = asr.emit(recognized, final=False)
                    deadline = time.monotonic() + .9
                    while not asr.end_packets and time.monotonic() < deadline:
                        time.sleep(.005)
                    self.assertGreater(asr.end_packets, 0)
                    asr.emit(recognized, dialog=current)
                    packets = []
                    while True:
                        data = ws.recv(timeout=2)
                        if isinstance(data, str):
                            self.assertEqual(json.loads(data),
                                             {'type':'listen','state':'stop','session_id':'cue-test'})
                            break
                        packets.append(data)
                    observed['stop_after'] = time.monotonic() - observed['marker_end']
                    self.assertEqual(len(packets), len(preceding) - 2)
                    codec = OpusCodec()
                    levels = [codec.decode_rms(packet,16000,960) for packet in packets]
                    self.assertGreater(max(levels), 300)
                    self.assertLess(max(levels), 4000)
                    with self.assertRaises(TimeoutError):
                        ws.recv(timeout=.15)
                    ws.send(json.dumps({'type':'goodbye'}))
                except Exception as exc:
                    errors.append(exc)
                finally:
                    stop_writer.set()

            with serve(handler,'127.0.0.1',0) as server:
                peer = threading.Thread(target=server.serve_forever,daemon=True)
                writer = threading.Thread(target=microphone,daemon=True)
                peer.start()
                writer.start()
                env = dict(os.environ, XZ_WS_URL=f'ws://127.0.0.1:{server.socket.getsockname()[1]}/',
                           XZ_DEVICE_ID='02:00:00:00:00:01', XZ_CLIENT_ID='cue-fixture',
                           XZ_ACCESS_TOKEN='fixture', XZ_MIC_FIFO=str(fifo), XZ_WAKE_TEXT='',
                           XZ_PLAYBACK='0', XZ_TALK_LOG='0', XZ_TIMELINE_FILE='-',
                           XZ_LISTEN_PAUSE_MS='3000', XZ_LISTEN_END_WORD=word, XZ_BARGE_ANY='0',
                           LX06_CONTROL_ASR_SOCKET=asr.path)
                result = subprocess.run([str(ROOT/'build/xz_client')], env=env,
                                        capture_output=True, text=True, timeout=8)
                server.shutdown()
                peer.join(2)
                writer.join(2)
        self.assertEqual(errors, [], result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('reason=end_word', result.stderr)
        self.assertLess(observed['stop_after'], 1.0)
        print(f"PASS end word {word} stop after last cue frame {observed['stop_after']:.3f}s")


if __name__=='__main__':unittest.main()

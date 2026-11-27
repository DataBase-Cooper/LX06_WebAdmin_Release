import base64
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import sys
from websockets.sync.server import serve

ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'build/doubao-host'
sys.path.insert(0,str(ROOT.parent/'common/tests'))
from fake_asr import SpeakerASR

def send(ws,kind,**body):ws.send(json.dumps(dict(type=kind,**body)))

def receive(ws,kind,timeout=5):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        item=json.loads(ws.recv(timeout=max(.01,end-time.monotonic())))
        if item['type']==kind:return item
    raise AssertionError('missing event '+kind)

def microphone_state(ws,muted):
    end=time.monotonic()+2
    while time.monotonic()<end:
        item=json.loads(ws.recv(timeout=2))
        assert item['type']=='input_audio_buffer.append',item
        if (not any(base64.b64decode(item['audio'])))==muted:return item
    raise AssertionError('microphone failed to change protection state')


class RuntimeTest(unittest.TestCase):
    def test_prefills_pcm_before_start_and_flushes_short_finished_reply(self):
        with tempfile.TemporaryDirectory() as td:
            output = Path(td)/'played.pcm'
            player = Path(td)/'player'
            player.write_text('#!/bin/sh\ncat >> "$FIXTURE_OUTPUT"\n')
            player.chmod(0o700)
            first = bytes([1,2])*480
            rest = bytes([3,4])*12000

            def scenario(ws,result):
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(first).decode())
                for _ in range(14):
                    receive(ws,'input_audio_buffer.append')
                self.assertFalse(output.exists() and output.stat().st_size)
                send(ws,'response.output_audio.delta',delta=base64.b64encode(rest).decode())
                send(ws,'response.output_audio.done')
                def wait_pcm(expected):
                    end=time.monotonic()+3
                    while time.monotonic()<end:
                        if output.exists() and output.read_bytes()==expected:
                            return
                        time.sleep(.02)
                    self.fail('queued PCM did not reach the player')
                wait_pcm(first+rest)
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(first).decode())
                send(ws,'response.output_audio.done')
                wait_pcm(first+rest+first)
                send(ws,'session.closed')

            code,text,_=self.run_fixture(scenario,{'DB_PLAYBACK':'1','DB_APLAY':str(player),
                                                  'FIXTURE_OUTPUT':str(output)})
            self.assertEqual(code,0,text)

    def test_protected_human_interruption_rejects_echo_and_replays_speech(self):
        with SpeakerASR() as asr:
            def scenario(ws,result):
                send(ws,'response.output_text.delta',delta='先讲一个窗边听雨的故事。')
                send(ws,'response.output_audio.started')
                send(ws,'response.output_text.delta',delta='你好，我是豆包。接下来我们一起试试音箱上的连续对话。你整理一段科技和生活的简明新闻播报，节奏适中。')
                def verify_silence():
                    for _ in range(12):
                        item=json.loads(ws.recv(timeout=2))
                        self.assertEqual(item['type'],'input_audio_buffer.append')
                        self.assertFalse(any(base64.b64decode(item['audio'])))
                verify_silence()
                asr.emit('你好我是豆包')
                verify_silence()
                asr.emit('窗边听雨的故事')
                verify_silence()
                asr.emit('音响上的连续对话')
                verify_silence()
                asr.emit('我给你整理科技生活的简介')
                verify_silence()
                asr.emit('请问现在几点')
                receive(ws,'response.cancel')
                verify_silence()
                send(ws,'response.canceled')
                send(ws,'response.done')
                self.assertTrue(any(base64.b64decode(microphone_state(ws,False)['audio'])))
                send(ws,'conversation.item.input_audio_transcription.started')
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='请问现在几点')
                send(ws,'response.output_audio.started')
                verify_silence()
                send(ws,'session.closed')
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'1','DB_CONTROL_ASR':'xiaomi',
                                                   'LX06_CONTROL_ASR_SOCKET':asr.path})
            self.assertEqual(code,0,text)
            self.assertIn('playback speech rejected by echo check',text)
            self.assertEqual(text.count('independent speech confirmed'),1)
            self.assertEqual(text.count('session ready'),1)

    def test_extension_multiline_prompt_is_json_data(self):
        prompt='第一行\n第二行："你好"\\继续\t正常对话'
        def scenario(ws,result):
            self.assertEqual(result['create']['session']['instructions'],prompt)
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='豆包再见')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_INSTRUCTIONS':prompt})
        self.assertEqual(code,0,text)

    def test_confirmed_barge_drops_local_playback_after_remote_reply_finishes(self):
        with SpeakerASR() as asr, tempfile.TemporaryDirectory() as td:
            player=Path(td)/'player'
            player.write_text('#!/usr/bin/env python3\nimport os,time\nwhile os.read(0,960): time.sleep(.02)\n')
            player.chmod(0o700)
            def frame(repeat,amplitude,seed):
                pcm=[]
                for _ in range(80):
                    seed=(1664525*seed+1013904223)&0xffffffff
                    pcm.extend([amplitude if seed>>31 else -amplitude]*repeat)
                return struct.pack('<'+'h'*len(pcm),*pcm)
            far=frame(6,2300,7)
            reflection=frame(4,2300,7)
            lead=frame(4,1700,62)
            speech=frame(4,6000,42)
            mic=Path(td)/'mic.pcm'
            mic.write_bytes(bytes(640*8)+reflection*16+lead*16+speech*100)
            def scenario(ws,result):
                send(ws,'response.output_text.delta',delta='窗边听雨，感觉很安静。')
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(far*60).decode())
                send(ws,'response.output_audio.done')
                send(ws,'response.done')
                for _ in range(38):
                    self.assertFalse(any(base64.b64decode(receive(ws,'input_audio_buffer.append')['audio'])))
                asr.emit('窗边听雨',final=False)
                for _ in range(5):
                    self.assertFalse(any(base64.b64decode(receive(ws,'input_audio_buffer.append')['audio'])))
                asr.emit('现在几点',final=False)
                # The cloud has finished. It has no cancel ACK to send.
                first=microphone_state(ws,False)
                self.assertEqual(base64.b64decode(first['audio']),lead)
                self.assertEqual(base64.b64decode(microphone_state(ws,False)['audio']),lead)
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='现在几点')
                send(ws,'session.closed')
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'1','DB_CONTROL_ASR':'xiaomi',
                'LX06_CONTROL_ASR_SOCKET':asr.path,'DB_MIC_FIFO':str(mic),
                'DB_PLAYBACK':'1','DB_APLAY':str(player),'DB_PLAYBACK_BUFFER_MS':'100'})
            self.assertEqual(code,0,text)
            stop=next(line for line in text.splitlines() if 'player_stop_ms=' in line)
            self.assertIn('source=buffered-tail',stop)
            self.assertLess(int(stop.rsplit('=',1)[1]),100)
            self.assertIn('ASR 现在几点',text)

    def test_native_mixed_controls_retain_connection_and_goodbye_wins(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            send(ws,'input_audio_buffer.speech_started');receive(ws,'response.cancel')
            send(ws,'response.canceled')
            for phrase in ['等一下，等等，停一下，'*8,'你说得不对，你说的不对，'*8]:
                send(ws,'conversation.item.input_audio_transcription.completed',transcript=phrase)
                for _ in range(8):self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='现在几点了')
            send(ws,'response.output_audio.started')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='你说得不对，等等，'*8)
            receive(ws,'response.cancel');send(ws,'response.canceled')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='等等，豆包再见，再见豆包，拜拜了哦，'*8)
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario)
        self.assertEqual(code,0,text)
        self.assertEqual(text.count('session ready'),1)
        self.assertEqual(text.count('interrupt phrase; connection retained'),1)
        self.assertEqual(text.count('goodbye; closing session'),1)

    def test_audio_spoken_during_cancel_is_replayed_in_order(self):
        with SpeakerASR() as asr, tempfile.TemporaryDirectory() as td:
            mic=Path(td)/'mic.pcm'
            mic.write_bytes(b''.join(struct.pack('<h',i)*320 for i in range(1,1501)))
            def scenario(ws,result):
                seen=1  # run_fixture consumed the first microphone packet
                send(ws,'response.output_audio.started');asr.emit('等一下')
                while True:
                    item=json.loads(ws.recv(timeout=2))
                    if item['type']=='response.cancel':break
                    self.assertEqual(item['type'],'input_audio_buffer.append');seen+=1
                first_saved=seen+1
                for _ in range(12):
                    item=json.loads(ws.recv(timeout=2))
                    self.assertEqual(item['type'],'input_audio_buffer.append')
                    self.assertFalse(any(base64.b64decode(item['audio'])))
                send(ws,'response.canceled');send(ws,'response.done')
                first=microphone_state(ws,False)
                self.assertEqual(struct.unpack_from('<h',base64.b64decode(first['audio']))[0],first_saved)
                for i in range(1,5):
                    item=receive(ws,'input_audio_buffer.append')
                    self.assertEqual(struct.unpack_from('<h',base64.b64decode(item['audio']))[0],first_saved+i)
                asr.emit('豆包再见');receive(ws,'session.close')
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_CONTROL_ASR':'xiaomi','LX06_CONTROL_ASR_SOCKET':asr.path,'DB_MIC_FIFO':str(mic)})
            self.assertEqual(code,0,text)

    def test_listening_final_endpoints_stalled_primary_once(self):
        with SpeakerASR() as asr:
            def scenario(ws,result):
                send(ws,'response.output_audio.started');microphone_state(ws,True)
                asr.emit('等一下');receive(ws,'response.cancel')
                send(ws,'response.canceled');send(ws,'response.done');microphone_state(ws,False)
                send(ws,'conversation.item.input_audio_transcription.started')
                asr.emit('等一下，等一下，今天的天气怎么样')
                receive(ws,'input_audio_buffer.commit')
                send(ws,'conversation.item.input_audio_transcription.started')
                asr.emit('等一下')
                for _ in range(16):self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='今天的天气怎么样')
                send(ws,'response.output_audio.started');microphone_state(ws,True)
                asr.emit('再见豆包，再见豆包');receive(ws,'session.close')
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_CONTROL_ASR':'xiaomi','LX06_CONTROL_ASR_SOCKET':asr.path})
            self.assertEqual(code,0,text)
            self.assertEqual(text.count('requesting primary final'),1)
            self.assertEqual(text.count('interrupt phrase; connection retained'),1)

    def test_followup_final_during_teardown_reaches_same_session_player(self):
        with SpeakerASR() as asr, tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            player = path / 'player'
            output = path / 'played.pcm'
            mic = path / 'mic.pcm'
            player.write_text('#!/usr/bin/python3\nimport os, shutil, sys\n'
                              'with open(os.environ["FIXTURE_OUTPUT"],"ab",buffering=0) as out:\n'
                              ' shutil.copyfileobj(sys.stdin.buffer,out,320)\n')
            player.chmod(0o755)
            mic.write_bytes(b''.join(struct.pack('<h', i) * 320 for i in range(1, 1001)))
            old, fresh, stale = b'\x11\x22' * 160, b'\x33\x44' * 480, b'\x55\x66' * 160

            def wait_for_played(expected):
                until = time.monotonic() + 2
                while time.monotonic() < until:
                    if output.exists() and output.read_bytes() == expected:
                        return
                    time.sleep(.01)
                self.fail('player did not write exact expected PCM')

            def scenario(ws,result):
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(old).decode())
                wait_for_played(old)
                microphone_state(ws,True)
                asr.emit('等一下')
                receive(ws,'response.cancel')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(stale).decode())
                # The new question ends while teardown still holds the PCM.
                microphone_state(ws,True)
                asr.emit('今天的天气怎么样')
                for _ in range(7):
                    microphone_state(ws,True)
                send(ws,'response.canceled')
                send(ws,'response.done')
                first = base64.b64decode(microphone_state(ws,False)['audio'])
                second = base64.b64decode(microphone_state(ws,False)['audio'])
                self.assertEqual(struct.unpack_from('<h',second)[0], struct.unpack_from('<h',first)[0]+1)
                send(ws,'conversation.item.input_audio_transcription.started')
                receive(ws,'input_audio_buffer.commit',timeout=2)
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='今天的天气怎么样')
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(fresh).decode())
                send(ws,'response.output_audio.done')
                wait_for_played(old+fresh)
                asr.emit('豆包再见')
                receive(ws,'session.close')
                self.assertNotIn('guard',result)

            code,text,_ = self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_CONTROL_ASR':'xiaomi',
                'LX06_CONTROL_ASR_SOCKET':asr.path,'DB_MIC_FIFO':str(mic),'DB_PLAYBACK':'1',
                'DB_APLAY':str(player),'FIXTURE_OUTPUT':str(output)})
            self.assertEqual(code,0,text)
            self.assertEqual(text.count('session ready'),1)
            self.assertEqual(text.count('requesting primary final'),1)

    def test_empty_response_done_releases_microphone_after_repeat(self):
        with SpeakerASR() as asr:
            def scenario(ws,result):
                send(ws,'response.output_audio.started')
                receive(ws,'input_audio_buffer.append')
                asr.emit('等一下');receive(ws,'response.cancel')
                send(ws,'response.canceled');send(ws,'response.done');microphone_state(ws,False)
                # Sequence from the affected device: started, then done with
                # no audio and no output_audio.done. Must still resume input.
                send(ws,'response.output_audio.started');microphone_state(ws,True)
                send(ws,'response.done');microphone_state(ws,False)
                asr.emit('等一下，豆包再见，豆包再见');receive(ws,'session.close')
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_CONTROL_ASR':'xiaomi','LX06_CONTROL_ASR_SOCKET':asr.path})
            self.assertEqual(code,0,text)
            self.assertEqual(text.count('goodbye; closing session'),1)

    def test_legacy_observer_and_primary_cannot_cancel_same_response_twice(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            for _ in range(3):receive(ws,'input_audio_buffer.append')
            send(result['guard'],'conversation.item.input_audio_transcription.started')
            send(result['guard'],'conversation.item.input_audio_transcription.completed',transcript='等一下')
            receive(ws,'response.cancel');send(ws,'response.canceled')
            for word in ('等一下，等一下','等一下，告诉我明天的天气'):
                send(ws,'conversation.item.input_audio_transcription.completed',transcript=word)
                for _ in range(6):
                    self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
            send(ws,'response.output_audio.started')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='停一下')
            receive(ws,'response.cancel');send(ws,'response.canceled')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='等一下，豆包再见，豆包再见')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0'})
        self.assertEqual(code,0,text)
        self.assertEqual(text.count('interrupt phrase; connection retained'),2)
        self.assertEqual(text.count('goodbye; closing session'),1)

    def test_repeated_keywords_cancel_once_and_keep_following_microphone(self):
        with SpeakerASR() as asr, tempfile.TemporaryDirectory() as td:
            mic=Path(td)/'mic.pcm';mic.write_bytes(b'\x01\x02'*32000*12)
            def only_audio(ws,count,muted):
                for _ in range(count):
                    item=json.loads(ws.recv(timeout=2))
                    self.assertEqual(item['type'],'input_audio_buffer.append',item)
                    self.assertEqual(not any(base64.b64decode(item['audio'])),muted)
            def scenario(ws,result):
                send(ws,'response.output_audio.started')
                receive(ws,'input_audio_buffer.append');only_audio(ws,4,True)
                asr.emit('等一下');receive(ws,'response.cancel')
                # Separate utterances before and after ACK must neither cancel
                # again nor reset listening. Partial/final replay is also inert.
                asr.emit('等等');only_audio(ws,18,True)
                send(ws,'response.canceled');send(ws,'response.done');microphone_state(ws,False)
                ident=asr.emit('停一下');asr.emit('等一下',dialog=ident)
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='等一下')
                only_audio(ws,24,False)
                late=asr.emit('等一下，今天的天气怎么样',False)
                only_audio(ws,6,False)
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='今天的天气怎么样')
                send(ws,'response.output_audio.started');microphone_state(ws,True)
                only_audio(ws,4,True)
                asr.emit('等一下，今天的天气怎么样',dialog=late)
                only_audio(ws,12,True)
                asr.emit('等一下');receive(ws,'response.cancel')
                # Goodbye must also work while cancellation is pending.
                asr.emit('等一下，再见豆包，拜拜了哦，豆包再见，'*8);receive(ws,'session.close')
                self.assertNotIn('guard',result)
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_CONTROL_ASR':'xiaomi','LX06_CONTROL_ASR_SOCKET':asr.path,'DB_MIC_FIFO':str(mic)})
            self.assertEqual(code,0,text)
            self.assertEqual(text.count('interrupt phrase; connection retained'),2,text)
            self.assertEqual(text.count('goodbye; closing session'),1,text)

    def test_keyword_endpoint_requires_exact_final_at_both_speech_thresholds(self):
        for rms in ('400','4000'):
            with self.subTest(rms=rms), SpeakerASR() as asr, tempfile.TemporaryDirectory() as td:
                mic=Path(td)/'quiet.pcm'
                mic.write_bytes(bytes(640*500))
                def scenario(ws,result):
                    send(ws,'response.output_audio.started')
                    receive(ws,'input_audio_buffer.append')
                    asr.emit('等一下',final=False)
                    for _ in range(26):
                        self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
                    self.assertEqual(asr.end_packets,1)
                    asr.emit('等一下帮我看看时间')
                    for _ in range(13):
                        self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
                    asr.emit('等一下')
                    receive(ws,'response.cancel')
                    send(ws,'response.canceled')
                    send(ws,'session.closed')
                code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_BARGE_RMS':rms,
                    'DB_CONTROL_ASR':'xiaomi','LX06_CONTROL_ASR_SOCKET':asr.path,'DB_MIC_FIFO':str(mic)})
                self.assertEqual(code,0,text)
                self.assertEqual(text.count('keyword candidate ended; requesting final'),1)
                self.assertEqual(text.count('interrupt phrase; connection retained'),1)

    def test_xiaomi_controls_preserve_one_doubao_connection(self):
        with SpeakerASR() as asr:
            def scenario(ws,result):
                send(ws,'response.output_audio.started')
                for _ in range(3):receive(ws,'input_audio_buffer.append')
                asr.emit('这是电视节目里的普通对话')
                for _ in range(6):
                    item=json.loads(ws.recv(timeout=2));self.assertEqual(item['type'],'input_audio_buffer.append')
                    self.assertFalse(any(base64.b64decode(item['audio'])))
                for sentence in ['我等一下要出门', '苹果梨等等都是水果']:
                    asr.emit(sentence)
                    for _ in range(6):
                        item=json.loads(ws.recv(timeout=2))
                        self.assertEqual(item['type'],'input_audio_buffer.append',sentence)
                        self.assertFalse(any(base64.b64decode(item['audio'])))
                ident=asr.emit('等一下后面的临时文本',False)
                asr.emit('灯亮了',dialog=ident)
                for _ in range(6):self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
                for word in ['等一下。','等等','停一下','你说的不对']:
                    send(ws,'response.output_audio.started')
                    for _ in range(3):receive(ws,'input_audio_buffer.append')
                    asr.emit(word);receive(ws,'response.cancel')
                    for _ in range(4):
                        item=json.loads(ws.recv(timeout=2))
                        self.assertEqual(item['type'],'input_audio_buffer.append','unmuted before cancel ACK')
                        self.assertFalse(any(base64.b64decode(item['audio'])))
                    send(ws,'response.canceled');send(ws,'response.done')
                    # Primary and auxiliary can report the same utterance.
                    # Only the auxiliary may execute controls in Xiaomi mode.
                    send(ws,'conversation.item.input_audio_transcription.completed',transcript=word)
                    for _ in range(3):receive(ws,'input_audio_buffer.append')
                send(ws,'conversation.item.input_audio_transcription.started')
                asr.emit('今天的天气怎么样')
                for _ in range(3):receive(ws,'input_audio_buffer.append')
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='今天的天气怎么样')
                send(ws,'response.output_audio.started')
                for _ in range(3):receive(ws,'input_audio_buffer.append')
                ident=asr.emit('豆包',False)
                send(ws,'response.output_audio.done')
                microphone_state(ws,False)
                for _ in range(6):
                    self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
                self.assertEqual(asr.dialog,ident,'playback boundary reset auxiliary recognition')
                asr.emit('豆包再见',dialog=ident);receive(ws,'session.close')
                self.assertNotIn('guard',result)
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_CONTROL_ASR':'xiaomi','LX06_CONTROL_ASR_SOCKET':asr.path})
            self.assertEqual(code,0,text);self.assertEqual(text.count('session ready'),1)
            self.assertEqual(text.count('interrupt phrase; connection retained'),4)
            self.assertEqual(text.count('non-control speech ignored; reply retained'),5)
            self.assertIn('primary microphone resumed',text)

    def run_fixture(self,scenario,extra=None,light=None):
        result={};errors=[]
        def handler(ws):
            try:
                self.assertEqual(ws.request.headers['X-Api-Key'],'fixture-key')
                created=receive(ws,'session.create')
                if created['session']['instructions']=='你是控制词监听助手，所有输入只回答好。':
                    result['guard']=ws
                    result['guard_nonzero']=0
                    result['guard_cancels']=0
                    result['guard_commits']=0
                    send(ws,'session.created')
                    while True:
                        item=json.loads(ws.recv(timeout=12))
                        if item['type']=='session.close':break
                        if item['type']=='input_audio_buffer.append':
                            if any(base64.b64decode(item['audio'])):result['guard_nonzero']+=1
                        elif item['type']=='response.cancel':
                            result['guard_cancels']+=1
                            send(ws,'response.canceled')
                        elif item['type']=='input_audio_buffer.commit':
                            result['guard_commits']+=1
                            send(ws,'input_audio_buffer.committed')
                    return
                result['create']=created
                self.assertEqual(created['session']['audio']['input']['format']['rate'],16000)
                self.assertEqual(created['session']['audio']['output']['format']['rate'],24000)
                send(ws,'session.created')
                self.assertTrue(receive(ws,'speech_text_buffer.commit')['text'])
                packet=receive(ws,'input_audio_buffer.append')
                self.assertEqual(len(base64.b64decode(packet['audio'])),640)
                scenario(ws,result)
            except BaseException as e:errors.append(e)
        with serve(handler,'127.0.0.1',0) as server, tempfile.TemporaryDirectory() as td:
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            port=server.socket.getsockname()[1]
            mic=Path(td)/'mic.pcm';mic.write_bytes(b'\x01\x02'*16000*20)
            env=dict(os.environ,DB_API_KEY='fixture-key',DB_ENDPOINT=f'ws://127.0.0.1:{port}/',
                     DB_BARGE_ANY='1',DB_INTERRUPT='等一下,等等,停一下,你说的不对,你说得不对',DB_AUDIO_FORMAT='pcm_s16le',DB_MIC_FIFO=str(mic),DB_PACE_INPUT='1',DB_PLAYBACK='0',DB_IDLE_SECONDS='2',DB_MAX_SECONDS='10')
            env.update(extra or {})
            if light:
                env.update(LX06_DIALOG_FD=str(light.fileno()),LX06_DIALOG_CHANNEL=os.readlink('/proc/self/fd/'+str(light.fileno())))
            process=subprocess.run([str(BIN)],env=env,capture_output=True,timeout=15,pass_fds=(light.fileno(),) if light else ())
            server.shutdown();thread.join(2)
        if errors:raise errors[0]
        text=(process.stdout+process.stderr).decode()
        self.assertNotIn('fixture-key',text)
        return process.returncode,text,result

    def test_continuous_turns_repeat_wake_and_goodbye(self):
        def scenario(ws,result):
            for word in ['豆包豆包','小智小智','北京今天怎么样']:
                send(ws,'conversation.item.input_audio_transcription.completed',item={'content':[{'transcript':word}]})
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(bytes(4800)).decode())
                send(ws,'response.output_audio.done')
                receive(ws,'input_audio_buffer.append')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='好的，豆包再见。')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario)
        self.assertEqual(code,0,text);self.assertEqual(text.count('session ready'),1)
        self.assertEqual(text.count('audio done bytes=4800'),3)
        self.assertIn('goodbye; closing',text)

    def test_barge_in_cancels_and_discards_stale_audio(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            send(ws,'response.output_audio.delta',delta=base64.b64encode(bytes(4800)).decode())
            send(ws,'conversation.item.input_audio_transcription.started')
            receive(ws,'response.cancel')
            send(ws,'response.canceled')
            send(ws,'response.output_audio.delta',delta='bad old data')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='等一下')
            for _ in range(3):
                self.assertEqual(json.loads(ws.recv(timeout=2))['type'],'input_audio_buffer.append')
            send(ws,'response.output_audio.started')
            send(ws,'response.output_audio.delta',delta=base64.b64encode(bytes(960)).decode())
            send(ws,'response.output_audio.done')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='再见豆包')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario)
        self.assertEqual(code,0,text);self.assertIn('barge-in speech',text)
        self.assertIn('repeated/idle interrupt ignored',text);self.assertIn('audio done bytes=960',text)

    def test_idle_exit_and_output_text_cannot_end_session(self):
        def scenario(ws,result):
            send(ws,'response.output_text.done',text='豆包再见')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_IDLE_SECONDS':'1'})
        self.assertEqual(code,0,text);self.assertIn('idle timeout',text)
        self.assertNotIn('goodbye; closing',text)

    def test_keyword_mode_keeps_playing_through_unrelated_speech(self):
        with tempfile.TemporaryDirectory() as td:
            directory=Path(td);player=directory/'player';output=directory/'audio.pcm'
            player.write_text('#!/usr/bin/python3\nimport os\nwith open(os.environ["FIXTURE_OUTPUT"],"ab",buffering=0) as f:\n while True:\n  b=os.read(0,8192)\n  if not b:break\n  f.write(b)\n')
            player.chmod(0o755)
            samples=bytes(range(256))*64
            mic=directory/'mic.pcm';mic.write_bytes(b'\x01\x02'*32000*10)
            def scenario(ws,result):
                guard=result['guard']
                send(ws,'response.output_audio.started')
                send(ws,'response.output_audio.delta',delta=base64.b64encode(samples).decode())
                for phrase in ['现在几点了','今天电视里说了什么','这是普通的背景说话声']:
                    # The primary must never hear ordinary speech while playing.
                    for _ in range(3):
                        item=receive(ws,'input_audio_buffer.append')
                        self.assertFalse(any(base64.b64decode(item['audio'])))
                    send(guard,'conversation.item.input_audio_transcription.started')
                    send(guard,'conversation.item.input_audio_transcription.completed',transcript=phrase)
                    send(guard,'response.output_audio.started')
                    send(ws,'response.output_audio.delta',delta=base64.b64encode(samples).decode())
                send(ws,'response.output_audio.done')
                deadline=time.monotonic()+3
                while time.monotonic()<deadline and (not output.exists() or output.stat().st_size<len(samples)*4):
                    item=json.loads(ws.recv(timeout=1))
                    self.assertEqual(item['type'],'input_audio_buffer.append',item)
                self.assertEqual(output.read_bytes(),samples*4)
                # A keyword must still cancel after the negative cases.
                send(ws,'response.output_audio.started')
                receive(ws,'input_audio_buffer.append')
                time.sleep(.05)
                send(guard,'conversation.item.input_audio_transcription.started')
                send(guard,'conversation.item.input_audio_transcription.completed',transcript='等一下')
                receive(ws,'response.cancel')
                send(ws,'response.canceled')
                # On interrupt, the original connection receives speech again.
                deadline=time.monotonic()+1
                while True:
                    item=receive(ws,'input_audio_buffer.append')
                    if any(base64.b64decode(item['audio'])):break
                    self.assertLess(time.monotonic(),deadline)
                self.assertGreater(result['guard_nonzero'],0)
                self.assertEqual(result['guard_cancels'],0)
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='再见豆包')
                receive(ws,'session.close')
            code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0','DB_PLAYBACK':'1','DB_MIC_FIFO':str(mic),'DB_APLAY':str(player),'FIXTURE_OUTPUT':str(output)})
            self.assertEqual(code,0,text)
            self.assertNotIn('barge-in speech',text)
            self.assertIn('DB_BARGE_ANY=0',text)
            self.assertIn('interrupt phrase; connection retained',text)
            self.assertEqual(text.count('non-control speech ignored; reply retained'),3)

    def test_all_script_interrupts_and_goodbyes_are_independent(self):
        import re
        template=(ROOT/'doubao_watch.sh.tpl').read_text()
        interrupt=re.search(r"export DB_INTERRUPT='([^']+)'",template)[1]
        goodbye=re.search(r"export DB_GOODBYE='([^']+)'",template)[1]
        for word in goodbye.split(','):
            with self.subTest(goodbye=word):
                def scenario(ws,result):
                    # XiaoZhi's ending is ordinary dialogue for Doubao.
                    send(ws,'conversation.item.input_audio_transcription.completed',transcript='小智再见')
                    receive(ws,'input_audio_buffer.append')
                    for cut in interrupt.split(','):
                        send(ws,'response.output_audio.started')
                        send(ws,'conversation.item.input_audio_transcription.completed',transcript=cut)
                        receive(ws,'response.cancel')
                        send(ws,'response.canceled')
                    send(ws,'conversation.item.input_audio_transcription.completed',transcript=word)
                    receive(ws,'session.close')
                code,text,_=self.run_fixture(scenario,{'DB_INTERRUPT':interrupt,'DB_GOODBYE':goodbye,'DB_BARGE_ANY':'0'})
                self.assertEqual(code,0,text)
                self.assertEqual(text.count('interrupt phrase; connection retained'),len(interrupt.split(',')))
                self.assertEqual(text.count('session ready'),1)
                self.assertIn('goodbye; closing session',text)

    def test_server_error_does_not_print_credentials(self):
        def scenario(ws,result):
            send(ws,'error',message='fixture-key is invalid')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario)
        self.assertNotEqual(code,0);self.assertIn('server error',text)

    def test_all_observer_controls_and_same_primary_connection(self):
        for goodbye in ['豆包再见','再见豆包','拜拜了哦']:
            with self.subTest(goodbye=goodbye):
                def scenario(ws,result):
                    guard=result['guard']
                    for phrase in ['等一下','等等','停一下','你说的不对']:
                        send(ws,'response.output_audio.started')
                        for _ in range(2):receive(ws,'input_audio_buffer.append')
                        send(guard,'conversation.item.input_audio_transcription.started')
                        send(guard,'conversation.item.input_audio_transcription.completed',transcript=phrase)
                        receive(ws,'response.cancel')
                        send(ws,'response.canceled')
                        for _ in range(2):receive(ws,'input_audio_buffer.append')
                    send(ws,'response.output_audio.started')
                    for _ in range(2):receive(ws,'input_audio_buffer.append')
                    send(guard,'conversation.item.input_audio_transcription.started')
                    send(guard,'conversation.item.input_audio_transcription.completed',transcript=goodbye)
                    receive(ws,'session.close')
                code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0'})
                self.assertEqual(code,0,text)
                self.assertEqual(text.count('interrupt phrase; connection retained (keyword observer)'),4)
                self.assertEqual(text.count('session ready'),1)
                self.assertIn('goodbye; closing session',text)

    def test_observer_error_closes_instead_of_disabling_protection(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            send(result['guard'],'error',message='fixture-key private error')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0'})
        self.assertNotEqual(code,0,text)
        self.assertIn('keyword observer unavailable; closing protected session',text)

    def test_late_cancel_ack_does_not_discard_new_reply(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            send(ws,'conversation.item.input_audio_transcription.started')
            receive(ws,'response.cancel')
            send(ws,'response.output_audio.started')
            send(ws,'response.canceled')
            send(ws,'response.output_audio.delta',delta=base64.b64encode(bytes(960)).decode())
            send(ws,'response.output_audio.done')
            send(ws,'conversation.item.input_audio_transcription.completed',transcript='再见豆包')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario)
        self.assertEqual(code,0,text)
        self.assertIn('audio done bytes=960',text)

    def test_observer_partial_only_requests_endpoint_final_controls_cancel(self):
        def scenario(ws,result):
            guard=result['guard']
            send(ws,'response.output_audio.started')
            for _ in range(2):receive(ws,'input_audio_buffer.append')
            send(guard,'conversation.item.input_audio_transcription.started')
            send(guard,'conversation.item.input_audio_transcription.delta',delta='等一下')
            deadline=time.monotonic()+2
            while not result['guard_commits']:
                item=json.loads(ws.recv(timeout=1))
                self.assertEqual(item['type'],'input_audio_buffer.append')
                self.assertLess(time.monotonic(),deadline)
            send(guard,'conversation.item.input_audio_transcription.delta',delta='等一下。')
            # A corrected final must not cancel the main response.
            send(guard,'conversation.item.input_audio_transcription.completed',text='灯亮了')
            for _ in range(5):
                item=json.loads(ws.recv(timeout=1))
                self.assertEqual(item['type'],'input_audio_buffer.append')
            self.assertEqual(result['guard_commits'],1)
            send(guard,'conversation.item.input_audio_transcription.started')
            send(guard,'conversation.item.input_audio_transcription.completed',text='等一下')
            receive(ws,'response.cancel')
            send(ws,'conversation.item.input_audio_transcription.completed',text='豆包再见')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0'})
        self.assertEqual(code,0,text)
        self.assertEqual(text.count('interrupt phrase; connection retained'),1)

    def test_continuous_observer_speech_is_endpointed_without_canceling(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            for _ in range(2):receive(ws,'input_audio_buffer.append')
            guard=result['guard']
            send(guard,'conversation.item.input_audio_transcription.started')
            send(guard,'conversation.item.input_audio_transcription.delta',delta='这是一段持续的电视节目声音')
            deadline=time.monotonic()+5
            while result['guard_commits']==0:
                item=json.loads(ws.recv(timeout=1))
                self.assertEqual(item['type'],'input_audio_buffer.append')
                self.assertLess(time.monotonic(),deadline)
            send(guard,'conversation.item.input_audio_transcription.completed',text='这是一段持续的电视节目声音')
            for _ in range(2):
                self.assertEqual(json.loads(ws.recv(timeout=1))['type'],'input_audio_buffer.append')
            send(ws,'conversation.item.input_audio_transcription.completed',text='豆包再见')
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_BARGE_ANY':'0'})
        self.assertEqual(code,0,text)
        self.assertIn('non-control speech ignored; reply retained',text)
        self.assertNotIn('interrupt phrase',text)

    def test_player_failure_closes_connection(self):
        def scenario(ws,result):
            send(ws,'response.output_audio.started')
            send(ws,'response.output_audio.delta',delta=base64.b64encode(bytes(4800)).decode())
            receive(ws,'session.close')
        code,text,_=self.run_fixture(scenario,{'DB_PLAYBACK':'1','DB_APLAY':'/bin/false'})
        self.assertNotEqual(code,0);self.assertIn('player failed',text)

    def test_real_player_pipe_audio_and_cleanup(self):
        with tempfile.TemporaryDirectory() as td:
            directory=Path(td);player=directory/'player';output=directory/'audio.pcm'
            player.write_text('#!/usr/bin/python3\nimport os,sys\nwith open(os.environ["FIXTURE_OUTPUT"],"ab",buffering=0) as f:\n while True:\n  b=os.read(0,8192)\n  if not b:break\n  f.write(b)\n')
            player.chmod(0o755)
            samples=bytes(range(256))*512
            def scenario(ws,result):
                for _ in range(2):
                    send(ws,'response.output_audio.started')
                    # A PCM sample can span two WebSocket deltas. No byte may
                    # be discarded or rejected at an odd chunk boundary.
                    send(ws,'response.output_audio.delta',delta=base64.b64encode(samples[:1023]).decode())
                    send(ws,'response.output_audio.delta',delta=base64.b64encode(samples[1023:]).decode())
                    send(ws,'response.output_audio.done')
                end=time.monotonic()+3
                while time.monotonic()<end and (not output.exists() or output.stat().st_size<2*len(samples)):time.sleep(.02)
                self.assertEqual(output.read_bytes(),samples*2)
                send(ws,'conversation.item.input_audio_transcription.completed',transcript='豆包再见')
                receive(ws,'session.close')
            code,text,_=self.run_fixture(scenario,{'DB_PLAYBACK':'1','DB_APLAY':str(player),'FIXTURE_OUTPUT':str(output)})
            self.assertEqual(code,0,text)

    def test_lights_wait_for_local_player_after_cloud_done(self):
        reader,writer=socket.socketpair(socket.AF_UNIX,socket.SOCK_DGRAM)
        reader.settimeout(3)
        def state(expected):
            self.assertEqual(reader.recv(2),bytes([expected]))
        try:
            with tempfile.TemporaryDirectory() as td:
                player=Path(td)/'player'
                player.write_text('#!/usr/bin/python3\nimport sys,time\nsys.stdin.buffer.read()\ntime.sleep(.7)\n')
                player.chmod(0o755)
                def scenario(ws,result):
                    state(2) # listening at ready
                    send(ws,'response.output_audio.started');state(3) # thinking
                    send(ws,'response.output_audio.delta',delta=base64.b64encode(bytes(960)).decode())
                    state(4) # first local PCM write
                    start=time.monotonic();send(ws,'response.output_audio.done')
                    state(2) # only once the local player has released output
                    self.assertGreater(time.monotonic()-start,.65)
                    send(ws,'conversation.item.input_audio_transcription.completed',transcript='再见豆包')
                    receive(ws,'session.close');state(0)
                code,text,_=self.run_fixture(scenario,{'DB_PLAYBACK':'1','DB_APLAY':str(player)},writer)
                self.assertEqual(code,0,text)
        finally:reader.close();writer.close()


if __name__=='__main__':unittest.main()

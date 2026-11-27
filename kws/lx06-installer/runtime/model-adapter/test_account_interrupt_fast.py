"""Account voice interruption: actual child stop plus uninterrupted ASR input.

This exercises the C loop and owned OS processes, without provider credentials
or physical/acoustic recognition. Live acceptance must also check the first
control word and the complete subsequent question through the speaker.
"""
import time
import unittest
from pathlib import Path
import test_model_voice as fixture


class FastInterruptTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture.VoiceTest.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        fixture.VoiceTest.tearDownClass.__func__(cls)

    setUp = fixture.VoiceTest.setUp
    start = fixture.VoiceTest.start
    close = fixture.VoiceTest.close
    wait = fixture.VoiceTest.wait
    requests = fixture.VoiceTest.requests
    count = fixture.VoiceTest.count

    def test_first_partial_stops_player_without_closing_followup_utterance(self):
        self.start(PLAY_DELAY='8', TAIL_DELAY='8',
                   ANSWER='这是仍在播放的故事。'*30)
        self.speaker.emit('先讲故事')
        self.wait(lambda:(self.p/'played').exists())
        self.wait(lambda:self.speaker.dialog and self.speaker.dialog!=self.speaker.last_sent)
        self.pcm_frame=b'\x00\x10'*320
        before=(self.speaker.cancel_packets,self.speaker.end_packets,
                len(self.speaker.start_packets))
        began=time.monotonic()
        dialog=self.speaker.emit('等一下',False)
        self.wait(lambda:(self.p/'player-stopped').exists(),timeout=.25)
        self.assertLess(time.monotonic()-began,.25)
        self.assertEqual(self.count('keyword interruption'),1)
        self.assertEqual(len(self.requests()),1)
        # Several controls and an evolving follow-up remain in the SAME ASR
        # turn. No local end/cancel/replay may truncate its final conditions.
        for text in ['等一下，你说得不对，等等',
                     '等一下，你说得不对，等等，请只回答一加一',
                     '等一下，你说得不对，等等，请只回答一加一，只回答数字']:
            self.speaker.emit(text,False,dialog=dialog)
            time.sleep(.12)
        self.assertEqual(before,(self.speaker.cancel_packets,
                                 self.speaker.end_packets,
                                 len(self.speaker.start_packets)))
        self.assertEqual(self.speaker.dialog,dialog)
        self.assertEqual(self.count('keyword interruption'),1)
        self.speaker.emit('等一下，你说得不对，等等，请只回答一加一，只回答数字',dialog=dialog)
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],
                         '请只回答一加一，只回答数字')
        self.assertIsNone(self.proc.poll())

    def test_partial_goodbye_silences_now_but_exits_only_on_final_once(self):
        self.start(PLAY_DELAY='8')
        self.speaker.emit('先讲故事')
        self.wait(lambda:(self.p/'played').exists())
        dialog=self.speaker.emit('结束对话',False)
        self.wait(lambda:(self.p/'player-stopped').exists(),timeout=.25)
        self.assertIsNone(self.proc.poll())
        self.assertEqual(self.count('goodbye'),0)
        self.speaker.emit('结束对话，拜拜了哦，结束对话',False,dialog=dialog)
        time.sleep(.1)
        self.assertEqual(self.count('goodbye pending'),1)
        self.speaker.emit('结束对话，拜拜了哦，结束对话',dialog=dialog)
        self.proc.wait(timeout=1)
        self.assertEqual(self.proc.returncode,0)
        self.wait(lambda:self.count('goodbye')==1)
        self.assertEqual(len(self.requests()),1)

    def test_cancel_three_resistant_jobs_keeps_microphone_moving(self):
        # All three jobs ignore TERM. Playback must be killed by the bounded
        # retirement queue, while the main loop keeps forwarding 20 ms PCM.
        (self.p/'request').write_text(fixture.REQUEST.replace(
            'p=Path(os.environ["FIXTURE"]); data=json.load(sys.stdin)',
            'import signal; signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            'p=Path(os.environ["FIXTURE"]); data=json.load(sys.stdin)'))
        (self.p/'audio').write_text(fixture.AUDIO.replace(
            'data=sys.stdin.buffer.read()',
            'import signal; signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            'data=sys.stdin.buffer.read()'))
        (self.p/'player').write_text(fixture.PLAYER.replace(
            'signal.signal(signal.SIGTERM,stop)',
            'signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            '(Path(os.environ["FIXTURE"])/"player-pid").write_text(str(os.getpid()))'))
        self.start(PLAY_DELAY='8',TAIL_DELAY='8',SYNTH_DELAY='.5',
                   ANSWER='这是仍在播放的长故事。'*40)
        self.speaker.emit('先讲故事')
        self.wait(lambda:(self.p/'player-pid').exists())
        self.wait(lambda:len((self.p/'synthesized').read_text().splitlines())>=2)
        pid=int((self.p/'player-pid').read_text())
        self.pcm_frame=b'\x00\x10'*320
        before=self.speaker.audio_packets
        dialog=self.speaker.emit('等一下',False)
        self.wait(lambda:self.count('keyword interruption')==1,timeout=.2)
        time.sleep(.26)
        self.assertGreaterEqual(self.speaker.audio_packets-before,10)
        stat=Path(f'/proc/{pid}/stat')
        self.assertTrue(not stat.exists() or stat.read_text().split()[2]=='Z')
        self.assertEqual(self.count('speaking'),1)
        self.assertEqual(self.count('answer'),0)
        self.speaker.emit('等一下，现在请只回答一加一',dialog=dialog)
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],
                         '现在请只回答一加一')
        self.assertIsNone(self.proc.poll())

    def test_late_synthesis_output_cannot_become_next_reply_audio(self):
        # Model a provider worker reopening its output after cancellation.
        # The next synthesis has written its own WAV but is not yet complete;
        # a shared filename would let the old callback replace that new audio.
        (self.p/'audio').write_text('''#!/usr/bin/env python3
import os,signal,sys,time
from pathlib import Path
signal.signal(signal.SIGTERM,signal.SIG_IGN)
data=sys.stdin.buffer.read();out=os.readlink('/proc/self/fd/1')
with (Path(os.environ['FIXTURE'])/'synthesized').open('ab') as f:f.write(data+b'\\n')
sys.stdout.close()
if data.decode().startswith('旧'):
 time.sleep(.075)
 with open(out,'wb') as f:f.write(data)
else:
 with open(out,'wb') as f:f.write(data)
 time.sleep(.15)
''')
        (self.p/'request').write_text(fixture.REQUEST.replace(
            'answer=os.environ.get("ANSWER","这是模型的完整回复。")',
            'answer="旧回复禁止复活。" if len((p/"requests").read_text().splitlines())==1 else "新回复正确。"').replace(
            'time.sleep(float(os.environ.get("REQUEST_DELAY","0.05")))',
            'time.sleep(.3 if len((p/"requests").read_text().splitlines())==1 else 0)'))
        self.start(PLAY_DELAY='.05')
        self.speaker.emit('第一个问题')
        self.wait(lambda:(self.p/'synthesized').exists())
        self.speaker.emit('等一下，第二个问题')
        self.wait(lambda:len(self.requests())==2)
        self.wait(lambda:self.count('listening')==1)
        self.assertEqual((self.p/'played').read_text().splitlines(),['新回复正确。'])
        self.assertEqual(self.count('keyword interruption'),1)

    def test_partial_self_controls_and_ordinary_speech_do_not_stop_player(self):
        self.start(PLAY_DELAY='8',ANSWER='现在请说等一下。')
        self.speaker.emit('说明怎么使用')
        self.wait(lambda:(self.p/'played').exists())
        dialog=self.speaker.emit('现在请说等一下',False)
        time.sleep(.15)
        self.assertFalse((self.p/'player-stopped').exists())
        self.assertEqual(self.count('keyword interruption'),0)
        self.speaker.emit('现在请说等一下',dialog=dialog)
        dialog=self.speaker.emit('旁边电视正在播放新闻',False)
        time.sleep(.15)
        self.assertFalse((self.p/'player-stopped').exists())
        self.assertEqual(self.count('keyword interruption'),0)
        self.speaker.emit('旁边电视正在播放新闻',dialog=dialog)
        self.assertEqual(len(self.requests()),1)

    def stream_fixtures(self):
        (self.p/'audio').write_text('''#!/usr/bin/env python3
import os,sys,time
from pathlib import Path
p=Path(os.environ['FIXTURE']);data=sys.stdin.buffer.read()
with (p/'audio-modes').open('a') as f:f.write(sys.argv[1]+'\\n')
time.sleep(float(os.environ.get('FIRST_PCM_DELAY','0')))
if 'MODEL_AUDIO_READY_FD' in os.environ:
 fd=int(os.environ['MODEL_AUDIO_READY_FD']);os.write(fd,b'\\1');os.close(fd)
sys.stdout.buffer.write(data[:3]);sys.stdout.buffer.flush()
time.sleep(float(os.environ.get('SYNTH_DELAY','.8')))
sys.stdout.buffer.write(data[3:]);sys.stdout.buffer.flush()
with (p/'audio-ended').open('a') as f:f.write('done\\n')
''')
        (self.p/'player').write_text(fixture.PLAYER.replace(
            'data=sys.stdin.buffer.read()',
            'data=sys.stdin.buffer.read(3)\n'
            '(Path(os.environ["FIXTURE"])/"first-audio").write_bytes(data)\n'
            'data+=sys.stdin.buffer.read()'))

    def test_stream_first_pcm_reaches_player_before_synthesis_completes(self):
        self.stream_fixtures()
        self.start(MODEL_TTS_STREAM='1',MODEL_TTS_YIELD='1',
                   MODEL_SPEECH_FLOW='连贯优先',PLAY_DELAY='.05',
                   SYNTH_DELAY='.8',ANSWER='这是用于测试边接收边播放的完整回答。'*10)
        began=time.monotonic();self.speaker.emit('请介绍故宫')
        self.wait(lambda:(self.p/'first-audio').exists(),timeout=.6)
        self.assertLess(time.monotonic()-began,.6)
        self.assertFalse((self.p/'audio-ended').exists())
        self.wait(lambda:self.count('speaking')==1)
        self.assertTrue(any('first PCM ' in x for x in self.logs))
        self.assertEqual(self.speaker.cancel_packets,0)
        self.assertEqual(self.count('TTS priority before synthesis'),0)
        self.wait(lambda:self.count('listening')==1,timeout=8)
        modes=(self.p/'audio-modes').read_text().splitlines()
        self.assertGreater(len(modes),1)
        self.assertEqual(set(modes),{'--stream-synthesize'})
        self.assertEqual((self.p/'played').read_text().replace('\n',''),
                         '这是用于测试边接收边播放的完整回答。'*10)

    def test_stream_keyword_silences_player_and_same_asr_accepts_next_question(self):
        self.stream_fixtures()
        self.start(MODEL_TTS_STREAM='1',PLAY_DELAY='8',SYNTH_DELAY='.8')
        self.speaker.emit('先讲故事')
        self.wait(lambda:(self.p/'first-audio').exists())
        dialog=self.speaker.emit('等一下',False)
        before=self.speaker.cancel_packets
        self.wait(lambda:(self.p/'player-stopped').exists(),timeout=.25)
        self.assertEqual(self.count('keyword interruption'),1)
        self.assertEqual(self.speaker.cancel_packets,before)
        self.assertEqual(self.speaker.dialog,dialog)
        self.speaker.emit('等一下，请只回答一加一，只回答数字',dialog=dialog)
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],
                         '请只回答一加一，只回答数字')
        self.assertIsNone(self.proc.poll())

    def test_stream_does_not_treat_unplayed_control_text_as_self_echo(self):
        self.stream_fixtures()
        self.start(MODEL_TTS_STREAM='1',FIRST_PCM_DELAY='.6',
                   ANSWER='现在请说等一下。')
        self.speaker.emit('说明怎么使用')
        self.wait(lambda:(self.p/'audio-modes').exists())
        dialog=self.speaker.emit('等一下',False)
        self.wait(lambda:self.count('keyword interruption')==1,timeout=.25)
        self.assertFalse((self.p/'first-audio').exists())
        self.assertEqual(self.count('speaking'),0)
        self.speaker.emit('等一下',dialog=dialog)
        self.speaker.emit('新的问题')
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'新的问题')


if __name__=='__main__':
    unittest.main()

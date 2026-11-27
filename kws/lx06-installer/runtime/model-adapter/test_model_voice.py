"""Real C event loop, SDK wire fixture, real child cancellation and PCM piping.

These are deterministic protocol tests, not acoustic recognition acceptance.
No credentials, live providers or physical playback are used here.
"""
import contextlib,json,os,subprocess,sys,tempfile,threading,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT.parent/'common/tests'))
from fake_asr import SpeakerASR

REQUEST='''#!/bin/sh
exec python3 -c 'import json,os,sys,time; from pathlib import Path
p=Path(os.environ["FIXTURE"]); data=json.load(sys.stdin)
with (p/"requests").open("a") as f: f.write(json.dumps(data)+"\\n")
time.sleep(float(os.environ.get("REQUEST_DELAY","0.05")))
answer=os.environ.get("ANSWER","这是模型的完整回复。")
for at in range(0,len(answer),100): print(json.dumps({"delta":answer[at:at+100]},ensure_ascii=False),flush=True)
time.sleep(float(os.environ.get("TAIL_DELAY","0")))
print(json.dumps({"done":True}),flush=True)
time.sleep(float(os.environ.get("CLEANUP_DELAY","0")))
sys.exit(int(os.environ.get("REQUEST_EXIT","0")))'
'''
AUDIO='''#!/usr/bin/env python3
import sys,os,time
from pathlib import Path
data=sys.stdin.buffer.read()
with (Path(os.environ["FIXTURE"])/"synthesized").open("ab") as f: f.write(data+b"\\n")
time.sleep(float(os.environ.get("SYNTH_DELAY","0")))
sys.stdout.buffer.write(data)
'''
PLAYER='''#!/usr/bin/env python3
import os,sys,time,signal
from pathlib import Path
def stop(sig,frame):
 (Path(os.environ["FIXTURE"])/"player-stopped").write_text("stopped")
 raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
data=sys.stdin.buffer.read()
with (Path(os.environ["FIXTURE"])/"played").open("ab") as f: f.write(data+b"\\n")
time.sleep(float(os.environ.get("PLAY_DELAY","0.35")))
'''

class VoiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.bin=Path(cls.tmp.name)/'voice'
        subprocess.run(['gcc','-O1','-g','-fsanitize=undefined','-Werror=implicit-function-declaration',str(ROOT/'model-voice.c'),str(ROOT.parent/'common/control_asr.c'),str(ROOT.parent/'doubao/src/json.c'),str(ROOT.parent/'doubao/src/text_match.c'),'-lopus','-o',str(cls.bin)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        self.tmpcase=tempfile.TemporaryDirectory();self.p=Path(self.tmpcase.name);self.addCleanup(self.tmpcase.cleanup)
        for name,source in [('request',REQUEST),('audio',AUDIO),('player',PLAYER)]:
            f=self.p/name;f.write_text(source);f.chmod(0o700)
        self.speaker=SpeakerASR().__enter__();self.addCleanup(self.speaker.__exit__,None,None,None)
    def start(self,**extra):
        self.logs=[];self.pcm_frame=b'\0'*640
        self.proc=subprocess.Popen([str(self.bin),str(self.p/'request'),str(self.p/'audio'),str(self.p/'player'),'fixture-model','召唤测试'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env={**os.environ,'FIXTURE':str(self.p),'LX06_CONTROL_ASR_SOCKET':self.speaker.path,'MODEL_GREETING':'','MODEL_IDLE_SECONDS':'10',**extra})
        def feed():
            try:
                while self.proc.poll() is None:self.proc.stdin.write(self.pcm_frame);self.proc.stdin.flush();time.sleep(.02)
            except (BrokenPipeError,ValueError):pass
        def log():
            for line in self.proc.stderr:self.logs.append(line.decode())
        self.feeder=threading.Thread(target=feed,daemon=True);self.reader=threading.Thread(target=log,daemon=True)
        self.feeder.start();self.reader.start();self.addCleanup(self.close)
        self.wait(lambda:any('ready' in x for x in self.logs))
    def close(self):
        self.speaker.stopping=True
        if self.proc.poll() is None:self.proc.terminate()
        self.proc.wait(timeout=3);self.feeder.join(1);self.reader.join(1)
        # Termination can race a buffered final PCM write; cleanup of a closed
        # child pipe must not hide a successful event-loop assertion.
        with contextlib.suppress(BrokenPipeError):self.proc.stdin.close()
        self.proc.stdout.close();self.proc.stderr.close()
        self.assertFalse(any('runtime error:' in x for x in self.logs),self.logs)
    def wait(self,predicate,timeout=4):
        end=time.monotonic()+timeout
        while not predicate():
            if time.monotonic()>end:self.fail('timed out: '+''.join(self.logs))
            time.sleep(.01)
    def requests(self):
        f=self.p/'requests';return [json.loads(x) for x in f.read_text().splitlines()] if f.exists() else []
    def count(self,event):return sum(x.strip()=='model-voice: '+event for x in self.logs)
    def test_continuous_context_and_goodbye_mixture(self):
        self.start()
        self.assertTrue(any('question endpoint 1600 ms' in x for x in self.logs))
        self.speaker.emit('我叫小明');self.wait(lambda:self.count('listening')==1)
        self.speaker.emit('我叫什么');self.wait(lambda:self.count('listening')==2)
        calls=self.requests();self.assertEqual(len(calls),2)
        self.assertEqual(calls[1]['input'],[{'role':'user','content':'我叫小明'},{'role':'assistant','content':'这是模型的完整回复。'},{'role':'user','content':'我叫什么'}])
        self.speaker.emit('结束对话，拜拜了哦，结束对话');self.proc.wait(timeout=3)
        self.assertEqual(self.proc.returncode,0);self.assertEqual(self.count('goodbye'),1)
    def test_typed_provider_error_spoken_once_without_history_or_reconnect(self):
        script=self.p/'request'
        script.write_text(REQUEST.replace('answer=os.environ.get(','''if not (p/"failed-once").exists():
 (p/"failed-once").write_text("1")
 print(json.dumps({"error":"DeepSeek 账号已被禁言，请到官网查看原因。"},ensure_ascii=False),flush=True)
 sys.exit(1)
answer=os.environ.get('''))
        self.start(PLAY_DELAY='0.1')
        self.speaker.emit('介绍故宫');self.wait(lambda:self.count('listening')==1)
        self.assertEqual(self.count('provider error notice'),1);self.assertEqual(self.count('answer'),0)
        played=(self.p/'played').read_text();self.assertIn('禁言',played)
        self.speaker.emit('一加一等于几');self.wait(lambda:self.count('listening')==2)
        calls=self.requests();self.assertEqual(len(calls),2)
        self.assertEqual(calls[1]['input'],[dict(role='user',content='一加一等于几')])
        self.assertEqual(self.count('answer'),1);self.assertIsNone(self.proc.poll())
    def test_overlong_provider_error_is_not_spoken_as_truncated_text(self):
        script=self.p/'request'
        script.write_text(REQUEST.replace('answer=os.environ.get(','''print(json.dumps({"error":"禁"*100},ensure_ascii=False),flush=True)
sys.exit(1)
answer=os.environ.get('''))
        self.start();self.speaker.emit('测试错误')
        self.wait(lambda:any('stream failed' in x for x in self.logs))
        self.assertEqual(self.count('provider error notice'),0)
        self.assertFalse((self.p/'played').exists());self.assertIsNone(self.proc.poll())
    def test_keyword_repetition_does_not_disconnect_or_lose_followup(self):
        self.start(PLAY_DELAY='3')
        self.speaker.emit('讲个故事');self.wait(lambda:self.count('speaking')==1)
        self.speaker.emit('旁边的电视正在讲话');time.sleep(.3)
        self.assertEqual(self.count('keyword interruption'),0);self.assertEqual(len(self.requests()),1)
        self.speaker.emit('等一下，你说得不对，等等，等一下');self.wait(lambda:self.count('keyword interruption')==1)
        self.speaker.emit('停一下，等等，等一下');time.sleep(.3)
        self.assertEqual(self.count('keyword interruption'),1);self.assertEqual(len(self.requests()),1)
        self.speaker.emit('一加一等于几');self.wait(lambda:len(self.requests())==2)
        self.assertIsNone(self.proc.poll())
    def test_keyword_partial_stops_owned_player_and_keeps_final_followup(self):
        self.start(PLAY_DELAY='3',TAIL_DELAY='4',MODEL_TTS_YIELD='1')
        self.speaker.emit('讲个长故事');self.wait(lambda:self.count('speaking')==1)
        self.pcm_frame=b'\x00\x10'*320  # playback/background is not silence
        dialog=self.speaker.emit('等一下',False)
        self.wait(lambda:(self.p/'player-stopped').exists(),timeout=.25)
        self.assertEqual(self.count('keyword interruption'),1)
        self.speaker.emit('等一下',dialog=dialog)
        self.speaker.emit('一加一等于几只回答数字')
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'一加一等于几只回答数字')
        self.assertIsNone(self.proc.poll())
    def test_prefetch_does_not_cancel_active_control_recognition(self):
        self.start(MODEL_TTS_YIELD='1',REQUEST_DELAY='.7',PLAY_DELAY='1',
                   ANSWER='这是测试完整连续回复的长段落。'*30)
        self.speaker.emit('先讲故事');self.wait(lambda:self.count('speaking')==1)
        before=self.speaker.cancel_packets
        dialog=self.speaker.emit('保留正在识别的问题',False)
        self.wait(lambda:self.count('speaking')>=2)
        self.assertEqual(self.speaker.cancel_packets,before)
        self.speaker.emit('等一下',dialog=dialog)
        self.wait(lambda:self.count('keyword interruption')==1)
        self.assertIsNone(self.proc.poll())
    def test_playback_prefix_does_not_hide_new_user_control(self):
        self.start(PLAY_DELAY='3',ANSWER='中国四大发明')
        self.speaker.emit('讲个长故事');self.wait(lambda:self.count('speaking')==1)
        self.wait(lambda:(self.p/'played').exists())
        self.speaker.emit('中国四大发明等一下')
        self.wait(lambda:self.count('keyword interruption')==1)
        self.wait(lambda:(self.p/'player-stopped').exists(),timeout=.25)
        time.sleep(.2);self.assertEqual(len(self.requests()),1)
    def test_mixed_controls_and_question_same_final(self):
        self.start(REQUEST_DELAY='3')
        self.speaker.emit('第一个问题');self.wait(lambda:len(self.requests())==1)
        self.speaker.emit('等一下，等等，你说得不对，现在几点');self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'现在几点')
        self.assertEqual(self.count('keyword interruption'),1)
    def test_human_partial_cancels_but_only_final_becomes_question(self):
        self.start(PLAY_DELAY='3',MODEL_BARGE_ANY='1')
        self.speaker.emit('先讲故事');self.wait(lambda:self.count('speaking')==1)
        d=self.speaker.emit('我要问',False);self.wait(lambda:self.count('human interruption')==1)
        self.assertEqual(len(self.requests()),1)
        self.speaker.emit('我要问天气',dialog=d);self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'我要问天气')
    def test_echo_controls_do_not_end_or_interrupt(self):
        self.start(PLAY_DELAY='3',ANSWER='需要退出时请说结束对话，需要暂停请说等一下。')
        # First-phrase streaming splits this explanation at the comma. Echo
        # only words that have actually reached playback, including chunk two.
        self.speaker.emit('怎么操作');self.wait(lambda:self.count('speaking')==2)
        self.speaker.emit('需要退出时请说结束对话');time.sleep(.3)
        self.assertIsNone(self.proc.poll());self.assertEqual(self.count('goodbye'),0)
        self.speaker.emit('等一下');time.sleep(.3)
        self.assertEqual(self.count('keyword interruption'),0)
    def test_long_answer_sentences_are_bounded_utf8(self):
        answer='这一段是一个中文长回复用于验证分段没有截断汉字。'*35
        self.start(ANSWER=answer,PLAY_DELAY='0.01')
        self.speaker.emit('长回复测试');self.wait(lambda:self.count('listening')==1,timeout=8)
        chunks=(self.p/'played').read_text().splitlines()
        self.assertEqual(''.join(chunks),answer)
        self.assertTrue(all(len(x.encode())<=240 for x in chunks))
    def test_delayed_tts_echo_final_does_not_start_another_request(self):
        self.start()
        self.speaker.emit('一个问题');self.wait(lambda:self.count('listening')==1)
        time.sleep(1.7)  # Final after the old 800 ms tail guard had expired.
        self.speaker.emit('这是模型的完整回复。');time.sleep(.3)
        self.assertEqual(len(self.requests()),1)
        self.speaker.emit('新的问题');self.wait(lambda:len(self.requests())==2)
    def test_single_character_answer_echo_is_not_a_question(self):
        self.start(ANSWER='二。')
        self.speaker.emit('一加一等于几');self.wait(lambda:self.count('listening')==1)
        self.speaker.emit('二');time.sleep(.3)
        self.assertEqual(len(self.requests()),1)
    def test_ascii_digit_answer_chinese_asr_echo_is_not_a_question(self):
        self.start(ANSWER='2',PLAY_DELAY='0.05')
        self.speaker.emit('一加一等于几');self.wait(lambda:self.count('listening')==1)
        self.speaker.emit('二。');time.sleep(.3)
        self.assertEqual(len(self.requests()),1)
        # A different number is a valid short user turn, not an echo.
        self.speaker.emit('三');self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'三')
    def test_ascii_digit_greeting_chinese_echo_does_not_barge_or_hide_question(self):
        self.start(MODEL_GREETING='2',MODEL_BARGE_ANY='1',PLAY_DELAY='1')
        self.wait(lambda:self.count('speaking')==1)
        dialog=self.speaker.emit('二',False);time.sleep(.2)
        self.assertEqual(self.count('human interruption'),0)
        self.speaker.emit('二',dialog=dialog);time.sleep(.2)
        self.assertEqual(len(self.requests()),0)
        self.wait(lambda:self.count('listening')==1)
        # Normalize a pure digit only; do not consume a real question that
        # starts with its spoken representation.
        self.speaker.emit('二加三等于几');self.wait(lambda:len(self.requests())==1)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'二加三等于几')
    def test_greeting_tail_final_is_echo_only_for_the_greeting_utterance(self):
        self.start(MODEL_GREETING='我在呢, 请说!',PLAY_DELAY='.7')
        self.wait(lambda:self.count('speaking')==1)
        dialog=self.speaker.emit(' 请说！ ',False);time.sleep(.15)
        self.wait(lambda:self.count('listening')==1)
        self.speaker.emit(' 请说！ ',dialog=dialog);time.sleep(.3)
        self.assertEqual(len(self.requests()),0)
        # Same short phrase from a new idle utterance remains a user question.
        self.speaker.emit('请说');self.wait(lambda:len(self.requests())==1)
        self.assertEqual(self.requests()[0]['input'][-1]['content'],'请说')
    def test_greeting_tail_partial_does_not_human_barge(self):
        self.start(MODEL_GREETING='我在呢，请说。',PLAY_DELAY='1',MODEL_BARGE_ANY='1')
        self.wait(lambda:self.count('speaking')==1)
        dialog=self.speaker.emit('请说',False);time.sleep(.2)
        self.assertEqual(self.count('human interruption'),0)
        self.speaker.emit('请说',dialog=dialog);time.sleep(.2)
        self.assertEqual(len(self.requests()),0)
        self.wait(lambda:self.count('listening')==1)
    def test_greeting_tail_mixed_with_question_is_not_swallowed(self):
        self.start(MODEL_GREETING='我在呢，请说。',PLAY_DELAY='2',MODEL_BARGE_ANY='1')
        self.wait(lambda:self.count('speaking')==1)
        dialog=self.speaker.emit('请说',False);time.sleep(.15)
        self.assertEqual(self.count('human interruption'),0)
        self.speaker.emit('请说，今天的天气怎么样',False,dialog=dialog)
        self.wait(lambda:self.count('human interruption')==1)
        self.speaker.emit('请说，今天的天气怎么样',dialog=dialog)
        self.wait(lambda:len(self.requests())==1)
        self.assertEqual(self.requests()[0]['input'][-1]['content'],'请说，今天的天气怎么样')
    def assert_greeting_fragment_not_echo(self,fragment):
        self.start(MODEL_GREETING='我在呢，请说。',PLAY_DELAY='.7')
        self.wait(lambda:self.count('speaking')==1)
        dialog=self.speaker.emit(fragment,False);time.sleep(.15)
        self.wait(lambda:self.count('listening')==1)
        self.speaker.emit(fragment,dialog=dialog);self.wait(lambda:len(self.requests())==1)
        self.assertEqual(self.requests()[0]['input'][-1]['content'],fragment)
    def test_greeting_tail_requires_a_complete_clause(self):
        self.assert_greeting_fragment_not_echo('在呢')
    def test_greeting_tail_does_not_expand_single_character_matching(self):
        self.assert_greeting_fragment_not_echo('说')
    def test_partial_started_before_greeting_playback_is_preserved(self):
        self.start(MODEL_GREETING='我在呢，请说。',SYNTH_DELAY='.7',PLAY_DELAY='.1')
        dialog=self.speaker.emit('请说',False);time.sleep(.15)
        self.assertEqual(self.count('speaking'),0)
        self.wait(lambda:self.count('listening')==1)
        self.speaker.emit('请说',dialog=dialog);self.wait(lambda:len(self.requests())==1)
        self.assertEqual(self.requests()[0]['input'][-1]['content'],'请说')
    def test_wake_phrase_is_consumed_instead_of_sent_as_a_question(self):
        self.start()
        self.speaker.emit('召唤测试');time.sleep(.3)
        self.assertEqual(len(self.requests()),0);self.assertEqual(self.count('wake phrase consumed'),1)
        self.speaker.emit('召唤测试，我有一个问题');self.wait(lambda:len(self.requests())==1)
        self.assertEqual(self.requests()[0]['input'][-1]['content'],'召唤测试，我有一个问题')
    def test_playback_starts_before_generation_finishes(self):
        self.start(TAIL_DELAY='2',PLAY_DELAY='0.05')
        self.speaker.emit('流式测试');self.wait(lambda:(self.p/'played').exists())
        self.assertEqual(self.count('answer'),0)
        self.wait(lambda:self.count('listening')==1)
    def test_first_phrase_short_but_following_chunks_remain_large(self):
        self.start(ANSWER='这是没有句号的很长正文需要尽早播放同时后续保持大的分段避免频繁调用官方语音服务'*12,PLAY_DELAY='0.01')
        self.speaker.emit('首段速度测试');self.wait(lambda:self.count('listening')==1,timeout=8)
        chunks=(self.p/'synthesized').read_text().splitlines()
        self.assertLessEqual(len(chunks[0].encode()),72)
        self.assertTrue(any(len(x.encode())>72 for x in chunks[1:]))
    def test_tiny_done_reply_plays_without_waiting_for_cleanup_exit(self):
        (self.p/'request').write_text("#!/bin/sh\nprintf '%s\\n' '{\"delta\":\"10\"}' '{\"done\":true}'\nsleep 2\n")
        self.start(PLAY_DELAY='0.05')
        self.speaker.emit('七加三');self.wait(lambda:(self.p/'played').exists(),timeout=1.5)
        self.assertEqual((self.p/'played').read_text().strip(),'10')
        self.assertEqual(self.count('answer'),0)
        self.wait(lambda:self.count('listening')==1)
    def assert_followup_during_cleanup(self,barge_any):
        self.start(ANSWER='10',PLAY_DELAY='0.05',CLEANUP_DELAY='1',MODEL_BARGE_ANY=barge_any)
        self.speaker.emit('第一个问题');self.wait(lambda:(self.p/'played').exists())
        time.sleep(.15)  # Audio has drained, but request cleanup still owns net.
        dialog=self.speaker.emit('第二个',False)
        self.speaker.emit('第二个问题',dialog=dialog)
        self.wait(lambda:self.count('follow-up queued during request cleanup')==1)
        self.assertEqual(len(self.requests()),1);self.assertEqual(self.count('answer'),0)
        self.assertEqual(self.count('human interruption'),0)
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[1]['input'],[
            {'role':'user','content':'第一个问题'},
            {'role':'assistant','content':'10'},
            {'role':'user','content':'第二个问题'}])
    def test_word_mode_followup_during_cleanup_waits_for_confirmed_context(self):
        self.assert_followup_during_cleanup('0')
    def test_human_mode_followup_during_cleanup_does_not_cancel_confirmed_context(self):
        self.assert_followup_during_cleanup('1')
    def test_followup_after_failed_cleanup_uses_only_confirmed_history(self):
        self.start(ANSWER='10',PLAY_DELAY='0.05',CLEANUP_DELAY='1',REQUEST_EXIT='1')
        self.speaker.emit('失败的问题');self.wait(lambda:(self.p/'played').exists())
        time.sleep(.15)
        self.speaker.emit('保留这个新问题')
        self.wait(lambda:self.count('follow-up queued during request cleanup')==1)
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.count('answer'),0)
        self.assertEqual(self.requests()[1]['input'],[{'role':'user','content':'保留这个新问题'}])
    def test_multiple_cleanup_finals_merge_in_one_bounded_followup(self):
        self.start(ANSWER='10',PLAY_DELAY='0.05',CLEANUP_DELAY='1')
        self.speaker.emit('先回答这个');self.wait(lambda:(self.p/'played').exists())
        time.sleep(.15)
        self.speaker.emit('第二个问题');self.wait(lambda:self.count('follow-up queued during request cleanup')==1)
        self.speaker.emit('补充条件');self.wait(lambda:self.count('follow-up queued during request cleanup')==2)
        self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[1]['input'][-1]['content'],'第二个问题 补充条件')
    def test_cleanup_interrupt_replaces_queued_followup(self):
        self.start(ANSWER='10',PLAY_DELAY='0.05',CLEANUP_DELAY='2')
        self.speaker.emit('第一个问题');self.wait(lambda:(self.p/'played').exists())
        time.sleep(.15)
        self.speaker.emit('被替换的问题');self.wait(lambda:self.count('follow-up queued during request cleanup')==1)
        self.speaker.emit('等一下，改问这个');self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.count('keyword interruption'),1)
        self.assertEqual(self.requests()[1]['input'],[{'role':'user','content':'改问这个'}])
    def test_cleanup_goodbye_cancels_queued_followup(self):
        self.start(ANSWER='10',PLAY_DELAY='0.05',CLEANUP_DELAY='2')
        self.speaker.emit('第一个问题');self.wait(lambda:(self.p/'played').exists())
        time.sleep(.15)
        self.speaker.emit('不能再发出的问题');self.wait(lambda:self.count('follow-up queued during request cleanup')==1)
        self.speaker.emit('结束对话，拜拜了哦');self.proc.wait(timeout=3)
        self.assertEqual(self.proc.returncode,0)
        self.assertEqual(len(self.requests()),1);self.assertEqual(self.count('goodbye'),1)
    def test_short_first_delta_starts_after_small_batch_delay(self):
        (self.p/'request').write_text("#!/bin/sh\nprintf '%s\\n' '{\"delta\":\"这是先收到的首段内容\"}'\nsleep 2\nprintf '%s\\n' '{\"done\":true}'\n")
        self.start(PLAY_DELAY='0.05',LX06_QUESTION_STABLE_MS='1000')
        self.assertTrue(any('question endpoint 1000 ms' in x for x in self.logs))
        self.speaker.emit('一边生成一边播');self.wait(lambda:(self.p/'played').exists(),timeout=1.5)
        self.assertEqual(self.count('answer'),0)
        self.wait(lambda:self.count('listening')==1)
    def test_continuous_flow_merges_tiny_sentences_and_still_streams_before_done(self):
        first='北京故宫。';rest='其历史与建筑文化值得仔细了解。'*12
        source='import json,time\nprint(json.dumps({"delta":'+repr(first)+'},ensure_ascii=False),flush=True)\ntime.sleep(.15)\nprint(json.dumps({"delta":'+repr(rest)+'},ensure_ascii=False),flush=True)\ntime.sleep(2)\nprint(json.dumps({"done":True}),flush=True)\n'
        (self.p/'producer.py').write_text(source)
        (self.p/'request').write_text('#!/bin/sh\nexec python3 "$FIXTURE/producer.py"\n')
        self.start(MODEL_SPEECH_FLOW='连贯优先',PLAY_DELAY='.25')
        self.speaker.emit('介绍故宫');self.wait(lambda:(self.p/'played').exists(),timeout=1.5)
        self.assertEqual(self.count('answer'),0)
        pieces=(self.p/'synthesized').read_text().splitlines()
        self.assertGreaterEqual(len(pieces[0].encode()),216)
        self.assertNotEqual(pieces[0],first)
        self.wait(lambda:self.count('listening')==1)
        played=(self.p/'played').read_text().splitlines()
        self.assertEqual(''.join(played),first+rest)
        self.assertTrue(all(len(x.encode())<=240 for x in played))
    def test_continuous_sparse_stream_starts_within_bounded_wait(self):
        text='这是一个暂时不再更新的短句。'
        source='import json,time\nprint(json.dumps({"delta":'+repr(text)+'},ensure_ascii=False),flush=True)\ntime.sleep(4)\nprint(json.dumps({"done":True}),flush=True)\n'
        (self.p/'producer.py').write_text(source)
        (self.p/'request').write_text('#!/bin/sh\nexec python3 "$FIXTURE/producer.py"\n')
        self.start(MODEL_SPEECH_FLOW='连贯优先',PLAY_DELAY='.05')
        began=time.monotonic();self.speaker.emit('简短回答')
        self.wait(lambda:(self.p/'played').exists(),timeout=2.8)
        self.assertLess(time.monotonic()-began,2.8)
        self.assertEqual(self.count('answer'),0)
        self.assertEqual((self.p/'played').read_text().strip(),text)
    def test_continuous_short_done_answer_flushes_without_batch_delay(self):
        self.start(MODEL_SPEECH_FLOW='连贯优先',ANSWER='二。',PLAY_DELAY='.05')
        began=time.monotonic();self.speaker.emit('一加一等于几')
        self.wait(lambda:(self.p/'played').exists(),timeout=.8)
        self.assertLess(time.monotonic()-began,.8)
        self.assertEqual((self.p/'played').read_text().strip(),'二。')
    def test_continuous_flow_keyword_stops_pending_audio_and_preserves_followup(self):
        self.start(MODEL_SPEECH_FLOW='连贯优先',MODEL_TTS_YIELD='1',PLAY_DELAY='3',ANSWER='完整的长回复不能影响识别与打断。'*30)
        self.speaker.emit('介绍故宫');self.wait(lambda:(self.p/'played').exists())
        self.speaker.emit('等一下，你说得不对，等一下')
        self.wait(lambda:self.count('keyword interruption')==1)
        self.wait(lambda:(self.p/'player-stopped').exists(),timeout=.25)
        self.speaker.emit('一加一等于几只回答数字');self.wait(lambda:len(self.requests())==2)
        self.assertEqual(self.requests()[-1]['input'][-1]['content'],'一加一等于几只回答数字')
        self.assertIsNone(self.proc.poll())
    def test_tts_yield_is_disabled_by_default(self):
        self.start(REQUEST_DELAY='.7',SYNTH_DELAY='.2')
        self.speaker.emit('默认路径');self.wait(lambda:self.count('listening')==1)
        self.assertEqual(self.count('TTS priority before synthesis'),0)
        self.assertEqual(self.speaker.end_packets,0)
    def test_tts_priority_preserves_all_reply_chunks(self):
        self.start(MODEL_TTS_YIELD='1',REQUEST_DELAY='.7',SYNTH_DELAY='.2',PLAY_DELAY='.05',ANSWER='这是第一句。第二句也需要播完。')
        self.speaker.emit('候选静音路径');self.wait(lambda:self.count('listening')==1)
        self.assertGreaterEqual(self.count('TTS priority before synthesis'),1)
        self.assertGreaterEqual(self.speaker.cancel_packets,1)
        self.assertEqual((self.p/'played').read_text().replace('\n',''),'这是第一句。第二句也需要播完。')
    def test_tts_priority_replays_weak_pcm_activity(self):
        self.start(MODEL_TTS_YIELD='1',REQUEST_DELAY='.7')
        # Below the peak threshold, but above the conservative mean level.
        self.pcm_frame=b'\x10\x00'*320
        self.speaker.emit('微弱人声');self.wait(lambda:self.count('listening')==1)
        self.assertGreaterEqual(self.count('TTS priority before synthesis'),1)
        self.assertGreaterEqual(len(self.speaker.start_packets),2)
    def test_tts_priority_does_not_cancel_existing_user_partial(self):
        self.start(MODEL_TTS_YIELD='1',REQUEST_DELAY='.7',SYNTH_DELAY='2')
        self.speaker.emit('先回答这个')
        dialog=self.speaker.emit('保留正在识别的问题',False)
        self.wait(lambda:(self.p/'synthesized').exists())
        self.assertEqual(self.count('TTS priority before synthesis'),0)
        self.assertEqual(self.speaker.cancel_packets,0)
        self.speaker.emit('等一下',dialog=dialog)
        self.wait(lambda:self.count('keyword interruption')==1)
        self.assertEqual(len(self.requests()),1)
    def test_tts_yield_new_pcm_resumes_before_controls_and_goodbye(self):
        self.start(MODEL_TTS_YIELD='1',REQUEST_DELAY='.7',SYNTH_DELAY='2')
        self.speaker.emit('先回答这个');self.wait(lambda:self.count('TTS priority before synthesis')==1)
        old=self.speaker.start_packets[-1][1]
        # A late result from the canceled dialog cannot cancel this response.
        self.speaker.emit('',dialog=old)
        self.pcm_frame=b'\x10\x00'*320
        self.wait(lambda:self.count('TTS yield resumed')>=1)
        self.wait(lambda:self.speaker.dialog and self.speaker.dialog!=old)
        self.speaker.emit('等一下，等等');self.wait(lambda:self.count('keyword interruption')==1)
        self.assertEqual(len(self.requests()),1)
        self.speaker.emit('结束对话，拜拜了哦');self.proc.wait(timeout=3)
        self.assertEqual(self.proc.returncode,0);self.assertEqual(self.count('goodbye'),1)
    def test_tts_priority_ignores_canceled_results_and_replayed_controls_work(self):
        self.start(MODEL_TTS_YIELD='1',REQUEST_DELAY='.7',SYNTH_DELAY='2')
        self.speaker.emit('先回答这个');self.wait(lambda:self.count('TTS priority before synthesis')==1)
        dialog=self.speaker.start_packets[-1][1]
        self.speaker.emit('等一下',False,dialog=dialog);time.sleep(.1)
        self.speaker.emit('等一下',dialog=dialog)
        time.sleep(.1);self.assertEqual(self.count('keyword interruption'),0)
        self.wait(lambda:self.speaker.dialog and self.speaker.dialog!=dialog)
        self.speaker.emit('等一下',dialog=self.speaker.dialog)
        self.wait(lambda:self.count('keyword interruption')==1)
        self.assertEqual(len(self.requests()),1)
    def test_interruption_cancels_stream_and_pending_sentences(self):
        self.start(TAIL_DELAY='3',PLAY_DELAY='3',ANSWER='这是第一句。'*60)
        self.speaker.emit('流式故事');self.wait(lambda:self.count('speaking')==1)
        self.speaker.emit('等一下，等等');self.wait(lambda:self.count('keyword interruption')==1)
        time.sleep(.5)
        self.assertEqual(self.count('speaking'),1);self.assertEqual(self.count('answer'),0)
    def test_missing_done_discards_pending_audio_and_context(self):
        (self.p/'request').write_text("#!/bin/sh\nprintf '%s\\n' '{\"delta\":\"不完整回复\"}'\n")
        self.start();self.speaker.emit('损坏的响应')
        self.wait(lambda:any('stream failed' in x for x in self.logs))
        self.assertFalse((self.p/'played').exists());self.assertEqual(self.count('answer'),0)
    def test_one_sentence_prefetched_while_current_sentence_plays(self):
        self.start(ANSWER='这是用于检验并行预取的长句子。'*30,PLAY_DELAY='2')
        self.speaker.emit('预取测试');self.wait(lambda:self.count('speaking')==1)
        self.wait(lambda:(self.p/'synthesized').exists() and len((self.p/'synthesized').read_text().splitlines())==2)
        time.sleep(.2)
        self.assertEqual(len((self.p/'synthesized').read_text().splitlines()),2)
        self.assertEqual(self.count('speaking'),1)
    def test_fragmented_utf8_ndjson_is_reassembled(self):
        data=(json.dumps({'delta':'连续中文。'},ensure_ascii=False)+'\n'+json.dumps({'done':True})+'\n').encode()
        (self.p/'producer.py').write_text('import os,time\ndata='+repr(data)+'\nfor b in data: os.write(1,bytes([b])); time.sleep(.001)\n')
        (self.p/'request').write_text('#!/bin/sh\nexec python3 "$FIXTURE/producer.py"\n')
        self.start();self.speaker.emit('分片测试');self.wait(lambda:self.count('listening')==1)
        self.assertEqual((self.p/'played').read_text().strip(),'连续中文。')
    def test_termination_kills_request_descendants(self):
        source=self.p/'request'
        source.write_text('#!/bin/sh\nsleep 60 &\necho $! > "$FIXTURE/child"\nwait\n')
        self.start();self.speaker.emit('问题');self.wait(lambda:(self.p/'child').exists())
        pid=int((self.p/'child').read_text());self.proc.terminate();self.proc.wait(timeout=3)
        # A reparented zombie is already stopped and cannot make late requests.
        stat=Path(f'/proc/{pid}/stat')
        self.wait(lambda:not stat.exists() or stat.read_text().split()[2]=='Z')
        self.assertEqual(self.count('answer'),0)

if __name__=='__main__':unittest.main()

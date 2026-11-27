"""Actual shell/embedded helper tests; no account or network required."""
import json
import re
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from test_deepseek_login import DeepSeekLoginTest, FAKE_CURL
from build_deepseek_login import build

ROOT=Path(__file__).resolve().parent
def event(value,name=''):
    return (('event: '+name+'\n') if name else '')+'data: '+json.dumps(value,ensure_ascii=False)+'\n\n'
def snapshot(text='你好',kind='RESPONSE'):
    return dict(v=dict(response=dict(status='WIP',fragments=[dict(type=kind,content=text)])))
SAMPLE=event(snapshot('推理秘密','THINK'))+event(dict(p='response/fragments/-1/content',o='APPEND',v='隐藏内容'))+event(dict(p='response/fragments',o='APPEND',v=[dict(type='RESPONSE',content='你好')]))+event(dict(p='response/fragments/-1/content',o='APPEND',v='，'))+event(dict(v='这是流式回复。'))+event(dict(p='response/status',o='SET',v='FINISHED'))

class StreamTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.tool=Path(cls.tmp.name)/'stream'
        subprocess.run(['gcc','-O2',str(ROOT/'deepseek-stream.c'),'-o',str(cls.tool)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def parse(self,text,mode='--stream'):
        return subprocess.run([str(self.tool),mode],input=text,text=True,capture_output=True)
    def test_reasoning_filter_incremental_and_metadata(self):
        r=self.parse(SAMPLE)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual([json.loads(x)['delta'] for x in r.stdout.splitlines()],['你好','，','这是流式回复。'])
        r=self.parse(SAMPLE,'--preview');self.assertEqual(json.loads(r.stdout),dict(message='你好，这是流式回复。'))
    def test_truncation_error_and_rewrite_never_success(self):
        for text in [SAMPLE[:SAMPLE.rindex('data:')],event(snapshot())+event(dict(p='response/status',v='FAILED')),event(snapshot())+event(dict(p='response/fragments/0/content',o='SET',v='替换正文')),event(dict(code=40301)),event(snapshot(),'error')]:
            self.assertNotEqual(self.parse(text).returncode,0)
    def test_snapshot_replay_no_duplicate_and_batch_finished(self):
        text=event(snapshot())+event(snapshot('你好世界'))+event(dict(p='response',o='BATCH',v=[dict(p='quasi_status',v='FINISHED'),dict(p='status',v='FINISHED')]))
        r=self.parse(text);self.assertEqual(r.returncode,0)
        self.assertEqual(''.join(json.loads(x)['delta'] for x in r.stdout.splitlines()),'你好世界')
    def test_live_flush_before_response_finished(self):
        import select
        p=subprocess.Popen([str(self.tool),'--stream'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
        try:
            p.stdin.write(event(snapshot()));p.stdin.flush()
            self.assertTrue(select.select([p.stdout],[],[],1)[0])
            self.assertEqual(json.loads(p.stdout.readline()),dict(delta='你好'))
            p.stdin.write(event(dict(p='response/status',v='FINISHED')));p.stdin.close();self.assertEqual(p.wait(timeout=2),0)
        finally:
            if p.poll() is None:p.kill();p.wait()
            p.stdout.close()
    def test_context_prompt_and_bounds(self):
        r=self.parse(json.dumps(dict(input=[dict(role='user',content='我叫路遥'),dict(role='assistant',content='你好'),dict(role='user',content='我叫什么？')]),ensure_ascii=False),'--prompt')
        self.assertEqual(r.returncode,0);self.assertIn('我叫路遥',r.stdout);self.assertIn('我叫什么？',r.stdout)
        self.assertNotEqual(self.parse(json.dumps(dict(input=[dict(role='system',content='bad')])),'--prompt').returncode,0)
    def test_json_and_sse_refusals_keep_only_code_and_time(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'error'
            body=dict(code=50006,msg='secret-token-and-phone',data=dict(mute_until=1793691845.13,token='private'))
            nested=dict(code=0,msg='private',data=dict(biz_code=5,biz_msg='private',biz_data=dict(is_muted=1,mute_until=1793691845.13,msg='private')))
            for wire in [json.dumps(body),event(body,'error'),json.dumps(nested)]:
                r=subprocess.run([str(self.tool),'--stream','--error-file',str(path)],input=wire,text=True,capture_output=True)
                self.assertNotEqual(r.returncode,0);self.assertEqual(r.stdout,'')
                diagnostic=json.loads(path.read_text())
                self.assertEqual(diagnostic['code'],0 if wire==json.dumps(nested) else 50006)
                self.assertEqual(diagnostic['data']['mute_until'],1793691845)
                self.assertEqual(diagnostic['data']['biz_code'],5 if wire==json.dumps(nested) else 0)
                self.assertNotIn('secret',path.read_text()+r.stdout+r.stderr);path.unlink()
            # Protocol corruption is not an account ban and must not fabricate one.
            r=subprocess.run([str(self.tool),'--stream','--error-file',str(path)],input='not JSON',text=True,capture_output=True)
            self.assertNotEqual(r.returncode,0);self.assertFalse(path.exists())

class VoiceShellTest(DeepSeekLoginTest):
    @classmethod
    def setUpClass(cls):
        super().setUpClass();cls.streamer=cls.base/'stream'
        subprocess.run(['gcc','-O2',str(ROOT/'deepseek-stream.c'),'-o',str(cls.streamer)],check=True)
    def setUp(self):
        super().setUp()
        old=self.script.read_text()
        original=build(self.p/'voice-source',self.sealer,self.public,voice=True,streamer=self.streamer).read_text()
        for before,after in [('TOOL=/usr/bin/lx06-model-tool','TOOL='+str(self.tool)),('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS='+str(self.settings)),('ROOT=/data/lx06/model-accounts','ROOT='+str(self.p/'accounts')),('[ "$(id -u)" = 0 ]','[ 0 = 0 ]'),('/tmp/lx06-account.',str(self.p/'work.')),('SMS_COOLDOWN=/tmp/lx06-deepseek-sms-cooldown','SMS_COOLDOWN='+str(self.p/'cooldown')),('DS_DIAGNOSTIC=/tmp/lx06-deepseek-last-request.json','DS_DIAGNOSTIC='+str(self.p/'diagnostic.json'))]:original=original.replace(before,after)
        self.script.write_text(original)
        fake=FAKE_CURL.replace("if '--data-binary' in a and url.endswith('/users/create_guest_challenge'):","if '--data-binary' in a and '/users/' not in url:\n body=Path(arg('--data-binary')[1:]).read_text()\nelif '--data-binary' in a and url.endswith('/users/create_guest_challenge'):")
        fake=fake.replace("if url.endswith('/users/create_guest_challenge'):\n result=",'''if url.endswith('/chat_session/create'):
 assert 'Authorization: Bearer ' in headers
 result=dict(code=0,data=dict(biz_code=0,biz_data=dict(chat_session=dict(id='fixture-session')))))
elif url.endswith('/chat/create_pow_challenge'):
 puzzle=dict(fixture['guest']['data']['biz_data']['guest_challenge'],signature='public-signature',target_path='/api/v0/chat/completion')
 result=dict(code=0,data=dict(biz_code=0,biz_data=dict(challenge=puzzle)))
elif url.endswith('/chat/completion'):
 proof=json.loads(base64.b64decode(re.search(r'x-ds-pow-response: ([A-Za-z0-9+/=]+)',headers)[1]))
 assert proof['answer']==42 and proof['signature']=='public-signature'
 assert json.loads(body)['thinking_enabled']==False and json.loads(body)['parent_message_id'] is None
 result=fixture['stream']
elif url.endswith('/chat_session/delete'):
 assert json.loads(body)==dict(chat_session_id='fixture-session')
 result=dict(code=0,data=dict(biz_code=0))
elif url.endswith('/users/create_guest_challenge'):
 result=''')
        # Correct the fixture's nested session envelope, checked by Python itself.
        fake=fake.replace("id='fixture-session')))))","id='fixture-session'))))")
        fake=fake.replace('import json,os,sys,re,base64','import json,os,sys,re,base64,time')
        fake=fake.replace("print(status,end='')",'''print(status,end='')
if '%{time_total}' in arg('--write-out'):
 print('\\n'+('0.123456' if url.endswith('/chat_session/create') else '0.045678'))
''')
        fake=fake.replace("if url.endswith('/chat_session/create'):\n assert",'''if url.endswith(('/chat_session/create','/chat/create_pow_challenge')):
 task='session' if url.endswith('/chat_session/create') else 'pow'
 (root/('pid-'+task)).write_text(str(os.getpid()))
 if fixture.get('setup_barrier'):
  (root/('active-'+task)).write_text('1')
  peer=root/('active-'+('pow' if task=='session' else 'session'))
  deadline=time.monotonic()+1.5
  while not peer.exists() and time.monotonic()<deadline:time.sleep(.01)
  assert peer.exists(),'setup requests were serialized'
 if fixture.get('setup_hold')==task:time.sleep(30)
 if fixture.get('setup_failure')==task:
  Path(arg('--output')).write_text(json.dumps(dict(code=40301,data=dict(biz_code=0))))
  print(200,end='');raise SystemExit(0)
if url.endswith('/chat_session/create'):
 assert''')
        (self.p/'bin/curl').write_text(fake)
        self.fixture['stream']=SAMPLE
    # Inherited login tests still exercise login-only output semantics/markers;
    # voice has intentionally different schema, so specialize just that test.
    def test_real_schema_and_nonresident_no_wake_markers(self):
        r=subprocess.run([str(self.settings),'show',str(self.script)],text=True,capture_output=True)
        self.assertEqual(r.returncode,0,r.stderr)
        j=json.loads(r.stdout);self.assertEqual(j['values']['official_wake'],'深度思考')
        self.assertNotIn('sms_code',j['values']);self.assertNotIn('password',j['values'])
        self.assertIn('# LX06-LOCALWAKEUP: on',self.script.read_text())
    def test_load_reads_local_settings_and_explicit_status_checks_mute(self):
        self.assertEqual(self.action('login').returncode,0)
        self.fixture['current']=dict(code=0,data=dict(biz_code=0,biz_data=dict(user=
            dict(id='user-1',token='rotated-ds-token',chat=dict(is_muted=1,mute_until=time.time()+86400)))))
        r=self.action('load');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        events=[json.loads(x) for x in r.stdout.splitlines()]
        self.assertIn('values',events[0]);self.assertEqual(events[-1]['step'],'configure')
        self.assertNotIn('禁言',r.stdout);self.assertNotIn('rotated-ds-token',r.stdout)
        status=self.action('status')
        self.assertEqual(status.returncode,0,status.stdout+status.stderr)
        self.assertIn('禁言',status.stdout);self.assertNotIn('rotated-ds-token',status.stdout)
    def test_full_streaming_request_context_and_cleanup(self):
        self.assertEqual(self.action('login').returncode,0)
        (self.p/'fixture.json').write_text(json.dumps(self.fixture))
        r=subprocess.run(['sh',str(self.script),'--model-request'],input=json.dumps(dict(values=dict(model='default'),input=[dict(role='user',content='你好')])),text=True,capture_output=True,env=self.env,timeout=8)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        lines=[json.loads(x) for x in r.stdout.splitlines()]
        self.assertEqual(lines[-1],dict(done=True));self.assertEqual(''.join(x.get('delta','') for x in lines),'你好，这是流式回复。')
        calls=[json.loads(x) for x in (self.p/'calls').read_text().splitlines()]
        self.assertTrue(calls[-1]['url'].endswith('/chat_session/delete'))
        self.assertFalse(list(self.p.glob('work.*')))
        timing=[line for line in r.stderr.splitlines() if line.startswith('deepseek-timing:')]
        stages=[];values={}
        for line in timing:
            match=re.fullmatch(r'deepseek-timing: stage=([a-z_]+) elapsed_ms=([0-9]+) total_ms=([0-9]+)',line)
            self.assertIsNotNone(match,line)
            stages.append(match[1]);values[match[1]]=int(match[2])
        self.assertEqual(stages,['auth','stream_helper','prompt_build','session_fetch','pow_fetch','pow_solve','request_build','completion_start','completion_transport','delete_cleanup'])
        self.assertEqual(values['session_fetch'],123);self.assertEqual(values['pow_fetch'],45)
        for secret in ['fake-ds-token','testsalt','fixture-session','public-signature',self.fixture['guest']['data']['biz_data']['guest_challenge']['challenge']]:
            self.assertNotIn(secret,'\n'.join(timing))
        self.assertEqual(self.action('test',dict(question='请介绍自己')).returncode,0)

    def model_request(self,timeout=8):
        (self.p/'fixture.json').write_text(json.dumps(self.fixture))
        return subprocess.run(['sh',str(self.script),'--model-request'],input=json.dumps(dict(values=dict(model='default'),input=[dict(role='user',content='你好')])),text=True,capture_output=True,env=self.env,timeout=timeout)
    def test_muted_completion_reports_reason_preserves_login_and_cleans_session(self):
        self.assertEqual(self.action('login').returncode,0)
        before=self.record.read_bytes()
        self.fixture['stream']=json.dumps(dict(code=0,msg='private-upstream',data=dict(biz_code=5,biz_data=dict(is_muted=1,mute_until=time.time()+86400))))
        r=self.model_request();self.assertNotEqual(r.returncode,0,r.stdout)
        notice=json.loads(r.stdout);self.assertEqual(set(notice), {'error'})
        self.assertIn('禁言',notice['error']);self.assertIn('北京时间',notice['error'])
        self.assertIn('禁言',r.stderr);self.assertNotIn('private-upstream',r.stdout+r.stderr)
        self.assertEqual(self.record.read_bytes(),before)
        self.assertNotIn('done',notice);self.assertNotIn('delta',notice)
        calls=[json.loads(x) for x in (self.p/'calls').read_text().splitlines()]
        self.assertTrue(calls[-1]['url'].endswith('/chat_session/delete'))
        self.assertFalse(any('/users/logout' in c['url'] for c in calls))
        self.assert_setup_children_gone()

    def assert_setup_children_gone(self):
        for file in self.p.glob('pid-*'):
            proc=Path('/proc')/file.read_text()/'stat'
            self.assertFalse(proc.exists(),'setup curl child leaked: '+file.name)
        self.assertFalse(list(self.p.glob('work.*')),'private setup directories leaked')

    def test_setup_network_requests_actually_overlap(self):
        self.assertEqual(self.action('login').returncode,0)
        self.fixture['setup_barrier']=True
        r=self.model_request()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertTrue((self.p/'active-session').exists());self.assertTrue((self.p/'active-pow').exists())
        calls=[json.loads(x) for x in (self.p/'calls').read_text().splitlines()]
        setup=[c for c in calls if c['url'].endswith(('/chat_session/create','/chat/create_pow_challenge'))]
        self.assertEqual(len(setup),2)
        forms=[Path(c['argv'][c['argv'].index('--data-binary')+1][1:]) for c in setup]
        self.assertEqual({p.parent.name for p in forms},{'create-session','create-pow'})
        self.assert_setup_children_gone()

    def test_either_setup_failure_prevents_completion_and_keeps_safe_diagnostic(self):
        self.assertEqual(self.action('login').returncode,0)
        for failed,route in [('session','/api/v0/chat_session/create'),('pow','/api/v0/chat/create_pow_challenge')]:
            self.fixture['setup_failure']=failed
            callsfile=self.p/'calls';callsfile.write_text('')
            r=self.model_request()
            self.assertNotEqual(r.returncode,0)
            calls=[json.loads(x) for x in callsfile.read_text().splitlines()]
            self.assertFalse(any(c['url'].endswith('/chat/completion') for c in calls))
            diagnostic=json.loads((self.p/'diagnostic.json').read_text())
            self.assertEqual(diagnostic['route'],route);self.assertEqual(diagnostic['code'],'40301')
            self.assert_setup_children_gone()

    def test_setup_cancellation_kills_owned_children_and_releases_account_lock(self):
        self.assertEqual(self.action('login').returncode,0)
        self.fixture.update(setup_hold='session',setup_barrier=True)
        (self.p/'fixture.json').write_text(json.dumps(self.fixture))
        p=subprocess.Popen(['sh',str(self.script),'--model-request'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=self.env)
        try:
            p.stdin.write(json.dumps(dict(values=dict(model='default'),input=[dict(role='user',content='你好')])));p.stdin.close()
            deadline=time.monotonic()+3
            while not ((self.p/'active-session').exists() and (self.p/'active-pow').exists()):
                self.assertLess(time.monotonic(),deadline);time.sleep(.01)
            p.terminate();p.wait(timeout=3)
            self.assertNotEqual(p.returncode,0)
            self.assert_setup_children_gone()
            self.assertEqual(self.action('load').returncode,0)
            calls=[json.loads(x) for x in (self.p/'calls').read_text().splitlines()]
            self.assertFalse(any(c['url'].endswith('/chat/completion') for c in calls))
        finally:
            if p.poll() is None:p.kill();p.wait()
            p.stdout.close();p.stderr.close()

if __name__=='__main__':unittest.main()

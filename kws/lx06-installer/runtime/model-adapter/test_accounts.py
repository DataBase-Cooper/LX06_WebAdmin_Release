"""Execute real generated shell adapters against a deterministic fake transport.

No real accounts, provider requests, speaker changes or credentials in fixtures.
The callback/JWT test uses a fresh RSA key and the real C verifier/listener.
"""
import base64,importlib.util,json,os,selectors,socket,subprocess,tempfile,time,unittest,urllib.parse,urllib.request
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import rsa,padding
from cryptography.hazmat.primitives import hashes

ROOT=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('builder',ROOT/'build_accounts.py')
builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
b64=lambda b:base64.urlsafe_b64encode(b).rstrip(b'=').decode()
FAKE_CURL=r'''#!/usr/bin/env python3
import json,os,sys,urllib.parse
from pathlib import Path
a=sys.argv[1:];root=Path(os.environ['FIXTURE']);url=a[-1]
def arg(k):return a[a.index(k)+1] if k in a else ''
config=arg('--config')
if config and Path(config).is_file() and Path(config).read_text().startswith('url = "http://127.0.0.1:'):
 os.execv('/usr/bin/curl',['curl',*a])
fixture=json.loads((root/'fixture.json').read_text());status=200
def arg(k):return a[a.index(k)+1] if k in a else ''
form=arg('--data-binary');body=Path(form[1:]).read_text() if form else ''
with (root/'calls').open('a') as f:f.write(json.dumps({'url':url,'argv':a,'body':body})+'\n')
if url.endswith('/device/code'):
 result={'device_code':'private-device','user_code':'PUBLIC-CODE','verification_uri':'https://auth.x.ai/activate','expires_in':200,'interval':5}
elif url.endswith('/token'):
 count=root/'polls';n=int(count.read_text()) if count.exists() else 0;count.write_text(str(n+1))
 grant=urllib.parse.parse_qs(body).get('grant_type',[''])[0]
 if fixture.get('deny'):result={'error':'access_denied'};status=400
 elif grant.endswith('device_code') and n<len(fixture.get('pending',[])):
  result={'error':fixture['pending'][n]};status=400
 else:result=fixture['token']
elif url.endswith('/jwks.json'):result=fixture['jwks']
elif url.endswith('/models'):result=fixture.get('models',{'data':[{'id':'grok-test'}]})
elif url.endswith('/responses'):
 result='data: '+json.dumps({'type':'response.output_text.delta','delta':'测试完整回答'})+'\n\n'
 if not fixture.get('incomplete'):result+='data: '+json.dumps({'type':'response.completed'})+'\n\n'
 if fixture.get('response_status'):status=fixture['response_status'];result={'error':'private upstream diagnostic fake-access'}
else:raise SystemExit(3)
Path(arg('--output')).write_text(result if isinstance(result,str) else json.dumps(result))
sys.stdout.write(str(status))
'''

class AccountsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build=tempfile.TemporaryDirectory();cls.base=Path(cls.build.name);cls.tool=cls.base/'tool'
        subprocess.run(['gcc','-O2',str(ROOT/'model-tool.c'),'-lcrypto','-o',str(cls.tool)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.p=Path(self.tmp.name)
        builder.build(self.p/'scripts');self.accounts=self.p/'accounts';self.accounts.mkdir()
        settings=self.p/'settings'
        settings.write_text('#!/bin/sh\ncase "$1" in show) printf \'%s\\n\' \'{"values":{}}\';; forget-options) :;; *) exit 1;; esac\n')
        settings.chmod(0o700)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));self.callback_port=listener.getsockname()[1]
        for path in (self.p/'scripts').glob('*.sh'):
            text=path.read_text().replace('TOOL=/usr/bin/lx06-model-tool','TOOL='+str(self.tool)).replace('ROOT=/data/lx06/model-accounts','ROOT='+str(self.accounts))
            text=text.replace('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS='+str(settings))
            text=text.replace('[ "$(id -u)" = 0 ]','[ 0 = 0 ]')
            text=text.replace('LOGIN_PENDING=/tmp/lx06-chatgpt-login','LOGIN_PENDING='+str(self.p/'chatgpt-pending'))
            text=text.replace('1455',str(self.callback_port))
            path.write_text(text)
        bin=self.p/'bin';bin.mkdir();(bin/'curl').write_text(FAKE_CURL);(bin/'curl').chmod(0o755)
        (bin/'sleep').write_text('#!/bin/sh\n/bin/sleep 0.05\n');(bin/'sleep').chmod(0o755)
        self.env={**os.environ,'FIXTURE':str(self.p),'PATH':str(bin)+':'+os.environ['PATH']}
        self.fixture={'token':{'access_token':'fake-access','refresh_token':'fake-refresh','token_type':'Bearer','expires_in':3600,'scope':'openid offline_access grok-cli:access api:access'}}
        self.write_fixture()
    def write_fixture(self): (self.p/'fixture.json').write_text(json.dumps(self.fixture))
    def run_action(self,action,values=None,provider='grok'):
        r=subprocess.run(['sh',str(self.p/'scripts'/(provider+'_account.sh')),'--web-action',action],input=json.dumps({'values':values or {}}).encode(),capture_output=True,env=self.env,timeout=10)
        for secret in [b'fake-access',b'fake-refresh',b'private-device']:self.assertNotIn(secret,r.stdout+r.stderr)
        return r
    def record(self,provider='grok'):return self.accounts/provider/'credentials.json'
    def chatgpt_link(self):
        r=self.run_action('login',provider='chatgpt');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return urllib.parse.parse_qs(urllib.parse.urlsplit(json.loads(r.stdout)['link']).query)
    def chatgpt_identity(self,query,subject='fixture-user',audience='oaiapp_fixture'):
        key=rsa.generate_private_key(public_exponent=65537,key_size=2048);nums=key.public_key().public_numbers();ib=lambda n:n.to_bytes((n.bit_length()+7)//8,'big')
        payload={'iss':'https://auth.openai.com','sub':subject,'aud':audience,'exp':int(time.time())+1000,'nonce':query['nonce'][0]}
        data=(b64(b'{"alg":"RS256","kid":"fixture"}')+'.'+b64(json.dumps(payload).encode())).encode()
        token=data.decode()+'.'+b64(key.sign(data,padding.PKCS1v15(),hashes.SHA256()))
        self.fixture['token'].update(id_token=token,scope='openid offline_access resource.invoke chatgpt.tokens.use.direct')
        self.fixture['jwks']={'keys':[{'kid':'fixture','kty':'RSA','n':b64(ib(nums.n)),'e':b64(ib(nums.e))}]};self.write_fixture()
        return token
    def chatgpt_uri(self,query,**extra):
        values={'state':query['state'][0],'code':'fixture-code/with+encoded=bytes','client_id':'oaiapp_fixture'};values.update(extra)
        return query['redirect_uri'][0]+'?'+urllib.parse.urlencode(values)
    def test_chatgpt_two_step_has_no_forward_and_consumes_one_use_pkce(self):
        query=self.chatgpt_link();pending=self.p/'chatgpt-pending'
        self.assertEqual(pending.stat().st_mode&0o777,0o700)
        self.assertTrue(all(p.stat().st_mode&0o777==0o600 for p in pending.iterdir()))
        with socket.socket() as probe:
            self.assertNotEqual(probe.connect_ex(('127.0.0.1',self.callback_port)),0)
        token=self.chatgpt_identity(query);uri=self.chatgpt_uri(query)
        self.fixture['models']={'models':[
            {'slug':'gpt-fixture-a','display_name':'First','visibility':'list'},
            {'slug':'gpt-fixture-b','display_name':'Second','visibility':'list'}]}
        self.write_fixture()
        r=self.run_action('complete-login',{'auth_callback':uri},provider='chatgpt')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        events=[json.loads(line) for line in r.stdout.splitlines()]
        models=next(event['options']['model'] for event in events if 'options' in event)
        self.assertEqual([item['value'] for item in models],['gpt-fixture-a','gpt-fixture-b'])
        self.assertEqual(events[-1]['step'],'configure')
        self.assertNotIn(uri.encode(),r.stdout+r.stderr);self.assertNotIn(token.encode(),r.stdout+r.stderr)
        self.assertFalse(pending.exists());record=json.loads(self.record('chatgpt').read_text())
        self.assertEqual(record['subject'],'fixture-user');self.assertEqual(record['client_id'],'oaiapp_fixture')
        calls=list(map(json.loads,(self.p/'calls').read_text().splitlines()))
        form=urllib.parse.parse_qs(calls[0]['body'])
        import hashlib
        self.assertEqual(b64(hashlib.sha256(form['code_verifier'][0].encode()).digest()),query['code_challenge'][0])
        self.assertEqual(form['code'][0],'fixture-code/with+encoded=bytes')
        for call in calls:self.assertNotIn('fixture-code',json.dumps(call['argv']))
        saved=self.record('chatgpt').read_bytes()
        replay=self.run_action('complete-login',{'auth_callback':uri},provider='chatgpt')
        self.assertNotEqual(replay.returncode,0);self.assertEqual(self.record('chatgpt').read_bytes(),saved)
    def test_chatgpt_wrong_or_duplicate_state_does_not_consume_valid_pending(self):
        query=self.chatgpt_link();self.chatgpt_identity(query)
        for uri in [self.chatgpt_uri(query,state='wrong-state'),self.chatgpt_uri(query)+'&state='+query['state'][0],self.chatgpt_uri(query)+'&error=%00']:
            r=self.run_action('complete-login',{'auth_callback':uri},provider='chatgpt')
            self.assertNotEqual(r.returncode,0);self.assertTrue((self.p/'chatgpt-pending').is_dir())
            self.assertFalse(self.record('chatgpt').exists());self.assertFalse((self.p/'calls').exists())
        self.assertEqual(self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(query)},provider='chatgpt').returncode,0)
    def test_chatgpt_callback_uri_never_becomes_curl_config_or_shell(self):
        query=self.chatgpt_link();base=self.chatgpt_uri(query)
        for uri in ['https://example.invalid/?code=private',base+'\nurl = "https://example.invalid"',base+'"',base+'\\',base+'#fragment',base+'`touch /tmp/DO_NOT_CREATE`',base+';x=$(id)']:
            r=self.run_action('complete-login',{'auth_callback':uri},provider='chatgpt')
            self.assertNotEqual(r.returncode,0);self.assertTrue((self.p/'chatgpt-pending').is_dir())
            self.assertFalse((self.p/'calls').exists())
            self.assertNotIn(uri.encode(),r.stdout+r.stderr)
    def test_chatgpt_pending_expiry_logout_and_new_login_invalidate_old_state(self):
        first=self.chatgpt_link();second=self.chatgpt_link()
        self.assertNotEqual(first['state'],second['state'])
        r=self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(first)},provider='chatgpt')
        self.assertNotEqual(r.returncode,0)
        pending=self.p/'chatgpt-pending';(pending/'created').write_text('999999999');(pending/'created').chmod(0o600)
        r=self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(second)},provider='chatgpt')
        self.assertNotEqual(r.returncode,0);self.assertIn('过期',r.stdout.decode());self.assertFalse(pending.exists())
        self.chatgpt_link();self.assertEqual(self.run_action('logout',provider='chatgpt').returncode,0)
        self.assertFalse(pending.exists())
    def test_chatgpt_denied_manual_login_preserves_existing_account(self):
        query=self.chatgpt_link();self.chatgpt_identity(query)
        self.assertEqual(self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(query)},provider='chatgpt').returncode,0)
        before=self.record('chatgpt').read_bytes();query=self.chatgpt_link()
        self.assertEqual(query['client_id'],['oaiapp_fixture'])
        r=self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(query,error='access_denied')},provider='chatgpt')
        self.assertNotEqual(r.returncode,0);self.assertEqual(self.record('chatgpt').read_bytes(),before)
        self.assertFalse((self.p/'chatgpt-pending').exists())
    def test_chatgpt_pending_permissions_and_symlinks_fail_without_following(self):
        query=self.chatgpt_link();pending=self.p/'chatgpt-pending';state=pending/'state'
        peer=self.p/'peer';peer.write_text('must-not-read-or-delete');peer.chmod(0o600)
        state.unlink();state.symlink_to(peer)
        r=self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(query)},provider='chatgpt')
        self.assertNotEqual(r.returncode,0);self.assertEqual(peer.read_text(),'must-not-read-or-delete')
        self.assertFalse((self.p/'calls').exists())
        self.assertEqual(self.run_action('logout',provider='chatgpt').returncode,0)
        self.assertEqual(peer.read_text(),'must-not-read-or-delete')
        pending.symlink_to(self.p,target_is_directory=True)
        r=self.run_action('login',provider='chatgpt')
        self.assertNotEqual(r.returncode,0);self.assertEqual(peer.read_text(),'must-not-read-or-delete')
        self.assertFalse(self.record('chatgpt').exists())
    def test_uninstall_removes_only_own_account(self):
        self.assertEqual(self.run_action('login').returncode,0)
        other=self.accounts/'chatgpt';other.mkdir();(other/'credentials.json').write_text('preserve')
        script=self.p/'scripts/grok_account.sh'
        text=script.read_text().replace('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS=/bin/true')
        text=text.replace('/tmp/lx06-',str(self.p/'fixture-'))
        script.write_text(text)
        r=subprocess.run(['sh',str(script),'--uninstall'],capture_output=True,env=self.env)
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertFalse((self.accounts/'grok').exists())
        self.assertEqual((other/'credentials.json').read_text(),'preserve')

    def test_device_pending_slowdown_and_independent_deployment(self):
        self.fixture['pending']=['authorization_pending','slow_down'];self.write_fixture()
        r=self.run_action('login');self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(json.loads(self.record().read_text())['access_token'],'fake-access')
        self.assertEqual(self.record().stat().st_mode&0o777,0o600)
        calls=[json.loads(x) for x in (self.p/'calls').read_text().splitlines()]
        self.assertEqual(len(calls),4)
        for c in calls:self.assertNotIn('fake-access',json.dumps(c['argv']))
        self.assertFalse(self.record('chatgpt').exists())
    def test_failure_does_not_overwrite_account_and_logout_is_local(self):
        self.assertEqual(self.run_action('login').returncode,0);before=self.record().read_bytes()
        self.fixture['deny']=True;self.write_fixture()
        self.assertNotEqual(self.run_action('login').returncode,0);self.assertEqual(self.record().read_bytes(),before)
        self.assertEqual(self.run_action('logout').returncode,0);self.assertFalse(self.record().exists())
    def test_refresh_atomic_rotation_and_no_secret_in_arguments(self):
        self.assertEqual(self.run_action('login').returncode,0)
        record=json.loads(self.record().read_text());record['expires_at']='1';self.record().write_text(json.dumps(record))
        self.fixture['token']['refresh_token']='rotated-test';self.write_fixture()
        r=self.run_action('models');self.assertEqual(r.returncode,0,r.stdout)
        self.assertEqual(json.loads(self.record().read_text())['refresh_token'],'rotated-test')
        for c in map(json.loads,(self.p/'calls').read_text().splitlines()):
            self.assertNotIn('fake-access',json.dumps(c['argv']));self.assertNotIn('fake-refresh',json.dumps(c['argv']))
    def test_sse_incomplete_and_shell_metacharacters(self):
        self.assertEqual(self.run_action('login').returncode,0)
        q='你好 " $(touch '+str(self.p/'SHOULD_NOT_EXIST')+')\n第二行'
        r=self.run_action('test',{'model':'grok-test','question':q});self.assertEqual(r.returncode,0,r.stdout)
        self.assertIn('测试完整回答',r.stdout.decode());self.assertFalse((self.p/'SHOULD_NOT_EXIST').exists())
        call=json.loads((self.p/'calls').read_text().splitlines()[-1]);self.assertEqual(json.loads(call['body'])['input'][0]['content'],q)
        self.fixture['incomplete']=True;self.write_fixture();r=self.run_action('test',{'model':'grok-test','question':'test'})
        self.assertNotEqual(r.returncode,0);self.assertNotIn('测试完整回答',r.stdout.decode())
    def test_missing_models_and_rejected_scope(self):
        self.fixture['token']['scope']='openid';self.write_fixture();self.assertNotEqual(self.run_action('login').returncode,0);self.assertFalse(self.record().exists())
        self.fixture['token']['scope']='grok-cli:access api:access';self.fixture['models']={'unexpected':[]};self.write_fixture()
        self.assertEqual(self.run_action('login').returncode,0);self.assertNotEqual(self.run_action('models').returncode,0)
    def test_model_dropdown_uses_server_ids_without_inference_or_private_metadata(self):
        self.assertEqual(self.run_action('login').returncode,0)
        self.fixture['models']={'data':[{'id':'grok-one','name':'快速模型','instructions':'private instructions'},
                                      {'id':'grok-two'}]};self.write_fixture()
        before=self.record().read_bytes()
        r=self.run_action('models');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertEqual(json.loads(r.stdout)['options'],{'model':[{'value':'grok-one','label':'快速模型'},{'value':'grok-two','label':'grok-two'}]})
        self.assertNotIn(b'private instructions',r.stdout)
        self.assertEqual(self.record().read_bytes(),before)
        self.assertFalse(any(c['url'].endswith('/responses') for c in map(json.loads,(self.p/'calls').read_text().splitlines())))
    def test_model_dropdown_merges_supported_catalog_shapes_and_deduplicates(self):
        self.assertEqual(self.run_action('login').returncode,0)
        self.fixture['models']={
            'data':[{'id':'grok-test','name':'默认模型'}, {'id':'grok-fast','name':'快速模型'}],
            'models':[{'slug':'grok-test','display_name':'默认模型','visibility':'list'},
                      {'slug':'grok-reasoning','display_name':'推理模型','visibility':'list'}],
        };self.write_fixture()
        r=self.run_action('models');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        options=json.loads(r.stdout)['options']['model']
        self.assertEqual([item['value'] for item in options],['grok-test','grok-fast','grok-reasoning'])
        self.assertEqual(options[-1]['label'],'推理模型')
    def test_voice_request_history_uses_actual_adapter(self):
        self.assertEqual(self.run_action('login').returncode,0)
        turns=[{'role':'user','content':'我叫小明'},{'role':'assistant','content':'你好小明'},{'role':'user','content':'我叫什么'}]
        r=subprocess.run(['sh',str(self.p/'scripts/grok_account.sh'),'--model-request'],input=json.dumps({'values':{'model':'grok-test'},'input':turns}),text=True,capture_output=True,env=self.env,timeout=10)
        self.assertEqual(r.returncode,0,r.stderr);self.assertEqual([json.loads(x) for x in r.stdout.splitlines()],[{'delta':'测试完整回答'},{'done':True}])
        call=json.loads((self.p/'calls').read_text().splitlines()[-1])
        self.assertEqual(json.loads(call['body'])['input'],turns)

    def test_large_catalogue_repeated_mappings_do_not_count_models_twice(self):
        self.assertEqual(self.run_action('login').returncode,0)
        self.fixture['models']={'data':[{'id':'grok-'+str(i),'name':'模型 '+str(i)} for i in range(40)]}
        self.write_fixture()
        r=self.run_action('models')
        self.assertEqual(r.returncode,0,r.stdout)
        self.assertEqual(len(json.loads(r.stdout)['options']['model']),40)
    def test_responses_low_reasoning_only_for_verified_provider_models(self):
        self.assertEqual(self.run_action('login').returncode,0)
        query=self.chatgpt_link();self.chatgpt_identity(query)
        r=self.run_action('complete-login',{'auth_callback':self.chatgpt_uri(query)},provider='chatgpt')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        for provider,model,low in [('chatgpt','gpt-6.1-sol',True),('chatgpt','custom-account-model',False),
                                   ('chatgpt','grok-4.7',False),('grok','gpt-6.1-sol',False),
                                   ('grok','grok-4.7',True),('grok','grok-4.6',True),('grok','grok-4.5',True),
                                   ('grok','grok-test',False),('grok','grok-4.7-fast',False),
                                   ('grok','grok-4.7-latest',False),('grok','grok-4.6-preview',False),
                                   ('grok','grok-4.5-thinking',False),('grok','grok-4',False),('grok','GROK-4.7',False)]:
            for voice in (False,True):
                with self.subTest(provider=provider,model=model,voice=voice):
                    if voice:
                        r=subprocess.run(['sh',str(self.p/'scripts'/(provider+'_account.sh')),'--model-request'],input=json.dumps({'values':{'model':model},'input':[{'role':'user','content':'你好'}]}),text=True,capture_output=True,env=self.env,timeout=10)
                    else:r=self.run_action('test',{'model':model,'question':'你好'},provider=provider)
                    self.assertEqual(r.returncode,0,r.stdout)
                    call=json.loads((self.p/'calls').read_text().splitlines()[-1]);body=json.loads(call['body'])
                    self.assertEqual(body['model'],model);self.assertIs(body['store'],False);self.assertIs(body['stream'],True)
                    if low:self.assertEqual(body['reasoning'],{'effort':'low'})
                    else:self.assertNotIn('reasoning',body)
                    self.assertNotIn('reasoning_effort',body)
    def test_grok_low_reasoning_actual_voice_request_uses_private_http_pool_body(self):
        # Execute the complete generated --model-request entry, including the
        # real prepare/options/http/FIFO/parser path. Only the transport peer is
        # synthetic; this is not a mirror of provider_request_options().
        self.assertEqual(self.run_action('login').returncode,0)
        helper=self.p/'bin/model-http'
        helper.write_text(r'''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
p=Path(sys.argv[3]);root=Path(os.environ['FIXTURE'])
assert sys.argv[1]=='--request' and p.stat().st_mode&0o777==0o700
for name in ('method','url','headers','body','timeout'):
 f=p/name;assert f.is_file() and not f.is_symlink() and f.stat().st_mode&0o777==0o600
with (root/'pool-calls').open('a') as f:f.write(json.dumps(dict(argv=sys.argv[1:],method=(p/'method').read_text(),url=(p/'url').read_text(),body=json.loads((p/'body').read_text()),timeout=(p/'timeout').read_text()))+'\n')
(p/'status').write_text('200')
print('data: '+json.dumps(dict(type='response.output_text.delta',delta='测试完整回答'))+'\n\n',end='')
print('data: '+json.dumps(dict(type='response.completed'))+'\n\n',end='')
''')
        helper.chmod(0o700)
        sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);self.addCleanup(sock.close)
        path=self.p/'pool.sock';sock.bind(str(path))
        turns=[{'role':'user','content':'请完整介绍故宫，保留全部问题。'}]
        env={**self.env,'LX06_MODEL_HTTP_HELPER':str(helper),'LX06_MODEL_HTTP_SOCKET':str(path)}
        initial_curl_calls=len((self.p/'calls').read_text().splitlines())
        for model,low in [('grok-4.7',True),('grok-4.6',True),('grok-4.5',True),('grok-4.7-fast',False)]:
            with self.subTest(model=model):
                r=subprocess.run(['sh',str(self.p/'scripts/grok_account.sh'),'--model-request'],
                                 input=json.dumps(dict(values=dict(model=model),input=turns)),text=True,capture_output=True,env=env,timeout=10)
                self.assertEqual(r.returncode,0,r.stdout+r.stderr)
                self.assertEqual([json.loads(line) for line in r.stdout.splitlines()],[{'delta':'测试完整回答'},{'done':True}])
                call=json.loads((self.p/'pool-calls').read_text().splitlines()[-1]);body=call['body']
                self.assertEqual((call['method'],call['url'],call['timeout']),('POST','https://cli-chat-proxy.grok.com/v1/responses','120'))
                self.assertEqual(body['model'],model);self.assertEqual(body['input'],turns)
                self.assertIs(body['store'],False);self.assertIs(body['stream'],True)
                self.assertEqual(set(body),{'model','store','stream','input'}|({'reasoning'} if low else set()))
                if low:self.assertEqual(body['reasoning'],{'effort':'low'})
                self.assertNotIn('reasoning_effort',body)
                self.assertNotIn('fake-access',json.dumps(call['argv']))
                self.assertNotIn('fake-access',r.stdout+r.stderr)
        self.assertEqual(len((self.p/'calls').read_text().splitlines()),initial_curl_calls,'pool request fell back to curl')
    def test_protocol_upgrade_error_preserves_login_and_hides_raw_error(self):
        self.assertEqual(self.run_action('login').returncode,0);before=self.record().read_bytes()
        self.fixture['response_status']=426;self.write_fixture()
        r=self.run_action('test',{'model':'grok-test','question':'test'})
        self.assertNotEqual(r.returncode,0);self.assertIn('更新适配脚本',r.stdout.decode())
        self.assertNotIn(b'private upstream diagnostic',r.stdout);self.assertEqual(self.record().read_bytes(),before)
    def test_chatgpt_real_pkce_callback_and_signed_identity(self):
        account=self.accounts/'chatgpt';account.mkdir()
        (account/'host-id').write_text('rejected-old-base64url-host-identifier')
        proc=subprocess.Popen(['sh',str(self.p/'scripts/chatgpt_account.sh'),'--web-action','login-auto'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=self.env)
        try:
            proc.stdin.write(b'{"values":{}}');proc.stdin.close();proc.stdin=None
            sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ)
            self.assertTrue(sel.select(3),'login did not publish callback URL');line=proc.stdout.readline();sel.close()
            event=json.loads(line);query=urllib.parse.parse_qs(urllib.parse.urlsplit(event['link']).query)
            self.assertRegex(query['ext_agent_host_id'][0],r'^urn:uuid:[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$')
            self.assertEqual(query['ext_agent_host_id'][0],(account/'host-id').read_text())
            self.assertEqual((account/'host-id').stat().st_mode&0o777,0o600)
            key=rsa.generate_private_key(public_exponent=65537,key_size=2048);nums=key.public_key().public_numbers();ib=lambda n:n.to_bytes((n.bit_length()+7)//8,'big')
            payload={'iss':'https://auth.openai.com','sub':'fixture-user','aud':'oaiapp_fixture','exp':int(time.time())+1000,'nonce':query['nonce'][0]}
            data=(b64(b'{"alg":"RS256","kid":"fixture"}')+'.'+b64(json.dumps(payload).encode())).encode()
            token=data.decode()+'.'+b64(key.sign(data,padding.PKCS1v15(),hashes.SHA256()))
            self.fixture['token'].update(id_token=token,scope='openid offline_access resource.invoke chatgpt.tokens.use.direct')
            self.fixture['jwks']={'keys':[{'kid':'fixture','kty':'RSA','n':b64(ib(nums.n)),'e':b64(ib(nums.e))}]};self.write_fixture()
            uri=query['redirect_uri'][0]+'?'+urllib.parse.urlencode({'state':query['state'][0],'code':'fixture-code','client_id':'oaiapp_fixture'})
            with urllib.request.urlopen(uri,timeout=3) as response:self.assertEqual(response.status,200)
            out,err=proc.communicate(timeout=5);self.assertEqual(proc.returncode,0,out+err)
            self.assertNotIn(token.encode(),out+err);record=json.loads(self.record('chatgpt').read_text())
            self.assertEqual(record['subject'],'fixture-user');self.assertEqual(record['client_id'],'oaiapp_fixture')
        finally:
            if proc.poll() is None:proc.terminate();proc.communicate(timeout=5)

    def test_chatgpt_host_id_stable_across_denied_logins(self):
        hosts=[];states=[]
        for _ in range(2):
            proc=subprocess.Popen(['sh',str(self.p/'scripts/chatgpt_account.sh'),'--web-action','login-auto'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=self.env)
            try:
                proc.stdin.write(b'{"values":{}}');proc.stdin.close();proc.stdin=None
                with selectors.DefaultSelector() as sel:
                    sel.register(proc.stdout,selectors.EVENT_READ)
                    self.assertTrue(sel.select(3),'login did not publish callback URL')
                    event=json.loads(proc.stdout.readline())
                query=urllib.parse.parse_qs(urllib.parse.urlsplit(event['link']).query)
                hosts.append(query['ext_agent_host_id'][0]);states.append(query['state'][0])
                uri=query['redirect_uri'][0]+'?'+urllib.parse.urlencode({'state':states[-1],'error':'access_denied'})
                with urllib.request.urlopen(uri,timeout=3) as response:self.assertEqual(response.status,200)
                proc.communicate(timeout=5);self.assertNotEqual(proc.returncode,0)
                self.assertFalse(self.record('chatgpt').exists())
            finally:
                if proc.poll() is None:proc.terminate();proc.communicate(timeout=5)
        self.assertEqual(hosts[0],hosts[1]);self.assertNotEqual(states[0],states[1])

    def test_chatgpt_bad_host_does_not_replace_registered_identity(self):
        account=self.accounts/'chatgpt';account.mkdir()
        host=account/'host-id';host.write_text('invalid-existing-identity')
        self.record('chatgpt').write_text('{"client_id":"oaiapp_existing"}')
        before=self.record('chatgpt').read_bytes()
        r=self.run_action('login',provider='chatgpt')
        self.assertNotEqual(r.returncode,0);self.assertIn('主机标识异常',r.stdout.decode())
        self.assertEqual(host.read_text(),'invalid-existing-identity')
        self.assertEqual(self.record('chatgpt').read_bytes(),before)
        self.assertFalse((self.p/'calls').exists())

    def test_chatgpt_reopen_load_resumes_same_private_authorization(self):
        query=self.chatgpt_link()
        pending=self.p/'chatgpt-pending'
        before={p.name:p.read_bytes() for p in pending.iterdir()}
        settings=self.p/'settings'
        settings.write_text('#!/bin/sh\nprintf \'%s\\n\' \'{"values":{"model":"gpt-6.1-sol"}}\'\n');settings.chmod(0o700)
        path=self.p/'scripts/chatgpt_account.sh'
        path.write_text(path.read_text().replace('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS='+str(settings)))
        result=self.run_action('load',provider='chatgpt')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        events=[json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(events[-1]['step'],'authorize')
        self.assertEqual(urllib.parse.parse_qs(urllib.parse.urlsplit(events[-1]['link']).query)['state'],query['state'])
        self.assertEqual({p.name:p.read_bytes() for p in pending.iterdir()},before)

if __name__=='__main__':unittest.main()

"""Run the actual upload and C JSON/schema helpers with a fake HTTPS transport.

No real credentials, account calls, speaker changes or payment operations.
"""
import json
import base64
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

# Loopback-only fixture used by the mocked login endpoint.
FAKE_PHONE = '138' + '0' * 8
from build_deepseek_login import build
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization

ROOT=Path(__file__).resolve().parent
FAKE_CURL=r'''#!/usr/bin/env python3
import json,os,sys,re,base64
from pathlib import Path
from cryptography.hazmat.primitives import serialization,hashes,padding
from cryptography.hazmat.primitives.asymmetric import padding as asymmetric_padding
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes
a=sys.argv[1:];root=Path(os.environ['FIXTURE']);fixture=json.loads((root/'fixture.json').read_text())
def arg(k):return a[a.index(k)+1] if k in a else ''
url=a[-1];status=fixture.get('status',200)
headers=Path(arg('--config')).read_text()
body=''
if '--data-binary' in a and url.endswith('/users/create_guest_challenge'):
 body=Path(arg('--data-binary')[1:]).read_text()
elif '--data-binary' in a:
 encrypted=Path(arg('--data-binary')[1:]).read_bytes()
 key=serialization.load_pem_private_key((root/'private.pem').read_bytes(),None)
 wrapped=base64.b64decode(re.search(r'x-request-encryption-key: ([A-Za-z0-9+/=]+)',headers)[1])
 material=key.decrypt(wrapped,asymmetric_padding.OAEP(mgf=asymmetric_padding.MGF1(hashes.SHA256()),algorithm=hashes.SHA256(),label=None))
 decryptor=Cipher(algorithms.AES(material[:16]),modes.CBC(material[16:])).decryptor()
 padded=decryptor.update(encrypted)+decryptor.finalize()
 unpadder=padding.PKCS7(128).unpadder();body=(unpadder.update(padded)+unpadder.finalize()).decode()
with (root/'calls').open('a') as f:f.write(json.dumps(dict(url=url,argv=a,body=body,headers=headers))+'\n')
if fixture.get('transport_error'):raise SystemExit(6)
if url.endswith('/users/create_guest_challenge'):
 result=fixture.get('guest',{})
 assert json.loads(body)['target_path'] in ['/api/v0/users/create_sms_verification_code','/api/v0/users/login_by_mobile_sms']
elif url.endswith('/users/login'):result=fixture['login']
elif url.endswith('/users/create_sms_verification_code'):
 result=fixture['sms']
 # Protocol captured from the official site's flow-label -> scenario mapping.
 if json.loads(body).get('scenario')!='login' or 'x-client-platform: web' not in headers:
  status=400;result=dict(code=40000,msg='invalid request')
elif url.endswith('/users/login_by_mobile_sms'):result=fixture.get('sms_login',fixture['login'])
elif url.endswith('/users/current'):result=fixture['current'];status=fixture.get('current_status',status)
else:raise SystemExit(3)
if url.endswith(('/users/create_sms_verification_code','/users/login_by_mobile_sms')):
 match=re.search(r'X-DS-Guest-PoW-Response: ([A-Za-z0-9+/=]+)',headers)
 assert match,'missing guest proof'
 proof=json.loads(base64.b64decode(match[1]));assert proof==dict(salt='testsalt',answer=42),proof
Path(arg('--output')).write_text(result if isinstance(result,str) else json.dumps(result))
print(status,end='')
'''


def envelope(user):return dict(code=0,data=dict(biz_code=0,biz_data=dict(user=user)))


class DeepSeekLoginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.base=Path(cls.tmp.name);cls.tool=cls.base/'tool';cls.settings=cls.base/'settings'
        cls.sealer=cls.base/'auth-seal'
        subprocess.run(['gcc','-O2','-Wno-deprecated-declarations',str(ROOT/'auth-seal.c'),'-lcrypto','-o',str(cls.sealer)],check=True)
        cls.digesttool=cls.base/'digest'
        digestsource=cls.base/'digest.c'
        digestsource.write_text('#include "hash23.h"\nint main(int n,char **v){unsigned char out[32];if(n!=2)return 2;hash23_digest((unsigned char*)v[1],strlen(v[1]),out);for(int i=0;i<32;i++)printf("%02x",out[i]);return 0;}')
        subprocess.run(['gcc','-O2','-I'+str(ROOT),str(digestsource),'-o',str(cls.digesttool)],check=True)
        cls.key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        cls.public=base64.b64encode(cls.key.public_key().public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)).decode()
        subprocess.run(['gcc','-O2',str(ROOT/'model-tool.c'),'-lcrypto','-o',str(cls.tool)],check=True)
        subprocess.run(['gcc','-O2',f'-DLX06_SETTINGS_DIR="{cls.base}/settings-store"',str(ROOT/'script-settings.c'),'-o',str(cls.settings)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        self.tmpcase=tempfile.TemporaryDirectory();self.addCleanup(self.tmpcase.cleanup);self.p=Path(self.tmpcase.name)
        self.script=build(self.p/'scripts',self.sealer,self.public)
        (self.p/'private.pem').write_bytes(self.key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        text=self.script.read_text().replace('TOOL=/usr/bin/lx06-model-tool','TOOL='+str(self.tool))
        text=text.replace('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS='+str(self.settings))
        text=text.replace('ROOT=/data/lx06/model-accounts','ROOT='+str(self.p/'accounts'))
        text=text.replace('[ "$(id -u)" = 0 ]','[ 0 = 0 ]').replace('/tmp/lx06-account.',str(self.p/'work.'))
        text=text.replace('SMS_COOLDOWN=/tmp/lx06-deepseek-sms-cooldown','SMS_COOLDOWN='+str(self.p/'cooldown'))
        text=text.replace('DS_DIAGNOSTIC=/tmp/lx06-deepseek-last-request.json','DS_DIAGNOSTIC='+str(self.p/'diagnostic.json'))
        self.script.write_text(text);self.script.chmod(0o600)
        bin=self.p/'bin';bin.mkdir();(bin/'curl').write_text(FAKE_CURL);(bin/'curl').chmod(0o700)
        self.env={**os.environ,'FIXTURE':str(self.p),'PATH':str(bin)+':'+os.environ['PATH']}
        self.fixture=dict(login=envelope(dict(id='user-1',token='fake-ds-token')),current=envelope(dict(id='user-1',token='rotated-ds-token')),sms=dict(code=0,data=dict(biz_code=0,biz_data=dict(send_window_secs=60))))
        expiry=int(time.time()*1000)+300000
        challenge=subprocess.check_output([str(self.digesttool),f'testsalt_{expiry}_42'],text=True)
        self.fixture['guest']=dict(code=0,data=dict(biz_code=0,biz_data=dict(guest_challenge=dict(algorithm='DeepSeekHashV1',salt='testsalt',expire_at=expiry,difficulty=80000,challenge=challenge))))
        self.record=self.p/'accounts/deepseek/credentials.json'
    def action(self,action,values=None):
        (self.p/'fixture.json').write_text(json.dumps(self.fixture))
        values=values or dict(account='tester@example.com',password='fake-password',area_code='+86')
        r=subprocess.run(['sh',str(self.script),'--web-action',action],input=json.dumps(dict(values=values)),text=True,capture_output=True,env=self.env,timeout=5)
        for secret in ['fake-password','fake-ds-token','rotated-ds-token','tester@example.com']:
            self.assertNotIn(secret,r.stdout+r.stderr)
        self.assertFalse(list(self.p.glob('work.*')),'private request temporary files leaked')
        return r
    def test_real_schema_and_nonresident_no_wake_markers(self):
        r=subprocess.run([str(self.settings),'show',str(self.script)],text=True,capture_output=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        j=json.loads(r.stdout);self.assertNotIn('password',j['values'])
        text=self.script.read_text();self.assertNotIn('# LX06-LOCALWAKEUP: on',text);self.assertNotIn('# LX06-RESIDENT: on',text)
        r=self.action('load');self.assertEqual(r.returncode,0);self.assertFalse((self.p/'calls').exists())
        values=json.loads(r.stdout)['values']
        self.assertEqual(values,{'account':'','area_code':'+86'})
    def test_login_checks_current_and_saves_only_token_identity(self):
        r=self.action('login');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        j=json.loads(self.record.read_text());self.assertEqual(set(j),{'access_token','subject','verified_at'})
        self.assertEqual(j['access_token'],'rotated-ds-token');self.assertEqual(self.record.stat().st_mode&0o777,0o600)
        calls=[json.loads(s) for s in (self.p/'calls').read_text().splitlines()]
        self.assertEqual(len(calls),2)
        body=json.loads(calls[0]['body']);self.assertEqual(body['email'],'tester@example.com');self.assertEqual(body['os'],'web')
        for c in calls:
            self.assertNotIn('fake-password',json.dumps(c['argv']));self.assertNotIn('fake-ds-token',json.dumps(c['argv']))
        self.assertEqual(self.action('status').returncode,0)
    def test_valid_login_does_not_claim_muted_account_can_chat(self):
        user=dict(id='user-1',token='rotated-ds-token',chat=dict(is_muted=1,mute_until=time.time()+86400))
        self.fixture['current']=envelope(user)
        r=self.action('login');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('禁言',r.stdout);self.assertIn('北京时间',r.stdout)
        before=self.record.read_bytes()
        r=self.action('status');self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertIn('登录凭据有效',r.stdout);self.assertIn('禁言',r.stdout)
        self.assertEqual(json.loads(self.record.read_text())['access_token'],json.loads(before)['access_token'])
        self.fixture['current']=envelope(dict(id='user-1',token='rotated-ds-token',chat=dict(is_muted=False,mute_until=0)))
        r=self.action('status');self.assertNotIn('禁言',r.stdout)
    def test_website_available_without_network_and_preserves_login(self):
        # Inherited by VoiceShellTest: exercise the generated voice upload too,
        # not only the login-only schema that originally retained the button.
        schema=json.loads(self.script.read_text().split('# LX06-EXTENSION-BEGIN\n# ',1)[1].split('\n',1)[0])
        self.assertEqual(next(a for a in schema['actions'] if a['id']=='website')['fields'],[])
        self.record.parent.mkdir(parents=True,exist_ok=True)
        original=b'{"access_token":"existing-private-token","subject":"user-1"}'
        self.record.write_bytes(original)
        r=self.action('website',{})
        self.assertEqual(r.returncode,0,r.stderr)
        event=json.loads(r.stdout)
        self.assertEqual(event['link'],'https://chat.deepseek.com/sign_in')
        self.assertIn('验证码登录',event['message'])
        self.assertEqual(self.record.read_bytes(),original)
        self.assertFalse((self.p/'calls').exists(),'opening website must not call login/SMS APIs')
        self.assertNotIn('existing-private-token',r.stdout+r.stderr)
    def test_mobile_normalization_and_json_metacharacters(self):
        password='p"\\ $(touch NEVER); `false`'
        r=self.action('login',dict(account='+8613800000000',password=password,area_code='+86'))
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        body=json.loads(json.loads((self.p/'calls').read_text().splitlines()[0])['body'])
        self.assertEqual(body['password'],password);self.assertEqual(body['mobile'],FAKE_PHONE);self.assertEqual(body['area_code'],'+86')
        self.assertNotIn('password',self.record.read_text());self.assertFalse((self.p/'NEVER').exists())
    def test_failed_relogin_preserves_existing_record(self):
        self.assertEqual(self.action('login').returncode,0);before=self.record.read_bytes()
        for response in ['<html>verification</html>',dict(code=0),envelope({}),envelope(dict(id='user-1',token='bad\r\nHeader: x')),dict(code=0,data=dict(biz_code=7,biz_data=dict(user=dict(id='user-1',token='fake-ds-token'))))]:
            self.fixture['login']=response
            self.assertNotEqual(self.action('login').returncode,0)
            self.assertEqual(self.record.read_bytes(),before)
    def test_identity_mismatch_and_rate_limit_cannot_report_success(self):
        self.assertEqual(self.action('login').returncode,0);before=self.record.read_bytes()
        self.fixture['current']=envelope(dict(id='different-user',token='fake-ds-token'))
        self.assertNotEqual(self.action('login').returncode,0)
        self.fixture['current_status']=429
        r=self.action('status');self.assertNotEqual(r.returncode,0);self.assertIn('429',r.stdout)
        self.assertEqual(self.record.read_bytes(),before)
    def test_transport_failure_and_missing_password(self):
        self.fixture['transport_error']=True
        self.assertNotEqual(self.action('login').returncode,0);self.assertFalse(self.record.exists())
        self.assertNotEqual(self.action('login',dict(account='tester@example.com',password='',area_code='+86')).returncode,0)
        for bad in ['fake-password\rInjected','fake-password\nInjected']:
            self.assertNotEqual(self.action('login',dict(account='tester@example.com',password=bad,area_code='+86')).returncode,0)
    def test_invalid_saved_token_is_visible_without_leaking_or_deleting_record(self):
        self.assertEqual(self.action('login').returncode,0)
        before=self.record.read_bytes()
        self.fixture['current']=dict(code=40003,msg='Authorization Failed (invalid token): fake-ds-token tester@example.com',data=None)
        r=self.action('status')
        self.assertNotEqual(r.returncode,0)
        self.assertIn('重新登录',r.stdout)
        self.assertIn('重新登录',r.stderr)
        self.assertIn('code=40003',r.stderr)
        self.assertIn('/api/v0/users/current',r.stderr)
        self.assertNotIn('Authorization Failed',r.stdout+r.stderr)
        self.assertEqual(self.record.read_bytes(),before)
    def test_logout_uninstall_do_not_touch_other_accounts(self):
        other=self.p/'accounts/grok';other.mkdir(parents=True);(other/'credentials.json').write_text('keep')
        self.assertEqual(self.action('login').returncode,0)
        self.assertEqual(self.action('logout').returncode,0);self.assertFalse(self.record.exists())
        self.assertEqual(self.action('login').returncode,0)
        r=subprocess.run(['sh',str(self.script),'--uninstall'],env=self.env,text=True,capture_output=True)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertFalse(self.record.parent.exists())
        self.assertEqual((other/'credentials.json').read_text(),'keep')

    def test_sms_send_cooldown_and_existing_account_code_one(self):
        values=dict(account=FAKE_PHONE,area_code='+86',sms_code='123456')
        self.assertEqual(self.action('send_sms',values).returncode,0)
        device=(self.record.parent/'device-id').read_text().strip()
        self.assertNotEqual(self.action('send_sms',values).returncode,0)
        self.fixture['sms_login']=envelope(dict(id='user-1',token='fake-ds-token'))
        self.fixture['sms_login']['data']['biz_code']=1
        self.assertEqual(self.action('sms_login',values).returncode,0)
        calls=[json.loads(s) for s in (self.p/'calls').read_text().splitlines()]
        self.assertEqual(len(calls),5)
        sent=json.loads(calls[1]['body']);logged=json.loads(calls[3]['body'])
        self.assertEqual(sent['scenario'],'login');self.assertEqual(sent['mobile_number'],FAKE_PHONE)
        self.assertEqual(sent['device_id'],device);self.assertEqual(logged['device_id'],device)
        self.assertEqual(logged['sms_verification_code'],'123456')
        self.assertNotIn('123456',self.record.read_text())
        self.assertNotIn('sms_verification_code',self.record.read_text())

    def test_sms_captcha_and_wrong_expired_codes_never_save(self):
        values=dict(account=FAKE_PHONE,area_code='+86',sms_code='123456')
        self.fixture['sms']['data']['biz_code']=2
        r=self.action('send_sms',values)
        self.assertNotEqual(r.returncode,0);self.assertIn('https://chat.deepseek.com/sign_in',r.stdout)
        self.assertFalse(self.record.exists())
        for code in [7,8,9,11,98]:
            self.fixture['sms_login']=dict(code=0,data=dict(biz_code=code))
            self.assertNotEqual(self.action('sms_login',values).returncode,0)
            self.assertFalse(self.record.exists())
        for bad in ['', '12345', '12x456', '1234567']:
            self.assertNotEqual(self.action('sms_login',dict(values,sms_code=bad)).returncode,0)

    def test_legacy_sms_scenario_rejected_and_only_safe_diagnostics_kept(self):
        values=dict(account=FAKE_PHONE,area_code='+86')
        self.script.write_text(self.script.read_text().replace('ds_device;put scenario login','ds_device;put scenario mobileLogin'))
        r=self.action('send_sms',values)
        self.assertNotEqual(r.returncode,0);self.assertIn('请求格式',r.stdout)
        diag=json.loads((self.p/'diagnostic.json').read_text())
        self.assertEqual(diag,dict(http='400',route='/api/v0/users/create_sms_verification_code',stage='request',code='40000',biz_code=''))
        self.assertNotIn(FAKE_PHONE,json.dumps(diag))

    def test_hash_official_vectors_solver_and_budget(self):
        vectors={
          '':'e594808bc5b7151ac160c6d39a02e0a8e261ed588578403099e3561dc40c26b3',
          'testsalt_1700000000_42':'d4a2ea58c89e40887c933484868380c6f803eaa8dc53a3b9df8e431b921a4f09',
          'testsalt_1700000000_100000':'abea2f35796b65486e9be1b36f7878c66cab021e96faa473fdf4decd31f9ba30',
          'abc123salt_1700000000_12345':'74b3b7452745b70e85eb32ee7f0a9ec0381d42dd5137b695da915e104fc390e1'}
        for text,expected in vectors.items():
            self.assertEqual(subprocess.check_output([str(self.digesttool),text],text=True),expected)
        files=[self.p/name for name in ['salt','expiry','limit','hash']]
        for path,value in zip(files,['abc123salt','1700000000','20000',vectors['abc123salt_1700000000_12345']]):path.write_text(value)
        command=[str(self.sealer),'--hash23-solve',*map(str,files)]
        r=subprocess.run(command,capture_output=True,text=True,timeout=3)
        self.assertEqual((r.returncode,r.stdout),(0,'12345'))
        for value in ['200001','4294967297','0']:
            files[2].write_text(value)
            self.assertNotEqual(subprocess.run(command,capture_output=True,timeout=3).returncode,0)
        self.assertEqual(subprocess.check_output([str(self.sealer),'--base64'],input=b'{"salt":"test","answer":42}'),base64.b64encode(b'{"salt":"test","answer":42}'))

    def test_guest_refusal_expiry_and_http200_diagnostic(self):
        values=dict(account=FAKE_PHONE,area_code='+86',sms_code='123456')
        puzzle=self.fixture['guest']['data']['biz_data']['guest_challenge']
        for name,value in [('algorithm','UnknownV2'),('difficulty',200001),('expire_at',1700000000000)]:
            old=puzzle[name];puzzle[name]=value
            self.assertNotEqual(self.action('sms_login',values).returncode,0)
            self.assertFalse(self.record.exists());puzzle[name]=old
        self.fixture['sms_login']=dict(code=40301,data=dict(biz_code=0))
        r=self.action('sms_login',values)
        self.assertNotEqual(r.returncode,0);self.assertIn('访客校验',r.stdout)
        diag=json.loads((self.p/'diagnostic.json').read_text())
        self.assertEqual(diag['code'],'40301');self.assertEqual(diag['http'],'200')
        self.assertNotIn(FAKE_PHONE,json.dumps(diag));self.assertFalse(self.record.exists())


if __name__=='__main__':unittest.main()

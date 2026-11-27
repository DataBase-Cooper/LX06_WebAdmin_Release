import base64,hashlib,json,os,selectors,socket,subprocess,tempfile,time,unittest,urllib.request,urllib.error
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import rsa,padding
from cryptography.hazmat.primitives import hashes
ROOT=Path(__file__).resolve().parent
b64=lambda b:base64.urlsafe_b64encode(b).rstrip(b'=').decode()

class ToolTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.p=Path(cls.tmp.name);cls.tool=cls.p/'tool'
        subprocess.run(['gcc','-O2',str(ROOT/'model-tool.c'),'-lcrypto','-o',str(cls.tool)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def call(self,*args,data=b''):
        return subprocess.run([str(self.tool),*map(str,args)],input=data,capture_output=True,timeout=5)
    def test_primitives(self):
        verifier='a'*43
        self.assertEqual(self.call('pkce',data=verifier.encode()).stdout.decode().strip(),b64(hashlib.sha256(verifier.encode()).digest()))
        self.assertEqual(len(self.call('random').stdout.strip()),43)
        self.assertEqual(json.loads(self.call('quote',data='"测试\n\\'.encode()).stdout),'"测试\n\\')
        self.assertEqual(self.call('get','v',data=b'{"v":"a","v":"b"}').returncode,3)
        self.assertEqual(self.call('get','v',data=b'{"v":"a\\u0000b"}').returncode,3)
    def test_form_and_object_never_interpolate(self):
        value='a&b=$(id)\n中文';p=self.p/'value';p.write_text(value)
        r=self.call('object','question',p);self.assertEqual(json.loads(r.stdout),{'question':value})
        r=self.call('form','question',p);self.assertIn(b'%26',r.stdout);self.assertNotIn(b'$(id)',r.stdout)
    def test_public_model_list_mapping(self):
        data=json.dumps({'data':[{'id':'model-a','name':'模型甲','internal':'not for display'},{'id':'model-b'}]}).encode()
        result=self.call('list','data','id','name',data=data)
        self.assertEqual(result.returncode,0)
        self.assertEqual(result.stdout.decode(),'model-a · 模型甲\nmodel-b\n')
        for value in [[],[{'name':'missing id'}],[{'id':'line\nbreak'}]]:
            self.assertNotEqual(self.call('list','data','id','name',data=json.dumps({'data':value}).encode()).returncode,0)
    def test_model_dropdown_maps_public_fields_and_filters_hidden_models(self):
        catalog={'models':[{'slug':'visible','display_name':'中文 <模型>','visibility':'list','instructions':'private metadata'},
                           {'slug':'other','visibility':'list'},{'slug':'hidden','visibility':'hide'}]}
        r=self.call('list-options','model','models','slug','display_name','visibility','list',data=json.dumps(catalog).encode())
        self.assertEqual(r.returncode,0,r.stderr)
        self.assertEqual(json.loads(r.stdout)['options'],{'model':[{'value':'visible','label':'中文 <模型>'},{'value':'other','label':'other'}]})
        self.assertNotIn(b'private metadata',r.stdout)
        self.assertNotIn(b'hidden',r.stdout)
    def test_model_dropdown_rejects_whole_invalid_or_oversized_catalog(self):
        bad=[[],[{'id':'duplicate'},{'id':'duplicate'}],[{'id':'x'*129}],
             [{'id':'bad\nvalue'}],[{'id':str(i)} for i in range(65)],
             [{'id':str(i),'name':'x'*160} for i in range(64)],
             [{'id':'good'},{'name':'missing id'}]]
        for items in bad:
            r=self.call('list-options','model','data','id','name',data=json.dumps({'data':items}).encode())
            self.assertNotEqual(r.returncode,0)
            self.assertEqual(r.stdout,b'',items)
        raw=b'{"data":[{"id":"valid"}]}\0garbage'
        for args in [('list-options','model','data','id','name'),('list','data','id','name')]:
            r=self.call(*args,data=raw)
            self.assertNotEqual(r.returncode,0)
            self.assertEqual(r.stdout,b'')
    def test_sse_completion_and_bounds(self):
        def run(events):
            data=''.join('data: '+json.dumps(e,ensure_ascii=False)+'\n\n' for e in events).encode()
            return self.call('sse','type','delta','text','done','failed,error',data=data)
        good=[{'type':'delta','text':'你好'},{'type':'done'}]
        self.assertEqual(json.loads(run(good).stdout),{'text':'你好'})
        for events in [good[:1],[{'type':'failed'}],good+[{'type':'delta','text':'late'}],[{'type':'delta','text':'x'*17000},{'type':'done'}]]:
            self.assertNotEqual(run(events).returncode,0)
        self.assertEqual(self.call('clip','5',data='你好呀'.encode()).stdout,'你'.encode())
    def test_sse_stream_emits_before_eof_and_rejects_incomplete(self):
        args=[str(self.tool),'sse-stream','type','delta','text','done','failed,error']
        p=subprocess.Popen(args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            p.stdin.write(('data: '+json.dumps({'type':'delta','text':'首句。'})+'\n\n').encode());p.stdin.flush()
            with selectors.DefaultSelector() as sel:
                sel.register(p.stdout,selectors.EVENT_READ);self.assertTrue(sel.select(2))
            self.assertEqual(json.loads(p.stdout.readline()),{'delta':'首句。'})
            self.assertIsNone(p.poll())
            p.stdin.close();p.stdin=None
            p.communicate(timeout=3);self.assertNotEqual(p.returncode,0)
        finally:
            if p.poll() is None:p.kill();p.communicate()
    def test_stream_error_is_signaled_without_waiting_for_upstream_eof(self):
        p=subprocess.Popen([str(self.tool),'sse-stream','type','delta','text','done','failed,error'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            p.stdin.write(b'data: {"type":"failed"}\n\n');p.stdin.flush()
            with selectors.DefaultSelector() as sel:
                sel.register(p.stdout,selectors.EVENT_READ);self.assertTrue(sel.select(2))
            self.assertEqual(json.loads(p.stdout.readline()),{'error':True})
            p.wait(timeout=2);self.assertNotEqual(p.returncode,0)
        finally:
            if p.poll() is None:p.kill()
            p.communicate()
    def test_large_catalog_filters_metadata_and_keeps_other_limits(self):
        catalog={'models':[{'slug':'visible','display_name':'显示模型','visibility':'list',
                            'instructions':'unused metadata '*43000,'options':list(range(1500))},
                           {'slug':'hidden','visibility':'hide'}]}
        raw=json.dumps(catalog).encode()
        result=self.call('list','models','slug','display_name','visibility','list',data=raw)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stdout.decode(),'visible · 显示模型\n')
        self.assertNotEqual(self.call('get','models',data=raw).returncode,0)
        for bad in [raw[:-2],b'{"models":[],"padding":"'+b'x'*(1024*1024)+b'"}',
                    b'{"models":[{"slug":"a","visibility":"hide"}]}']:
            self.assertNotEqual(self.call('list','models','slug','display_name','visibility','list',data=bad).returncode,0)
    def test_rs256_claims_and_signature(self):
        key=rsa.generate_private_key(public_exponent=65537,key_size=2048);numbers=key.public_key().public_numbers()
        intb=lambda n:n.to_bytes((n.bit_length()+7)//8,'big')
        jwks=self.p/'jwks';jwks.write_text(json.dumps({'keys':[{'kid':'test','kty':'RSA','alg':'RS256','n':b64(intb(numbers.n)),'e':b64(intb(numbers.e))}]}))
        nonce=self.p/'nonce';nonce.write_text('random_nonce')
        payload={'iss':'https://issuer.example','aud':'client','nonce':'random_nonce','exp':int(time.time())+120,'sub':'user'}
        jwt=self.p/'jwt'
        def verify(p,algorithm='RS256',corrupt=False):
            signed=(b64(json.dumps({'alg':algorithm,'kid':'test'}).encode())+'.'+b64(json.dumps(p).encode())).encode()
            signature=key.sign(signed,padding.PKCS1v15(),hashes.SHA256())
            if corrupt:signature=bytes([signature[0]^1])+signature[1:]
            jwt.write_text(signed.decode()+'.'+b64(signature))
            return self.call('jwt-verify',jwt,jwks,'https://issuer.example','client',nonce)
        self.assertEqual(verify(payload).returncode,0)
        for k,v in [('iss','https://other'),('aud','other'),('nonce','other'),('exp',1)]:
            p=payload.copy();p[k]=v;self.assertNotEqual(verify(p).returncode,0)
        self.assertNotEqual(verify(payload,corrupt=True).returncode,0)
        self.assertNotEqual(verify(payload,algorithm='none').returncode,0)
    def test_callback_binds_first_and_rejects_forged_state(self):
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        state=self.p/'state';state.write_text('a'*32);ready=self.p/'ready'
        proc=subprocess.Popen([str(self.tool),'callback',str(port),'/auth/callback',str(state),str(ready)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            until=time.monotonic()+2
            while not ready.exists() and time.monotonic()<until:time.sleep(.02)
            self.assertTrue(ready.exists())
            url=f'http://127.0.0.1:{port}/auth/callback'
            for query in ['state=wrong&code=private','state='+('a'*32)+'&state=other&code=private']:
                with self.assertRaises(urllib.error.HTTPError):urllib.request.urlopen(url+'?'+query,timeout=2)
            with urllib.request.urlopen(url+'?state='+('a'*32)+'&code=test-code&client_id=issued',timeout=2) as f:
                self.assertNotIn(b'test-code',f.read())
            out,err=proc.communicate(timeout=3);self.assertEqual(proc.returncode,0)
            self.assertEqual(json.loads(out)['code'],'test-code');self.assertFalse(ready.exists());self.assertEqual(err,b'')
        finally:
            if proc.poll() is None:proc.kill();proc.communicate()

if __name__=='__main__':unittest.main()

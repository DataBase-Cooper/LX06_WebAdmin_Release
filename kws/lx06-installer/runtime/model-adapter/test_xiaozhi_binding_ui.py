"""Structured binding output must preserve leading zero and never expose OTA tokens."""
import json, subprocess, tempfile, unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parent

class BindingUI(unittest.TestCase):
    def test_event_mapping(self):
        source=(ROOT/'xiaozhi-binding-ui.sh').read_text()
        for message,expected in [('绑定码 001234  请到 console 填写','001234'),
                                 ('小智已绑定，无需验证码',None),
                                 ('绑定码解析失败',None),
                                 ('绑定码 <bad> token-secret',None)]:
            with tempfile.TemporaryDirectory() as t:
                p=Path(t);(p/'message').write_text(message)
                settings=p/'settings';settings.write_text('#!/bin/sh\nprintf \'%s\\n\' \'{"values":{"XZ_BARGE_ANY":"0"}}\'\n');settings.chmod(0o700)
                tool=p/'tool'
                subprocess.run(['gcc','-O2',str(ROOT/'model-tool.c'),'-lcrypto','-o',str(tool)],check=True)
                helper=source.replace('/usr/bin/lx06-model-tool',str(tool)).replace('/tmp/lx06-xz-bound',str(p/'bound'))
                script=('SETTINGS='+str(settings)+'\n'+helper+
                        '\nbinding_cache_key(){ echo test-device; }\n'
                        'publish_bind_code(){ touch queried; log "private-token"; say "$(cat message)"; }\n'
                        'binding_web_load\n')
                r=subprocess.run(['sh','-c',script],cwd=p,capture_output=True)
                event=json.loads(r.stdout)
                self.assertEqual(event.get('code'),expected)
                self.assertEqual(r.returncode,0 if expected or '已绑定' in message else 1)
                if r.returncode==0:
                    self.assertEqual(event['step'],'pair' if expected else 'configure')
                    self.assertEqual(event['values'],{'XZ_BARGE_ANY':'0'})
                self.assertNotIn(b'private-token',r.stdout)
                self.assertNotIn(b'token-secret',r.stdout)
                if '已绑定' in message:
                    (p/'queried').unlink()
                    cached=subprocess.run(['sh','-c',script],cwd=p,capture_output=True,check=True)
                    self.assertEqual(json.loads(cached.stdout)['step'],'configure')
                    self.assertFalse((p/'queried').exists())

if __name__=='__main__':unittest.main()

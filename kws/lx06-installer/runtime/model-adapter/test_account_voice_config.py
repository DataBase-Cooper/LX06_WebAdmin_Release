"""Generated upload, schema persistence, worker overlay and shared wake routes."""
import json,os,subprocess,tempfile,time,unittest
from pathlib import Path
from build_accounts import build
from build_deepseek_login import build as build_deepseek
ROOT=Path(__file__).resolve().parent

class AccountVoiceConfigTest(unittest.TestCase):
    def test_saved_worker_values_independent_of_install_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);store=p/'settings';binary=p/'settings-helper';tool=p/'model-tool'
            subprocess.run(['gcc','-O2',f'-DLX06_SETTINGS_DIR="{store}"',str(ROOT/'script-settings.c'),'-o',str(binary)],check=True)
            subprocess.run(['gcc','-O2',str(ROOT/'model-tool.c'),'-lcrypto','-o',str(tool)],check=True)
            build(p/'scripts')
            # Build the DeepSeek upload with inert local fixture bytes only.
            # No account, authentication helper execution or network is needed.
            (p/'sealer').write_bytes(b'local sealer fixture')
            (p/'streamer').write_bytes(b'local stream fixture')
            build_deepseek(p/'scripts',p/'sealer','AA==',voice=True,streamer=p/'streamer')
            xa=p/'xa';xa.mkdir()
            voice=p/'voice';voice.write_text('#!/bin/sh\nif [ "$1" = --protocol ]; then echo LX06DIALOGUE/1; exit 0; fi\nprintf "%s|%s|%s|%s|%s|%s" "$MODEL_ID" "$MODEL_BARGE_ANY" "$MODEL_GOODBYE" "$4" "$MODEL_TTS_YIELD" "${MODEL_TTS_STREAM:-0}" > "$FIXTURE/values"\n');voice.chmod(0o700)
            for provider,alias in [('deepseek','深度思考'),('grok','召唤马斯克'),('chatgpt','召唤奥特曼')]:
                path=p/'scripts'/(provider+'_account.sh')
                text=path.read_text().replace('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS='+str(binary)).replace('TOOL=/usr/bin/lx06-model-tool','TOOL='+str(tool)).replace('XA=/usr/lib/lx06/xiaoai','XA='+str(xa)).replace('VOICE=/usr/bin/lx06-model-voice','VOICE='+str(voice)).replace('ROOT=/data/lx06/model-accounts','ROOT='+str(p/'accounts'))
                # This fixture supplies one extra declared seed; arbitrary
                # browser-entered model IDs are no longer valid choices.
                marker='# LX06-EXTENSION-BEGIN\n# '
                start=text.index(marker)+len(marker);end=text.index('\n# LX06-EXTENSION-END',start)
                schema=json.loads(text[start:end])
                model_field='MODEL_ID' if provider=='deepseek' else 'model'
                next(f for f in schema['fields'] if f['name']==model_field)['options'].append(provider+'-fixture')
                self.assertEqual(next(f for f in schema['fields'] if f['name']=='MODEL_TTS_YIELD')['default'],'0')
                self.assertNotIn('export MODEL_TTS_STREAM=',text)
                text=text[:start]+json.dumps(schema,ensure_ascii=False,separators=(',',':'))+text[end:]
                path.write_text(text);path.chmod(0o600)
                self.assertIn('# LX06-LOCALWAKEUP: on',text);self.assertIn('# 小爱通道唤醒词='+alias,text)
                def call(*args,data=None):
                    r=subprocess.run(['sh',str(path),*args],input=json.dumps(data) if data else None,text=True,capture_output=True,env={k:v for k,v in {**os.environ,'FIXTURE':str(p),'LX06_DIALOGUED':str(voice)}.items() if k!='MODEL_TTS_STREAM'},timeout=5)
                    self.assertEqual(r.returncode,0,r.stdout+r.stderr);return r
                values=json.loads(call('--web-action','load').stdout)['values']
                self.assertEqual(values['official_wake'],alias)
                self.assertEqual(values['MODEL_TTS_YIELD'],'0')
                call('--session-worker','默认配置')
                self.assertTrue((p/'values').read_text().endswith('|默认配置|0|0'))
                save_fields=next(a for a in schema['actions'] if a['id']=='save')['fields']
                values={k:v for k,v in values.items() if k in save_fields}
                values.update({model_field:provider+'-fixture'},MODEL_GOODBYE='测试结束,结束测试',MODEL_TRACE_ASR='1',MODEL_TTS_YIELD='1')
                if provider == 'deepseek':
                    self.assertIn('MODEL_BARGE_ANY',save_fields)
                    values['MODEL_BARGE_ANY']='1'
                else:
                    self.assertNotIn('MODEL_BARGE_ANY',save_fields)
                    # Seed a synthetic prior setting; this mode is not editable via save.
                    store.mkdir(mode=0o700,exist_ok=True)
                    state=store/(path.name+'.json')
                    state.write_text(json.dumps({'MODEL_BARGE_ANY':'1'}));state.chmod(0o600)
                call('--web-action','save',data={'name':path.name,'action':'save','values':values})
                call('--session-worker','测试唤醒')
                self.assertEqual((p/'values').read_text(),provider+'-fixture|1|测试结束,结束测试|测试唤醒|1|0')
                saved=json.loads(call('--web-action','load').stdout)['values']
                self.assertEqual(saved[model_field],provider+'-fixture');self.assertEqual(saved['MODEL_TTS_YIELD'],'1')
            self.assertEqual(len(list(store.glob('*.json'))),3)
    def test_builtin_save_rejects_forged_mode_without_changing_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);store=p/'settings';binary=p/'settings-helper';tool=p/'model-tool'
            subprocess.run(['gcc','-O2',f'-DLX06_SETTINGS_DIR="{store}"',str(ROOT/'script-settings.c'),'-o',str(binary)],check=True)
            subprocess.run(['gcc','-O2',str(ROOT/'model-tool.c'),'-lcrypto','-o',str(tool)],check=True)
            build(p/'scripts');store.mkdir(mode=0o700)
            for provider in ('grok','chatgpt'):
                path=p/'scripts'/(provider+'_account.sh')
                text=path.read_text().replace('SETTINGS=/usr/lib/lx06/script-settings','SETTINGS='+str(binary)).replace('TOOL=/usr/bin/lx06-model-tool','TOOL='+str(tool)).replace('XA=/usr/lib/lx06/xiaoai','XA='+str(p/'xa')).replace('ROOT=/data/lx06/model-accounts','ROOT='+str(p/'accounts'))
                schema=json.loads(text.split('# LX06-EXTENSION-BEGIN\n# ',1)[1].split('\n',1)[0])
                save_fields=next(a for a in schema['actions'] if a['id']=='save')['fields']
                self.assertNotIn('MODEL_BARGE_ANY',save_fields)
                path.write_text(text);path.chmod(0o600)
                state=store/(path.name+'.json')
                for mode in (None,'0','1'):
                    with self.subTest(provider=provider,mode=mode):
                        prior={'MODEL_GOODBYE':'original'}
                        if mode is not None: prior['MODEL_BARGE_ANY']=mode
                        initial=json.dumps(prior)
                        state.write_text(initial);state.chmod(0o600)
                        fields={f['name']:f for f in schema['fields']}
                        values={name:fields[name]['default'] for name in save_fields}
                        values['MODEL_GOODBYE']='changed'
                        for attempted in ('0','1'):
                            forged={'name':path.name,'action':'save','values':dict(values,MODEL_BARGE_ANY=attempted)}
                            result=subprocess.run(['sh',str(path),'--web-action','save'],input=json.dumps(forged),text=True,capture_output=True,timeout=5)
                            self.assertNotEqual(result.returncode,0,result.stdout+result.stderr)
                            self.assertEqual(state.read_text(),initial)
                        valid={'name':path.name,'action':'save','values':values}
                        result=subprocess.run(['sh',str(path),'--web-action','save'],input=json.dumps(valid),text=True,capture_output=True,timeout=5)
                        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                        saved=json.loads(state.read_text())
                        self.assertEqual(saved.get('MODEL_BARGE_ANY'),mode)
                        self.assertEqual('MODEL_BARGE_ANY' in saved,mode is not None)
                        self.assertEqual(saved['MODEL_GOODBYE'],'changed')
    def test_web_marker_actual_generated_headers(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);build(p/'scripts')
            web=(ROOT.parents[1]/'hooks/webd/webd.c').read_text()
            flags=web[web.index('#define APP_RESIDENT'):web.index('/* 读已部署应用文件头')]
            source='#include <stddef.h>\n#include <string.h>\n#include <stdio.h>\n#include <assert.h>\n'+flags+'''
int main(int argc,char**argv){char b[8193]={0};FILE*f=fopen(argv[1],"r");assert(f);size_t n=fread(b,1,8192,f);fclose(f);
unsigned flags=app_marker_flags((unsigned char*)b,n);
assert(flags&APP_DECLARED_RESIDENT);assert(flags&APP_LOCAL_WAKEWORD);assert(flags&APP_SESSION_MANAGED);assert(flags&APP_RUN_ROOT);return 0;}
'''
            (p/'markers.c').write_text(source)
            subprocess.run(['gcc','-I'+str(ROOT.parents[1]/'hooks/webd'),str(p/'markers.c'),'-o',str(p/'markers')],check=True)
            for script in (p/'scripts').glob('*.sh'):subprocess.run([str(p/'markers'),str(script)],check=True)

if __name__=='__main__':unittest.main()

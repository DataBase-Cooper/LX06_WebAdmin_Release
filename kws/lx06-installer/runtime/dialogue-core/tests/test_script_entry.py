"""Generated upload-to-core wiring. Synthetic helpers; no cloud/device access.

These fixtures verify settings overlays, saved identities and independent upload
order. The actual core ownership/audio path is tested separately: this mock
cannot establish real microphone, acoustic interruption or provider acceptance.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
ADAPTER=ROOT/'runtime/model-adapter'

def run(args,**kw):
    result=subprocess.run(list(map(str,args)),text=True,capture_output=True,timeout=8,**kw)
    assert result.returncode==0,(args,result.stdout[-1000:],result.stderr[-1500:])
    return result

def schema(text):
    return json.loads(text.split('# LX06-EXTENSION-BEGIN\n# ',1)[1].split('\n# LX06-EXTENSION-END',1)[0])

class ScriptEntry(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
        self.p=Path(self.temporary.name);self.uploads=self.p/'uploads';self.uploads.mkdir()
        self.env={**os.environ,'FIXTURE':str(self.p)}
        self.settings=self.p/'settings'
        run(['gcc','-O2',f'-DLX06_SETTINGS_DIR="{self.p}/saved"',ADAPTER/'script-settings.c','-o',self.settings])
        run(['gcc','-O2',ADAPTER/'model-tool.c','-lcrypto','-o',self.p/'tool'])
        self.core=self.p/'dialogued'
        self.core.write_text('''#!/usr/bin/env python3
import json,os,sys
if sys.argv[1:]==['--protocol']:print('LX06DIALOGUE/1');raise SystemExit(0)
names=['DC_MAX_SECONDS','DC_UPLINK_MODE','MODEL_ID','MODEL_BARGE_ANY','MODEL_GOODBYE',
 'XZ_BARGE_ANY','XZ_GOODBYE_TEXT','XZ_INPUT_GAIN_DB','XZ_IDLE_TIMEOUT_MS','XZ_CLIENT_ID','XZ_DEVICE_ID',
 'DB_BARGE_ANY','DB_GOODBYE','DB_IDLE_SECONDS','DB_API_KEY']
with open(os.environ['FIXTURE']+'/calls','a') as f:f.write(json.dumps({'args':sys.argv[1:],'env':{k:os.environ.get(k) for k in names}})+'\\n')
''');self.core.chmod(0o700)
        self.xa=self.p/'xa';self.xa.mkdir()
        supervisor=self.xa/'session-run';supervisor.write_text('#!/bin/sh\n[ "$1" = run ] || exit 2\nshift 2\nexec "$@"\n');supervisor.chmod(0o700)
        self.mac=self.p/'mac';self.mac.write_text('00:11:22:33:44:55\n')
        self.bases={name:self.p/name for name in ('xiaozhi','doubao','accounts')}
        self.bases['xiaozhi'].mkdir()
        self.identity='11111111-2222-3333-4444-555555555555\n'
        (self.bases['xiaozhi']/'client_id').write_text(self.identity)
        run(['python3',ADAPTER/'build_accounts.py',self.uploads])
        run(['python3',ROOT/'runtime/doubao/build_deploy.py','--output',self.uploads/'doubao_watch.sh'])
        run(['python3',ROOT.parent/'webdeploy166/build_xz_deploy.py'],env={**self.env,'XZ_DEPLOY_OUTPUT':str(self.uploads/'xz_watch.sh')})
        for script in self.uploads.glob('*.sh'):
            text=script.read_text()
            for old,new in [('/usr/lib/lx06/script-settings',str(self.settings)),
                ('/usr/bin/lx06-model-tool',str(self.p/'tool')),
                ('/usr/lib/lx06/xiaoai',str(self.xa)),
                ('/usr/bin/lx06-dialogued',str(self.core)),
                ('/usr/bin/lx06-model-http','/missing/http'),
                ('/data/lx06/model-accounts',str(self.bases['accounts'])),
                ('/data/lx06/xiaozhi',str(self.bases['xiaozhi'])),
                ('/data/lx06/doubao',str(self.bases['doubao'])),
                ('/data/lx06/logs',str(self.p/'logs')),
                ('/sys/class/net/wlan0/address',str(self.mac))]:text=text.replace(old,new)
            script.write_text(text);run(['sh','-n',script])

    def save(self,name,**changes):
        path=self.uploads/name;definition=schema(path.read_text())
        keys=next(a['fields'] for a in definition['actions'] if a['id']=='save')
        values={f['name']:f.get('default','') for f in definition['fields'] if f['name'] in keys}
        values.update(changes)
        run([self.settings,'save',path],input=json.dumps({'name':name,'action':'save','values':values}))

    def call(self,name,wake='测试唤醒'):
        run(['sh',self.uploads/name,'--session-worker',wake],env=self.env)
        return json.loads((self.p/'calls').read_text().splitlines()[-1])

    def test_independent_upload_orders_and_saved_worker_settings(self):
        # Same source/credential identities must survive both upload orders.
        for order in [('xz_watch.sh','doubao_watch.sh','grok_account.sh','chatgpt_account.sh'),
                      ('chatgpt_account.sh','grok_account.sh','doubao_watch.sh','xz_watch.sh')]:
            for name in order:
                if name=='xz_watch.sh':
                    self.save(name,XZ_BARGE_ANY='1',XZ_INPUT_GAIN_DB='3.25',XZ_MAX_SESSION_SECONDS='431',XZ_IDLE_TIMEOUT_MS='67000',XZ_GOODBYE_TEXT='测试结束')
                    out=self.call(name);self.assertEqual(out['args'],['--audio','xiaozhi','测试唤醒'])
                    for k,v in dict(DC_MAX_SECONDS='431',DC_UPLINK_MODE='native',XZ_BARGE_ANY='1',XZ_INPUT_GAIN_DB='3.25',XZ_IDLE_TIMEOUT_MS='67000',XZ_GOODBYE_TEXT='测试结束',XZ_CLIENT_ID=self.identity.strip(),XZ_DEVICE_ID='00:11:22:33:44:55').items():self.assertEqual(out['env'][k],v,k)
                elif name=='doubao_watch.sh':
                    self.save(name,DB_BARGE_ANY='1',DB_MAX_SECONDS='432',DB_IDLE_SECONDS='68',DB_GOODBYE='测试结束',DB_API_KEY='synthetic-fixture-key')
                    out=self.call(name);self.assertEqual(out['args'],['--audio','doubao','测试唤醒'])
                    for k,v in dict(DC_MAX_SECONDS='432',DC_UPLINK_MODE='native',DB_BARGE_ANY='1',DB_IDLE_SECONDS='68',DB_GOODBYE='测试结束',DB_API_KEY='synthetic-fixture-key').items():self.assertEqual(out['env'][k],v,k)
                else:
                    self.save(name,MODEL_BARGE_ANY='1',MODEL_MAX_SECONDS='433',MODEL_GOODBYE='测试结束')
                    out=self.call(name);self.assertEqual(out['args'][0:2],['--account',str(self.uploads/name)])
                    self.assertEqual(out['args'][3],'测试唤醒')
                    self.assertEqual(out['env']['DC_MAX_SECONDS'],'433');self.assertEqual(out['env']['MODEL_BARGE_ANY'],'1');self.assertEqual(out['env']['MODEL_GOODBYE'],'测试结束')
        self.assertEqual((self.bases['xiaozhi']/'client_id').read_text(),self.identity)
        self.assertFalse((self.bases['xiaozhi']/'xz_client_armhf').exists())
        self.assertFalse((self.bases['doubao']/'doubao-client').exists())
        self.assertFalse(list(self.p.glob('lx06-account-mic.*')))

    def test_guarded_mapping_and_no_retired_hardware_fallback(self):
        for name,key in [('xz_watch.sh','XZ_BARGE_ANY'),('doubao_watch.sh','DB_BARGE_ANY')]:
            self.save(name,**{key:'0'})
            self.assertEqual(self.call(name)['env']['DC_UPLINK_MODE'],'guarded')
        for path in self.uploads.glob('*.sh'):
            text=path.read_text()
            self.assertNotIn('processed-mic-read',text);self.assertNotIn('/usr/bin/aplay',text)
        self.core.unlink()
        for name in ['xz_watch.sh','doubao_watch.sh','chatgpt_account.sh','grok_account.sh']:
            before=(self.p/'calls').read_bytes()
            result=subprocess.run(['sh',self.uploads/name,'--session-worker','test'],env=self.env,capture_output=True,timeout=5)
            self.assertIn(result.returncode,(0,2));self.assertEqual((self.p/'calls').read_bytes(),before)

    def test_doubao_extension_uses_network_only_firmware_probe(self):
        probe=self.p/'transport';probe.write_text('''#!/bin/sh
if [ "$1" = --protocol ]; then echo LX06DCTRANSPORT/1; exit 0; fi
[ "$1|$2|$3" = '--check|--provider|doubao' ] || exit 2
printf '%s' "$DB_API_KEY" > "$FIXTURE/probe-key"
''');probe.chmod(0o700)
        self.save('doubao_watch.sh',DB_API_KEY='synthetic-fixture-key')
        result=run(['sh',self.uploads/'doubao_watch.sh','--web-action','check'],env={**self.env,'DC_TRANSPORT_ADAPTER':str(probe)})
        response=json.loads(result.stdout);self.assertEqual(response['step'],'configure')
        self.assertNotIn('synthetic-fixture-key',result.stdout)
        self.assertEqual((self.p/'probe-key').read_text(),'synthetic-fixture-key')
        self.assertFalse((self.p/'calls').exists())

if __name__=='__main__':unittest.main()

"""Exercise real persistence and exec, not copies of configuration logic."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from voice_settings import integrate, schema

ROOT=Path(__file__).resolve().parents[2]

class SettingsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.p=Path(cls.tmp.name)
        cls.binary=cls.p/'settings';cls.store=cls.p/'store'
        subprocess.run(['gcc','-O2',f'-DLX06_SETTINGS_DIR="{cls.store}"',str(ROOT/'runtime/model-adapter/script-settings.c'),'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def script(self,name,provider):
        p=self.p/name
        # Exercise the same action contract as uploaded voice scripts. The
        # Doubao setup flow upgrades this block to verify credentials first.
        base='''#!/bin/sh
# 小爱通道唤醒词=测试出来
SETTINGS=@@SETTINGS_HELPER@@
if [ "${1:-}" = --web-action ]; then
  case "${2:-}" in
    load) exec "$SETTINGS" show "$0";;
    save) exec "$SETTINGS" save "$0";;
  esac
fi
exit 0
'''
        p.write_text(integrate(base,provider))
        p.chmod(0o600);return p

    def call(self,cmd,p,values=None,env=None,check=True,extra=()):
        request=None if values is None else json.dumps(dict(name=p.name,action='save',values=values),ensure_ascii=False)
        r=subprocess.run([str(self.binary),cmd,str(p),*extra],input=request,text=True,capture_output=True,env=dict(os.environ,**(env or {})))
        if check:self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return r

    def test_defaults_secret_save_reload_upgrade_and_isolation(self):
        for first,second in [('doubao','xiaozhi'),('xiaozhi','doubao')]:
            for provider in (first,second):
                p=self.script(first+'-'+provider+'.sh',provider)
                secret='DB_API_KEY' if provider=='doubao' else 'XZ_ACCESS_TOKEN'
                prefix='DB' if provider=='doubao' else 'XZ'
                env={secret:'private-example-key'}
                current=json.loads(self.call('show',p,env=env).stdout)['values']
                self.assertNotIn(secret,current)
                self.assertEqual(current['official_wake'],'测试出来')
                current[secret]='';current[prefix+'_BARGE_ANY']='1'
                current['official_wake']='测试一,测试二'
                if provider=='doubao':current['DB_INSTRUCTIONS']='两行\n$(touch /tmp/NEVER) `false`'
                r=self.call('save',p,current,env)
                self.assertNotIn('private-example-key',r.stdout)
                persisted=json.loads((self.store/(p.name+'.json')).read_text())
                self.assertEqual(persisted[secret],env[secret])
                self.assertEqual((self.store/(p.name+'.json')).stat().st_mode&0o777,0o600)
                self.script(p.name,provider) # uploading newer script retains DATA
                got=json.loads(self.call('show',p).stdout)['values']
                self.assertEqual(got[prefix+'_BARGE_ANY'],'1')
                self.assertEqual(got['official_wake'],'测试一,测试二')
                captured=self.call('run',p,extra=['/usr/bin/python3','-c','import os,json;print(json.dumps(dict(os.environ)))'])
                process=json.loads(captured.stdout)
                self.assertEqual(process[secret],env[secret]);self.assertEqual(process[prefix+'_BARGE_ANY'],'1')
                self.assertNotIn('official_wake',process)
                got[secret]='';got[prefix+'_BARGE_ANY']='0'
                self.call('save',p,got)
                self.assertEqual(json.loads((self.store/(p.name+'.json')).read_text())[secret],env[secret])

    def test_invalid_request_keeps_previous_settings(self):
        p=self.script('invalid.sh','doubao');v=json.loads(self.call('show',p).stdout)['values'];v['DB_API_KEY']='test'
        self.call('save',p,v);file=self.store/(p.name+'.json');before=file.read_bytes()
        for key,value in [('DB_IDLE_SECONDS','0'),('DB_MAX_SECONDS','86401'),('DB_BARGE_ANY','2'),
                          ('DB_IDLE_SECONDS','1.5'),('DB_ENDPOINT','http://bad'),('DB_INTERRUPT',','),
                          ('DB_INTERRUPT',''),('DB_GOODBYE','a\x00b'),('DB_API_KEY','x'*2049),
                          ('DB_API_KEY','key\r\nHeader: x'),('DB_GREETING','你'*151),('DB_VOICE','x'*101)]:
            bad=dict(v);bad[key]=value
            self.assertNotEqual(self.call('save',p,bad,check=False).returncode,0,(key,value))
            self.assertEqual(file.read_bytes(),before)

    def test_untrusted_or_corrupt_store_cannot_start_client(self):
        p=self.script('broken.sh','xiaozhi');v=json.loads(self.call('show',p).stdout)['values'];v['XZ_ACCESS_TOKEN']=''
        self.call('save',p,v);file=self.store/(p.name+'.json');good=file.read_bytes()
        for text in ['not json','{"XZ_BARGE_ANY":"0","XZ_BARGE_ANY":"1"}','{"XZ_BARGE_ANY":false}']:
            file.write_text(text)
            self.assertNotEqual(self.call('run',p,extra=['true'],check=False).returncode,0)
        file.write_bytes(good);file.chmod(0o644)
        self.assertNotEqual(self.call('show',p,check=False).returncode,0)
        file.chmod(0o600);file.unlink();target=self.p/'external';target.write_bytes(good);target.chmod(0o600);file.symlink_to(target)
        self.assertNotEqual(self.call('show',p,check=False).returncode,0)

    def test_schema_current_script_parameters_and_limits(self):
        for provider in ['xiaozhi','doubao']:
            p=self.script('inventory-'+provider+'.sh',provider)
            j=json.loads(self.call('show',p).stdout)
            self.assertGreater(len(j['values']),8)
            self.assertTrue(all(f['name'] in j['values'] for f in schema(provider)['fields'] if f['type']!='password'))

    def test_xiaozhi_decimal_gain_persists_and_reaches_client_environment(self):
        p=self.script('gain.sh','xiaozhi')
        current=json.loads(self.call('show',p).stdout)['values']
        current['XZ_ACCESS_TOKEN']=''
        self.assertEqual(current['XZ_INPUT_GAIN_DB'],'0.00')
        for value in ['-12.00','-3.25','0.00','3.25','12.00']:
            current['XZ_INPUT_GAIN_DB']=value
            self.call('save',p,current)
            self.assertEqual(json.loads(self.call('show',p).stdout)['values']['XZ_INPUT_GAIN_DB'],value)
            r=self.call('run',p,extra=['/usr/bin/python3','-c','import os;print(os.environ["XZ_INPUT_GAIN_DB"])'])
            self.assertEqual(r.stdout.strip(),value)
        for bad in ['12.01','-12.01','1.001','nan','','2;id']:
            current['XZ_INPUT_GAIN_DB']=bad
            self.assertNotEqual(self.call('save',p,current,check=False).returncode,0)
            self.assertEqual(json.loads(self.call('show',p).stdout)['values']['XZ_INPUT_GAIN_DB'],'12.00')

    def test_forget_removes_only_own_settings_and_is_idempotent(self):
        a=self.script('remove-a.sh','doubao');b=self.script('remove-b.sh','xiaozhi')
        for p,secret in [(a,'DB_API_KEY'),(b,'XZ_ACCESS_TOKEN')]:
            v=json.loads(self.call('show',p).stdout)['values'];v[secret]='private'
            self.call('save',p,v)
        self.call('forget',a);self.call('forget',a)
        self.assertFalse((self.store/(a.name+'.json')).exists())
        self.assertTrue((self.store/(b.name+'.json')).exists())

if __name__=='__main__':unittest.main()

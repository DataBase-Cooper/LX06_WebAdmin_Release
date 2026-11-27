"""Offline RED: real session-run -> production voice_worker -> account core seam.

Run in an isolated Linux mount+network namespace with private tmpfs /tmp and
TMPDIR; only synthetic scripts, no actual providers or devices. The core probe
includes production configure_account and invokes the real account runner, not
production main's hardware reactor. Each subtest atomically replaces its upload
after configuration, before model request. Current legacy is tested separately.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

class SupervisorBinding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.readlink('/proc/self/ns/net') == os.readlink('/proc/1/ns/net'):
            raise RuntimeError('requires a private network namespace')
        cls.tmp = tempfile.TemporaryDirectory(prefix='account-supervisor-')
        cls.dir = Path(cls.tmp.name)
        cls.supervisor = cls.dir/'session-run'
        cls.probe = cls.dir/'core-probe'
        def build(args): subprocess.run(list(map(str,args)), check=True)
        build(['gcc','-O2','-Wall','-Wextra','-Werror',
               '-DLX06_SESSION_FILE="'+str(cls.dir/'lease')+'"',
               ROOT/'hooks/xiaoai/session-run.c','-o',cls.supervisor])
        build(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
               '-ffunction-sections','-fdata-sections',HERE/'test_supervisor_account_binding_probe.c',
               HERE.parent/'account.c',HERE.parent/'session.c',HERE.parent/'control.c',
               ROOT/'runtime/doubao/src/text_match.c','-Wl,--gc-sections','-lm','-o',cls.probe])
        source=(ROOT/'runtime/model-adapter/account-voice.sh').read_text()
        cls.worker=source[source.index('voice_http_start() {'):source.index('voice_warmup() {')]

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def scenario(self, declaration, override):
        script=self.dir/'upload.sh'; replacement=self.dir/'new.sh'
        header='#!/bin/sh\n'
        if declaration:
            header += '# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: '+declaration+'\n'
        script.write_text(header + '''
MODEL_ID=synthetic
case "$1" in
 --model-request) printf '%s\\n' '{"delta":"original"}' '{"done":true}'; exit 0;;
esac
''' + self.worker + '\nvoice_worker "${2:-}"\n')
        replacement.write_text('''#!/bin/sh
printf '%s\\n' '{"delta":"replacement"}' '{"done":true}'
''')
        gate=self.dir/'gate'
        gate.unlink(missing_ok=True)
        env={'PATH':'/usr/bin:/bin','HOME':str(self.dir),'TMPDIR':str(self.dir),
             'LX06_DIALOGUED':str(self.probe),'MODEL_BARGE_ANY':str(override),
             'FIXTURE_GATE':str(gate)}
        proc=subprocess.Popen([self.supervisor,'run','synthetic','sh',script,'--session-worker','wake'],
                              stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                              env=env,text=True)
        try:
            mode=proc.stdout.readline().strip()
            self.assertTrue(mode.startswith('MODE '),mode)
            os.replace(replacement,script)
            gate.touch()
            out,err=proc.communicate(timeout=8)
            self.assertEqual(proc.returncode,0,(out,err))
            return int(mode.split()[1]),out
        finally:
            if proc.poll() is None: proc.kill();proc.communicate(timeout=5)

    def test_declared_identity_binds_mode_and_original_bytes(self):
        for declared,expected in [('keyword',0),('voice',1)]:
            for override in (0,1):
                with self.subTest(declared=declared,override=override):
                    mode,out=self.scenario(declared,override)
                    self.assertEqual((mode,'DELTA original' in out),(expected,True),
                                     f'expected declaration and original bytes; got mode={mode}, output={out!r}')

    def test_current_undeclared_legacy_is_independent(self):
        for override in (0,1):
            with self.subTest(override=override):
                mode,out=self.scenario(None,override)
                self.assertEqual(mode,override)
                self.assertIn('DELTA replacement',out)

if __name__=='__main__': unittest.main(verbosity=2)

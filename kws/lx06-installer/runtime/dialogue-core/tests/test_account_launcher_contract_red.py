"""Experimental REDs for a proposed account-launch contract; Linux host only.

Provisional names DC_ACCOUNT_IDENTITY_VERSION and DC_ACCOUNT_EPOCH are test
requirements, not an implemented or approved interface. Run with a freshly
built DC_CORE_HOST_FIXTURE binary in DC_TEST_CORE. Fixtures use generated
uploads and synthetic files only; no network, device, or real accounts.
"""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GENERATOR = ROOT / 'runtime/model-adapter/build_accounts.py'


class AccountLauncherContract(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='account-launch-red-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def test_generated_worker_supplies_sealed_exact_bytes_and_epoch(self):
        uploads = self.root / 'uploads'
        subprocess.run(['python3', str(GENERATOR), str(uploads)], check=True)
        script = uploads / 'grok_account.sh'
        expected = script.read_bytes()
        probe = self.root / 'dialogued'
        probe.write_text('''#!/usr/bin/python3
import fcntl,json,os,sys
if sys.argv[1:] == ['--protocol']:
 print('LX06DIALOGUE/1'); sys.exit(0)
r={'argv':sys.argv[1:], 'epoch':os.getenv('DC_ACCOUNT_EPOCH'), 'fd':os.getenv('DC_ACCOUNT_SEALED_FD')}
try:
 fd=int(r['fd']); r['seals']=fcntl.fcntl(fd,fcntl.F_GET_SEALS)
 r['bytes']=os.pread(fd,1048577,0).hex()
except (ValueError,TypeError,OSError): pass
with open(os.environ['PROBE_RESULT'],'w') as f: json.dump(r,f)
''')
        probe.chmod(0o700)
        env = dict(os.environ, LX06_DIALOGUED=str(probe), MODEL_ID='synthetic',
                   DC_ACCOUNT_IDENTITY_VERSION='1', PROBE_RESULT=str(self.root/'result'))
        subprocess.run(['sh', str(script), '--configured', '--session-worker', 'wake'],
                       env=env, timeout=10, capture_output=True, check=True)
        result = json.loads((self.root/'result').read_text())
        self.assertIsNotNone(result['fd'], 'opted-in generated worker did not supply a descriptor')
        self.assertEqual(bytes.fromhex(result['bytes']), expected)
        required = fcntl.F_SEAL_SEAL | fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK
        self.assertEqual(result['seals'] & required, required)
        self.assertTrue(result['epoch'] and result['epoch'].isdigit() and int(result['epoch']) > 0)

    def test_generated_worker_rejects_unknown_version_without_starting_controller(self):
        uploads = self.root / 'uploads'
        subprocess.run(['python3', str(GENERATOR), str(uploads)], check=True)
        probe = self.root / 'dialogued'
        probe.write_text('''#!/bin/sh
if [ "$1" = --protocol ]; then echo LX06DIALOGUE/1; exit 0; fi
printf called > "$PROBE_RESULT"
''')
        probe.chmod(0o700)
        env = dict(os.environ, LX06_DIALOGUED=str(probe), MODEL_ID='synthetic',
                   DC_ACCOUNT_IDENTITY_VERSION='2', PROBE_RESULT=str(self.root/'called'))
        subprocess.run(['sh', str(uploads/'grok_account.sh'), '--configured', '--session-worker', 'wake'],
                       env=env, timeout=10, capture_output=True, check=True)
        self.assertFalse((self.root/'called').exists(), 'unknown version reached controller')

    def test_real_main_rejects_missing_or_invalid_explicit_epoch(self):
        binary = os.environ.get('DC_TEST_CORE')
        if not binary:
            self.fail('DC_TEST_CORE must name a fresh host-fixture build of production main')
        fd = os.memfd_create('synthetic-account', os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, fd)
        os.write(fd, b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\nexit 0\n')
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SEAL | fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK)
        for epoch in (None, '0', 'bad', '18446744073709551616'):
            with self.subTest(epoch=epoch):
                env = dict(os.environ, DC_HOST_FIXTURE='1', DC_TRACE_STARTUP='1',
                           DC_ACCOUNT_IDENTITY_VERSION='1', DC_ACCOUNT_SEALED_FD=str(fd),
                           LX06_CONTROL_ASR_SOCKET=str(self.root/'missing-asr'),
                           DC_MIC_DIRECTORY=str(self.root/'missing-mic'))
                env.pop('DC_ACCOUNT_EPOCH', None)
                if epoch is not None: env['DC_ACCOUNT_EPOCH'] = epoch
                proc = subprocess.run([binary, '--account', str(self.root/'script'), 'synthetic'],
                                      env=env, pass_fds=(fd,), capture_output=True, timeout=5)
                self.assertEqual(proc.returncode, 2, proc.stderr.decode(errors='replace'))
                self.assertNotIn(b'startup_stage=4', proc.stderr)

    def test_real_main_rejects_unknown_contract_version(self):
        binary = os.environ.get('DC_TEST_CORE')
        if not binary: self.fail('DC_TEST_CORE must name a host-fixture production main')
        fd = os.memfd_create('synthetic-account', os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close, fd)
        os.write(fd, b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\nexit 0\n')
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_SEAL | fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK)
        env = dict(os.environ, DC_HOST_FIXTURE='1', DC_TRACE_STARTUP='1',
                   DC_ACCOUNT_IDENTITY_VERSION='2', DC_ACCOUNT_EPOCH='1234',
                   DC_ACCOUNT_SEALED_FD=str(fd), LX06_CONTROL_ASR_SOCKET=str(self.root/'missing'),
                   DC_MIC_DIRECTORY=str(self.root/'missing-mic'))
        proc = subprocess.run([binary, '--account', str(self.root/'script'), 'synthetic'], env=env,
                              pass_fds=(fd,), capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 2, proc.stderr.decode(errors='replace'))
        self.assertNotIn(b'startup_stage=4', proc.stderr)


if __name__ == '__main__': unittest.main()

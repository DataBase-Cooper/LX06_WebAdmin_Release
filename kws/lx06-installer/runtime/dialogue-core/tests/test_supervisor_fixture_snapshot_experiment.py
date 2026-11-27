"""Offline prototype: an external fixture supplies sealed script to real supervisor.
NOT an authenticated supervisor launcher: no production changes, only synthetic data.
"""
import fcntl, os, subprocess, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_supervisor_account_binding_red as baseline

class SnapshotVertical(baseline.SupervisorBinding):
    def test_declared_identity_binds_mode_and_original_bytes(self):
        for declaration, expected in [('keyword',0),('voice',1)]:
            for override in (0,1):
                with self.subTest(mode=declaration,override=override):
                    self.check_snapshot(declaration,expected,override)
    def test_current_undeclared_legacy_is_independent(self):
        super().test_current_undeclared_legacy_is_independent()
    def check_snapshot(self,declaration,expected,override):
        path=self.dir/'upload.sh'; repl=self.dir/'new.sh'; gate=self.dir/'gate'; gate.unlink(missing_ok=True)
        data=('#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: '+declaration+'\n'+'''MODEL_ID=synthetic
case "$1" in
 --model-request) exec sh "$0" --inner;;
 --inner) if ! grep -q 'LX06-INTERRUPTION' "$0"; then exit 88; fi; printf '%s\\n' '{"delta":"original"}' '{"done":true}'; exit 0;;
esac
'''+self.worker+'\nvoice_worker "${2:-}"\n').encode()
        path.write_bytes(data); repl.write_text('#!/bin/sh\nprintf \'%s\\n\' \'{"delta":"replacement"}\' \'{"done":true}\'\n')
        fd=os.memfd_create('synthetic-account',os.MFD_ALLOW_SEALING)
        self.addCleanup(os.close,fd)
        os.write(fd,data)
        required=fcntl.F_SEAL_WRITE|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_GROW|fcntl.F_SEAL_SEAL
        fcntl.fcntl(fd,fcntl.F_ADD_SEALS,required)
        self.assertEqual(fcntl.fcntl(fd,fcntl.F_GET_SEALS)&required,required)
        env={'PATH':'/usr/bin:/bin','HOME':str(self.dir),'TMPDIR':str(self.dir), 'LX06_DIALOGUED':str(self.probe),'MODEL_BARGE_ANY':str(override),'FIXTURE_GATE':str(gate),'DC_ACCOUNT_SEALED_FD':str(fd)}
        p=subprocess.Popen([self.supervisor,'run','synthetic','sh',f'/proc/self/fd/{fd}','--session-worker','wake'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,text=True,pass_fds=(fd,))
        try:
            line=p.stdout.readline().strip(); self.assertEqual(line,'MODE '+str(expected),line)
            lease=os.open(self.dir/'lease',os.O_RDWR)
            try:
                with self.assertRaises(BlockingIOError): fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
                os.replace(repl,path); gate.touch(); out,err=p.communicate(timeout=8)
                self.assertEqual(p.returncode,0,(out,err)); self.assertIn('DELTA original',out); self.assertNotIn('replacement',out)
                fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB); self.assertEqual(os.fstat(lease).st_size,0)
            finally: os.close(lease)
        finally:
            if p.poll() is None: p.kill();p.communicate(timeout=5)

if __name__=='__main__': unittest.main(verbosity=2)

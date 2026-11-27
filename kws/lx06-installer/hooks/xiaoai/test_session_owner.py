"""Scope and handoff checks with real Linux processes, before device deployment."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
import unittest
import ctypes

ROOT=Path(__file__).resolve().parent
class SessionOwnerTest(unittest.TestCase):
    def test_memory_pressure_rejects_or_reaps_only_owned_group(self):
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder);exe=base/'run';owner=base/'owner';memory=base/'meminfo'
            subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',
                '-DLX06_SESSION_FILE="'+str(owner)+'"','-DLX06_MEMINFO_FILE="'+str(memory)+'"',
                str(ROOT/'session-run.c'),'-o',str(exe)],check=True)
            unrelated=subprocess.Popen(['sleep','30']);runner=None
            try:
                for text in ('MemAvailable: 19000 kB\n','invalid\n'):
                    memory.write_text(text)
                    refused=subprocess.run([str(exe),'run','low','touch',str(base/'unexpected')],capture_output=True)
                    self.assertEqual(refused.returncode,125)
                    self.assertFalse((base/'unexpected').exists())
                memory.write_text('MemFree: 16000 kB\nMemAvailable: 32000 kB\n')
                runner=subprocess.Popen([str(exe),'run','pressure','sh','-c','trap "" TERM; sleep 30 & wait'],stderr=subprocess.PIPE)
                deadline=time.monotonic()+2
                while not owner.exists() or owner.stat().st_size!=96:
                    self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                rec=struct.unpack('=IIIIQQ',owner.read_bytes()[:32]);group=rec[2]
                memory.write_text('MemFree: 6000 kB\nMemAvailable: 10000 kB\n')
                self.assertEqual(runner.wait(2),125)
                self.assertIn(b'memory pressure',runner.stderr.read())
                with self.assertRaises(ProcessLookupError):os.killpg(group,0)
                self.assertIsNone(unrelated.poll())
                self.assertEqual(subprocess.run([str(exe),'busy']).returncode,1)
                memory.write_text('MemFree: 16000 kB\nMemAvailable: 32000 kB\n')
                self.assertEqual(subprocess.run([str(exe),'run','recovered','true']).returncode,0)
            finally:
                unrelated.terminate();unrelated.wait()
                if runner is not None:
                    if runner.poll() is None:runner.terminate();runner.wait(3)
                    runner.stderr.close()

    def test_supervisor_and_leader_killed_lease_reaps_orphan(self):
        # Adopt orphan descendants in this test process so zombies cannot make
        # a dead group appear alive under a minimal container PID 1.
        self.assertEqual(ctypes.CDLL(None).prctl(36,1,0,0,0),0)
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder); exe=base/'run'; owner=base/'owner'; childfile=base/'child'
            subprocess.run(['gcc','-O2','-DLX06_SESSION_FILE="'+str(owner)+'"',str(ROOT/'session-run.c'),'-o',str(exe)],check=True)
            script='trap "" TERM; sleep 30 & echo $! > "$1"; wait'
            runner=subprocess.Popen([str(exe),'run','crash','sh','-c',script,'sh',str(childfile)],stderr=subprocess.PIPE)
            group=None; orphan=None
            try:
                deadline=time.monotonic()+2
                while not childfile.exists() or not childfile.read_text().strip():
                    self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                rec=struct.unpack('=IIIIQQ',owner.read_bytes()[:32]); group=rec[2]
                orphan=int(childfile.read_text())
                runner.kill();runner.wait(2)
                os.kill(group,signal.SIGKILL)
                deadline=time.monotonic()+2
                while Path(f'/proc/{group}').exists():
                    try: os.waitpid(group,os.WNOHANG)
                    except ChildProcessError: pass
                    self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                self.assertEqual(subprocess.run([str(exe),'busy']).returncode,0)
                self.assertEqual(subprocess.run([str(exe),'run','other','true']).returncode,75)
                self.assertEqual(subprocess.run([str(exe),'preempt']).returncode,0)
                os.waitpid(orphan,0); orphan=None
                self.assertEqual(subprocess.run([str(exe),'busy']).returncode,1)
            finally:
                if runner.poll() is None: runner.kill();runner.wait()
                runner.stderr.close()
                for pid in (group,orphan):
                    if pid:
                        try: os.kill(pid,signal.SIGKILL)
                        except ProcessLookupError: pass
                        try: os.waitpid(pid,0)
                        except ChildProcessError: pass

    def test_exclusion_preemption_and_exit_status(self):
        with tempfile.TemporaryDirectory() as folder:
            base=Path(folder); exe=base/'run'; owner=base/'owner'
            subprocess.run(['gcc','-O2','-Wall','-Wextra','-DLX06_SESSION_FILE="'+str(owner)+'"',str(ROOT/'session-run.c'),'-o',str(exe)],check=True)
            unrelated=subprocess.Popen(['sleep','30'])
            try:
                for sig in [signal.SIGUSR1,signal.SIGTERM]:
                    runner=subprocess.Popen([str(exe),'run','test','sh','-c','sleep 30 & wait'],stderr=subprocess.PIPE)
                    deadline=time.monotonic()+2
                    while not owner.exists() or owner.stat().st_size!=96:
                        self.assertLess(time.monotonic(),deadline);time.sleep(.01)
                    rec=struct.unpack('=IIIIQQ',owner.read_bytes()[:32]);group=rec[2]
                    self.assertEqual(owner.read_bytes()[32:],b'test'+bytes(60))
                    self.assertEqual(subprocess.run([str(exe),'run','other','true']).returncode,75)
                    self.assertEqual(subprocess.run([str(exe),'busy']).returncode,0)
                    start=time.monotonic(); runner.send_signal(sig)
                    self.assertEqual(runner.wait(2),0,runner.stderr.read())
                    runner.stderr.close()
                    self.assertLess(time.monotonic()-start,1)
                    with self.assertRaises(ProcessLookupError): os.killpg(group,0)
                    self.assertEqual(subprocess.run([str(exe),'busy']).returncode,1)
                    self.assertIsNone(unrelated.poll())
                self.assertEqual(subprocess.run([str(exe),'run','bad','sh','-c','exit 7']).returncode,7)
                expired=subprocess.run([str(exe),'run','timeout','sh','-c','trap "" TERM; sleep 30 & wait'],
                                       env=dict(os.environ,LX06_SESSION_MAX_SECONDS='1'),timeout=3)
                self.assertEqual(expired.returncode,124)
                self.assertEqual(subprocess.run([str(exe),'busy']).returncode,1)
                # A reused/unrelated PID with mismatched birth ticks must never
                # be signalled by a stale metadata record.
                owner.write_bytes(struct.pack('=IIIIQQ',0x3153584c,unrelated.pid,unrelated.pid,0,1,1))
                self.assertEqual(subprocess.run([str(exe),'preempt']).returncode,0)
                self.assertIsNone(unrelated.poll())
                for name in ('', 'x'*64, 'bad/name', 'bad name'):
                    self.assertEqual(subprocess.run([str(exe),'run',name,'true']).returncode,2)
            finally: unrelated.terminate();unrelated.wait()

if __name__=='__main__': unittest.main()

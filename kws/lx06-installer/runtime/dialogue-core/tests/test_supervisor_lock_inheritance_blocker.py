"""Offline counterexample: a script can release the supervisor's own lease.

Run only in a private network namespace with a private POSIX temporary directory.
This exercises the unmodified session-run with a synthetic child and lease; it
is not a capture implementation or an assertion about a device kernel.
"""
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class LeaseInheritanceBlocker(unittest.TestCase):
    def test_direct_shell_child_can_unlock_supervisor_owned_lease(self):
        self.assertNotEqual(os.readlink('/proc/self/ns/net'), os.readlink('/proc/1/ns/net'),
                            'requires private network namespace')
        with tempfile.TemporaryDirectory(prefix='lease-blocker-') as tmp:
            d = Path(tmp)
            lease = d / 'lease'
            binary = d / 'session-run'
            subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-DLX06_SESSION_FILE="' + str(lease) + '"',
                            str(ROOT / 'hooks/xiaoai/session-run.c'), '-o', str(binary)], check=True)
            script = d / 'child.sh'
            script.write_text('''#!/bin/sh
for f in /proc/self/fd/*; do
  [ "$(readlink "$f")" = "$LEASE" ] || continue
  fd=${f##*/}
  printf 'CHILD %s %s\\n' "$$" "$fd"
  read -r command || exit 1
  [ "$command" = unlock ] || exit 2
  flock -u "$fd" || exit 3
  printf 'UNLOCKED\\n'
  read -r command || exit 4
  [ "$command" = finish ] || exit 5
  exit 0
done
exit 6
''')
            env = {'PATH': '/usr/bin:/bin', 'HOME': tmp, 'TMPDIR': tmp, 'LEASE': str(lease)}
            p = subprocess.Popen([str(binary), 'run', 'synthetic', 'sh', str(script)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, env=env)
            try:
                line = p.stdout.readline().strip().split()
                self.assertEqual(line[:1], ['CHILD'], line)
                child, fd = map(int, line[1:])
                self.assertEqual(int(Path(f'/proc/{child}/status').read_text().split('PPid:\t')[1].splitlines()[0]), p.pid)
                self.assertEqual(os.stat(f'/proc/{child}/fd/{fd}').st_ino, os.stat(lease).st_ino)
                contender = os.open(lease, os.O_RDWR)
                try:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    p.stdin.write('unlock\n'); p.stdin.flush()
                    self.assertEqual(p.stdout.readline().strip(), 'UNLOCKED')
                    self.assertIsNone(p.poll(), 'supervisor exited before contender acquired lease')
                    fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    self.assertTrue(Path(f'/proc/{child}').exists())
                    fcntl.flock(contender, fcntl.LOCK_UN)
                finally:
                    os.close(contender)
                p.stdin.write('finish\n'); p.stdin.flush()
                _, err = p.communicate(timeout=8)
                self.assertEqual(p.returncode, 0, err)
            finally:
                if p.poll() is None:
                    p.kill()
                    p.communicate(timeout=8)


if __name__ == '__main__':
    unittest.main(verbosity=2)

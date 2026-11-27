"""Offline Linux feasibility probe, NOT integrated into the account runner.

A sealed anonymous snapshot can feed /bin/sh while preserving selected shell
observables. This is not full shell-script compatibility or device acceptance.
"""
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SnapshotSeamTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dc-snapshot-seam-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.script = self.root / 'adapter.sh'
        self.resource = self.root / 'resource'
        self.resource.write_text('sibling')

    def snapshot(self, content):
        fd = os.memfd_create('synthetic-account', os.MFD_ALLOW_SEALING | os.MFD_CLOEXEC)
        self.addCleanup(os.close, fd)
        os.write(fd, content)
        fcntl.fcntl(fd, fcntl.F_ADD_SEALS, fcntl.F_SEAL_WRITE | fcntl.F_SEAL_GROW |
                    fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_SEAL)
        return fd

    def run_snapshot(self, fd):
        os.lseek(fd, 0, os.SEEK_SET)
        return subprocess.run(['/bin/sh', '-c', '. /proc/self/fd/' + str(fd),
                               str(self.script), '--model-request'],
                              cwd=self.root, pass_fds=(fd,), capture_output=True,
                              text=True, check=True).stdout

    def test_snapshot_pins_body_and_preserves_basic_path_mode_contract(self):
        original = b'''#!/bin/sh
# LX06-ACCOUNT-IDENTITY: v1
# LX06-INTERRUPTION: keyword
[ "$0" = "$PWD/adapter.sh" ] || exit 90
[ "$1" = --model-request ] || exit 91
[ "$(cat "$(dirname "$0")/resource")" = sibling ] || exit 92
printf 'original:%s:%s\\n' "$0" "$1"
'''
        self.script.write_bytes(original)
        fd = self.snapshot(original)
        expected = f'original:{self.script}:--model-request\n'
        self.assertEqual(self.run_snapshot(fd), expected)
        self.script.write_bytes(b'#!/bin/sh\nprintf replacement\\n\n')
        self.assertEqual(self.run_snapshot(fd), expected)
        with self.assertRaises(OSError):
            os.pwrite(fd, b'X', 0)
        replacement = self.root / 'new.sh'
        replacement.write_text('#!/bin/sh\nprintf replacement\\n\n')
        replacement.replace(self.script)
        self.assertEqual(self.run_snapshot(fd), expected)

    def test_snapshot_does_not_preserve_reopening_dollar_zero(self):
        original = b'''#!/bin/sh
case "$(cat "$0")" in *ORIGINAL_MARKER*) printf original;; *) printf changed;; esac
# ORIGINAL_MARKER
'''
        self.script.write_bytes(original)
        fd = self.snapshot(original)
        self.assertEqual(self.run_snapshot(fd), 'original')
        self.script.write_text('# REPLACED_MARKER\n')
        # Demonstrates why this seam cannot be transparently substituted for
        # all existing scripts, even though the interpreted body is pinned.
        self.assertEqual(self.run_snapshot(fd), 'changed')


if __name__ == '__main__':
    unittest.main()

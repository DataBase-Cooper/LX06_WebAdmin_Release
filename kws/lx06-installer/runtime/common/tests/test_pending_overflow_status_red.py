"""Expected-RED offline check: eviction must not be reported as feed success.

Runs the actual two translation units without sockets/devices. The test state is
placed in the existing post-FINAL restart delay, and optional QUESTION bank OOM
is injected at malloc. Requires Linux, gcc, libopus headers/library, and TMPDIR.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


class PendingOverflowStatus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='pending-status-')
        cls.executables = {}
        for name, source in (
            ('common', ROOT / 'runtime/common/control_asr.c'),
            ('dialogue', ROOT / 'runtime/dialogue-core/asr/control_asr.c'),
        ):
            output = Path(cls.tmp.name) / name
            cmd = ['gcc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                   f'-DCONTROL_ASR_SOURCE="{source}"',
                   str(HERE / 'pending_overflow_status_red.c'),
                   str(ROOT / 'runtime/doubao/src/json.c'),
                   '-Wl,--wrap=malloc', '-lopus', '-o', str(output)]
            subprocess.run(cmd, check=True)
            cls.executables[name] = output

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_eviction_requires_explicit_feed_error(self):
        for implementation, exe in self.executables.items():
            for profile, oom in ((0, 0), (1, 0), (1, 1)):
                with self.subTest(implementation=implementation, profile=profile, oom=oom):
                    env = os.environ.copy()
                    env.pop('LX06_CONTROL_ASR_TRACE', None)
                    p = subprocess.run([str(exe), str(profile), str(oom)],
                                       capture_output=True, text=True, env=env, timeout=5)
                    self.assertNotEqual(p.returncode, 2, p.stderr)
                    self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == '__main__':
    unittest.main()

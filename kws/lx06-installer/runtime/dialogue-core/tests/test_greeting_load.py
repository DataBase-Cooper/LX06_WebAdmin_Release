"""Offline in-memory greeting-load contract; no live cache or provider."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class GreetingLoadTest(unittest.TestCase):
    def test_bounded_identity_and_wav_contract(self):
        with tempfile.TemporaryDirectory() as d:
            exe = pathlib.Path(d) / 'load-test'
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'greeting_identity.c'), str(ROOT / 'greeting_load.c'),
                            str(ROOT / 'tests/test_greeting_load.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()

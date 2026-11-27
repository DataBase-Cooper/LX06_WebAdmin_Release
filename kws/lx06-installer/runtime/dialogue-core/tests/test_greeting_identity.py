"""Offline contract tests for the production greeting identity seam."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class GreetingIdentityTest(unittest.TestCase):
    def test_identity_contract(self):
        source = ROOT / 'greeting_identity.c'
        self.assertTrue(source.is_file(), 'greeting identity validation seam not implemented')
        with tempfile.TemporaryDirectory() as d:
            exe = pathlib.Path(d) / 'identity-test'
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(source), str(ROOT / 'tests' / 'test_greeting_identity.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()

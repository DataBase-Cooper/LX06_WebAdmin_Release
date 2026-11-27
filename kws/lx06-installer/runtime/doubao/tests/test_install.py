"""Exercise interrupted, concurrent and invalid installs with the real shell code."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InstallTests(unittest.TestCase):
    def test_stale_install_recovers_and_bad_payload_preserves_client(self):
        template = (ROOT / 'doubao_watch.sh.tpl').read_text()
        function = template[template.index('ensure_install()'):template.index('\nworker()')]
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            client = base / 'doubao-client'
            client.write_bytes(b'working-old-client')
            client.chmod(0o700)
            work = base / '.install'
            work.mkdir()
            (work / 'client').write_bytes(b'interrupted-partial-client')
            (work / 'client.gz').write_bytes(b'incomplete-gzip')
            payload = b'new-verified-client'
            sha = hashlib.sha256(payload).hexdigest()
            setup = f"BASE='{base}'\nBIN=$BASE/doubao-client\nCLIENT_SHA='{sha}'\n"
            script = base / 'install.sh'
            script.write_text(setup + function.replace('@@PAYLOAD@@', "printf new-verified-client | gzip > \"$WORK/client.gz\"\nsleep .1") + '\nensure_install\n')
            # Concurrent launches serialize and reuse the verified result.
            a = subprocess.Popen(['sh', str(script)])
            b = subprocess.Popen(['sh', str(script)])
            self.assertEqual(a.wait(timeout=5), 0)
            self.assertEqual(b.wait(timeout=5), 0)
            self.assertEqual(client.read_bytes(), payload)
            self.assertFalse(work.exists())
            # Corrupt or mismatched payload never replaces the working binary.
            script.write_text(setup.replace(sha, '0'*64) + function.replace('@@PAYLOAD@@', "printf bad > \"$WORK/client.gz\"") + '\nensure_install\n')
            self.assertNotEqual(subprocess.run(['sh', str(script)], capture_output=True, timeout=5).returncode, 0)
            self.assertEqual(client.read_bytes(), payload)
            self.assertFalse(work.exists())


if __name__ == '__main__':
    unittest.main()

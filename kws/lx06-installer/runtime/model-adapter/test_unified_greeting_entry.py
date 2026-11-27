"""Offline tests of generated unified-account web actions, without device or network."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


class UnifiedGreetingEntry(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        subprocess.run(['python3', str(HERE/'build_accounts.py'), str(self.root)], check=True)
        subprocess.run(['gcc', str(HERE/'model-tool.c'), '-lcrypto', '-o', str(self.root/'tool')], check=True)
        self.exe('settings', '''#!/bin/sh
case "$1" in
 save) echo saved >> "$FIXTURE/events"; exit 0;;
 run) shift 2; export MODEL_GREETING='你好'; exec "$@";;
esac
exit 2
''')
        self.exe('audio', '''#!/bin/sh
 echo "$1" >> "$FIXTURE/audio-events"
 cat >/dev/null
 exit 0
''')
        self.xa = self.root/'xa'
        self.xa.mkdir()
        (self.xa/'session-run').write_text('#!/bin/sh\nexit 1\n')
        (self.xa/'session-run').chmod(0o700)
        for path in self.root.glob('*_account.sh'):
            text = path.read_text()
            for old, new in [('/usr/lib/lx06/script-settings', str(self.root/'settings')),
                             ('/usr/bin/lx06-model-tool', str(self.root/'tool')),
                             ('/usr/bin/lx06-model-audio', str(self.root/'audio')),
                             ('/usr/lib/lx06/xiaoai', str(self.xa))]:
                text = text.replace(old, new)
            path.write_text(text)

    def exe(self, name, content):
        p = self.root/name
        p.write_text(content)
        p.chmod(0o700)

    def action(self, provider, action):
        result = subprocess.run(['sh', str(self.root/(provider+'_account.sh')), '--web-action', action],
                                env={**os.environ, 'FIXTURE': str(self.root)},
                                capture_output=True, text=True, timeout=10)
        return result, json.loads(result.stdout)['message']

    def test_unified_save_and_regenerate_do_not_claim_legacy_cache_is_ready(self):
        for provider in ('chatgpt', 'grok'):
            with self.subTest(provider=provider):
                result, message = self.action(provider, 'save')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('配置已保存', message)
                self.assertIn('未生成', message)
                self.assertIn('不支持', message)
                self.assertFalse((self.root/'audio-events').exists())
        self.assertEqual((self.root/'events').read_text().splitlines(), ['saved', 'saved'])

    def test_extension_description_reports_unsupported(self):
        for provider in ('chatgpt', 'grok'):
            script = (self.root/(provider+'_account.sh')).read_text()
            schema = json.loads(script.split('# LX06-EXTENSION-BEGIN\n# ', 1)[1].split('\n# LX06-EXTENSION-END', 1)[0])
            self.assertIn('暂不支持', schema['description'])
            self.assertNotIn('点击重新生成问候', schema['description'])

    def test_regenerate_reports_unsupported_without_legacy_audio(self):
        for provider in ('chatgpt', 'grok'):
            with self.subTest(provider=provider):
                result, message = self.action(provider, 'regenerate-greeting')
                self.assertNotEqual(result.returncode, 0, message)
                self.assertIn('未生成', message)
                self.assertIn('不支持', message)
                self.assertFalse((self.root/'audio-events').exists())


if __name__ == '__main__':
    unittest.main()

"""Versioned uploads retain client payload/private assignments during UI upgrades."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from voice_settings import integrate


class VoiceSetupFlow(unittest.TestCase):
    def test_repeated_upgrade_replaces_single_declaration_and_keeps_payload(self):
        for provider in ('xiaozhi','doubao'):
            base='''#!/bin/sh
# LX06-RESIDENT: on
# LX06-RUN: root
export %s_BARGE_ANY=1
BASE=/data/fixture
SETTINGS=@@SETTINGS_HELPER@@
'''.replace('%s','XZ' if provider=='xiaozhi' else 'DB')
            if provider=='xiaozhi':base+='publish_bind_code() {\n  say "小智已绑定，无需验证码"\n}\n'
            base+='''if [ "${1:-}" = --web-action ]; then
  case "${2:-}" in
    load) exec "$SETTINGS" show "$0";;
    save) exec "$SETTINGS" save "$0";;
  esac
fi
printf '\\123\\234' >> "$WORK/client.gz"
'''
            once=integrate(base,provider);twice=integrate(once,provider)
            self.assertEqual(once,twice)
            self.assertEqual(twice.count('# LX06-EXTENSION-BEGIN'),1)
            self.assertIn("printf '\\123\\234'",twice)
            schema=json.loads(twice.split('# LX06-EXTENSION-BEGIN\n# ',1)[1].split('\n',1)[0])
            fields = {field['name']: field for field in schema['fields']}
            if provider == 'doubao':
                self.assertEqual(fields['DB_BARGE_ANY']['default'], '1')
            else:
                self.assertNotIn('XZ_BARGE_ANY', fields)
            with tempfile.NamedTemporaryFile('w',suffix='.sh') as f:
                f.write(twice);f.flush();subprocess.run(['sh','-n',f.name],check=True)
            if provider=='xiaozhi':
                self.assertEqual(twice.count('binding_web_event() ('),1)
                self.assertIn('load) home_mcp_options; binding_web_load; exit $?;;',twice)
                self.assertEqual(twice.count('home_mcp_options() {'),1)
                pair=schema['flow']['steps'][0]
                self.assertEqual(pair['fields'],['XZ_ACCESS_TOKEN'])
                self.assertIn('save',pair['actions'])
                for field in schema['fields']:
                    if field['name'] in pair['fields']:
                        self.assertNotIn('group',field)
            else:
                self.assertEqual(twice.count('doubao_setup_event() {'),1)
                self.assertEqual(schema['flow']['steps'][0]['actions'],['verify-key'])
                self.assertEqual(schema['flow']['steps'][1]['actions'],['save'])
                self.assertEqual([(a['id'], a['label']) for a in schema['actions'] if a['id'] != 'load'],
                                 [('save', '保存设置'), ('verify-key', '验证连接')])
                self.assertIn('load) doubao_setup_load; exit $?;;',twice)
                self.assertIn('verify-key|save) doubao_setup_save "$2"; exit $?;;',twice)

    def test_doubao_verifies_before_saving_and_keeps_the_previous_key_on_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            helper = directory / 'helper.sh'
            model_tool = directory / 'model-tool'
            settings = directory / 'settings'
            transport = directory / 'transport'
            saved = directory / 'saved.json'
            text = Path(__file__).with_name('doubao-setup-ui.sh').read_text()
            helper.write_text(text.replace('/usr/bin/lx06-model-tool', str(model_tool)))
            model_tool.write_text("""#!/usr/bin/env python3
import json
import sys
value = json.load(sys.stdin)[sys.argv[2]]
print(value if isinstance(value, str) else json.dumps(value), end='')
""")
            settings.write_text('''#!/bin/sh
command=$1
shift 2
case "$command" in
  run) exec "$@";;
  save) cat > "$FIXTURE_SAVED";;
esac
''')
            transport.write_text('''#!/bin/sh
if [ "$1" = --protocol ]; then echo LX06DCTRANSPORT/1; exit; fi
if [ "$DB_API_KEY" != fixture-key ]; then echo 'websocket upgrade HTTP=401' >&2; exit 1; fi
[ "$DB_ENDPOINT" = wss://fixture.invalid/voice ] && [ "$DB_VOICE" = test-voice ] &&
[ "$DB_INSTRUCTIONS" = 'test prompt' ] && [ "$DB_AUDIO_FORMAT" = pcm_s16le ] &&
[ -z "${LX06_DB_SETUP_KEY+x}" ]
''')
            for path in (model_tool, settings, transport):
                path.chmod(0o700)
            check = '''
. "$1"
doubao_setup_event() { printf '%s\n' "$@"; }
doubao_setup_save
'''
            for supplied, previous, status, message in [
                ('', '', 1, '请输入豆包 API Key。'),
                ('fixture-key', '', 0, '豆包连接验证通过，配置已保存。'),
                ('bad-key', 'fixture-key', 1, '服务拒绝了 API Key（HTTP 401），请核对密钥。'),
                ('', 'fixture-key', 0, '豆包连接验证通过，配置已保存。'),
            ]:
                with self.subTest(supplied=supplied, previous=previous):
                    saved.write_text('previous')
                    action = 'verify-key' if supplied or not previous else 'save'
                    values = {
                        'DB_API_KEY': supplied, 'DB_ENDPOINT': 'wss://fixture.invalid/voice',
                        'DB_VOICE': 'test-voice', 'DB_INSTRUCTIONS': 'test prompt',
                        'DB_AUDIO_FORMAT': 'pcm_s16le', 'DB_BARGE_ANY': '0',
                    }
                    request = json.dumps({'action': action, 'values': values})
                    env = dict(os.environ, SETTINGS=str(settings), DC_TRANSPORT_ADAPTER=str(transport),
                               FIXTURE_SAVED=str(saved), DB_API_KEY=previous)
                    result = subprocess.run(['sh', '-c', check, 'fixture', str(helper)], env=env,
                                            input=request, capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, status, result.stderr)
                    self.assertEqual(result.stdout.splitlines()[-1], message)
                    if status:
                        self.assertEqual(saved.read_text(), 'previous')
                    else:
                        self.assertEqual(json.loads(saved.read_text()), {'action': 'save', 'values': values})


if __name__=='__main__':unittest.main()

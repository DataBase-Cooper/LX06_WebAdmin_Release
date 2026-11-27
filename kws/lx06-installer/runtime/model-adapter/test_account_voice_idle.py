"""Unified entry must not start a retired background audio controller."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class IdleWarmupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        source = (Path(__file__).parent/'account-voice.sh').read_text()
        self.functions = source.split('# Audio components come from the unified firmware.')[0]
        self.greeting = self.root/'greeting'
        self.greeting.write_text('你好，请说')
        self.executable('settings', '''#!/bin/sh
[ "$1" = run ] || exit 2
shift 2
MODEL_GREETING=$(cat "$FIXTURE/greeting")
export MODEL_GREETING
exec "$@"
''')
        self.executable('audio', '''#!/bin/sh
[ "$1" = --warm-cache ] || exit 2
n=0
[ ! -f "$FIXTURE/count" ] || n=$(cat "$FIXTURE/count")
n=$((n+1)); printf '%s' "$n" > "$FIXTURE/count"
printf 'warm-start\n' >> "$FIXTURE/events"
cat > "$FIXTURE/capture.$n"
while [ -f "$FIXTURE/gate" ] && [ ! -f "$FIXTURE/release.$n" ]; do sleep .02; done
printf 'warm-end\n' >> "$FIXTURE/events"
''')
        self.executable('session-run', '''#!/bin/sh
if [ "$1" = busy ]; then [ -f "$FIXTURE/busy" ]; exit $?; fi
printf 'session-start\n' >> "$FIXTURE/events"
sleep .05
printf 'session-end\n' >> "$FIXTURE/events"
''')

    def executable(self, name, text):
        path = self.root/name
        path.write_text(text)
        path.chmod(0o700)

    def run_shell(self, body):
        script = self.root/'run.sh'
        script.write_text('set -eu\nFIXTURE='+shlex.quote(str(self.root))+'''
export FIXTURE
SETTINGS=$FIXTURE/settings
AUDIO=$FIXTURE/audio
VOICE=/bin/true
LX06_DIALOGUED=/bin/true
XA=$FIXTURE
WARMUP_PID=
wait_file() {
    n=0
    while [ ! -f "$1" ]; do
        n=$((n+1)); [ "$n" -lt 100 ] || exit 3
        sleep .02
    done
}
'''+self.functions+'\n'+body)
        result = subprocess.run(['sh',str(script)],capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr,'')

    def test_repeated_idle_warmup_never_starts_retired_audio(self):
        (self.root/'gate').touch()
        self.run_shell('voice_warmup\nvoice_warmup\n[ -z "$WARMUP_PID" ]\n')
        self.assertFalse((self.root/'count').exists())

    def test_session_does_not_start_post_session_legacy_warmup(self):
        self.run_shell('voice_session 测试唤醒\n[ -z "$SESSION_PID" ]\n[ -z "$WARMUP_PID" ]\n')
        self.assertEqual((self.root/'events').read_text().splitlines(),['session-start','session-end'])
        self.assertFalse((self.root/'count').exists())

    def test_empty_greeting_does_not_launch_audio(self):
        self.greeting.write_text('')
        self.run_shell('voice_warmup\n[ -z "$WARMUP_PID" ]\n')
        self.assertFalse((self.root/'count').exists())

    def test_other_provider_conversation_never_starts_legacy_audio(self):
        (self.root/'busy').touch()
        self.run_shell('voice_warmup\n[ -z "$WARMUP_PID" ]\n')
        self.assertFalse((self.root/'count').exists())

if __name__ == '__main__': unittest.main()

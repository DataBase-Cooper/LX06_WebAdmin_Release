"""Session preconnect ordering and fallback, without accounts or live services.

The native broker suite separately verifies actual TLS pooling. This exercises
the real upload shell: greeting concurrency, private anonymous HEAD material,
listen-ready barrier, warm-in-flight queuing and session-owned cancellation.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
BROKER = r'''#!/usr/bin/env python3
import json,os,signal,socket,sys,time
from pathlib import Path
root=Path(os.environ['FIXTURE'])
def event(name):
 with (root/'events').open('a') as f:f.write(json.dumps({'event':name,'time':time.monotonic()})+'\n')
if sys.argv[1]=='--request':
 d=Path(sys.argv[3]);event('post-client-start')
 with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
  s.connect(sys.argv[2]);s.sendall(b'question');reply=s.recv(64)
 assert reply==b'answer'
 (d/'status').write_text('200')
 sys.stdout.write('data: warmed-answer\n\n')
 raise SystemExit(0)
assert sys.argv[1]=='--serve' and sys.argv[3]=='--warm'
d=Path(sys.argv[4]);assert d.stat().st_mode&0o777==0o700
for name in ['method','url','headers','body','timeout']:
 p=d/name;assert p.is_file() and not p.is_symlink() and p.stat().st_mode&0o777==0o600
assert (d/'method').read_text()=='HEAD' and (d/'timeout').read_text()=='6'
assert (d/'headers').read_bytes()==b'' and (d/'body').read_bytes()==b''
(root/'server-state').write_text(json.dumps({'pid':os.getpid(),'directory':str(d),'url':(d/'url').read_text(),'socket':sys.argv[2]}))
event('server-start')
mode=os.environ.get('BROKER_MODE','normal')
if mode=='old-helper':raise SystemExit(2)
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
s=None
try:
 if mode=='never-ready':
  while True:time.sleep(.05)
 time.sleep(.15)
 s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.bind(sys.argv[2]);s.listen(4)
 (d/'ready').touch(mode=0o600);event('listen-ready')
 if mode=='cancel-warm':
  while True:time.sleep(.05)
 # A greeting must run independently of both startup and the network warm.
 deadline=time.monotonic()+1
 while not (root/'greeting').exists() and time.monotonic()<deadline:time.sleep(.01)
 assert (root/'greeting').exists()
 time.sleep(.3);event('warm-done')
 while True:
  c,_=s.accept()
  with c:
   assert c.recv(64)==b'question';event('post-accepted');c.sendall(b'answer')
finally:
 if s:s.close()
 event('server-stopped')
'''
CURL = r'''#!/usr/bin/env python3
import os,sys
from pathlib import Path
r=Path(os.environ['FIXTURE']);a=sys.argv[1:]
with (r/'curl-calls').open('a') as f:f.write('fallback\n')
Path(a[a.index('--output')+1]).write_text('data: curl-answer\n\n')
sys.stdout.write('200')
'''
VOICE = r'''#!/usr/bin/env python3
import os,subprocess,sys,time
from pathlib import Path
if sys.argv[1:]==['--protocol']:print('LX06DIALOGUE/1');raise SystemExit(0)
r=Path(os.environ['FIXTURE']);(r/'greeting').touch()
if os.environ.get('BROKER_MODE')=='cancel-warm':
 deadline=time.monotonic()+2
 while not Path(os.environ['LX06_MODEL_HTTP_READY']).exists() and time.monotonic()<deadline:time.sleep(.01)
 raise SystemExit(0)
for _ in range(2 if os.environ.get('BROKER_MODE')=='normal' else 1):
 p=subprocess.run(['sh',str(r/'request.sh')],capture_output=True)
 assert p.returncode==0,(p.stdout,p.stderr)
'''


class AccountPreconnectTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.p = Path(self.tmp.name)
        self.bin = self.p/'bin'
        self.bin.mkdir()
        for path, text in [(self.bin/'model-http', BROKER),
                           (self.bin/'curl', CURL), (self.p/'voice', VOICE)]:
            path.write_text(text)
            path.chmod(0o700)
        xa = self.p/'xa'
        xa.mkdir()
        self.functions = (ROOT/'account-voice.sh').read_text().split('# Audio components come from the unified firmware.')[0]
        request = (ROOT/'account-common.sh').read_text()+r'''
WORK=$(mktemp -d "$FIXTURE/request.XXXXXX")
printf '{}' > "$WORK/form"; : > "$WORK/auth"
rc=0;http POST https://fixture.invalid/responses "$WORK/output" "$WORK/auth" streaming || rc=$?
printf '%s|%s\n' "$rc" "${status:-000}" >> "$FIXTURE/results"
[ ! -f "$WORK/output" ] || cat "$WORK/output" >> "$FIXTURE/response"
'''
        (self.p/'request.sh').write_text(request)

    def run_worker(self, mode='normal', prepare_failure=False):
        script = self.p/'worker.sh'
        script.write_text('set -eu\n'+self.functions+'''
HTTP=$FIXTURE/bin/model-http
LX06_DIALOGUED=$FIXTURE/voice
XA=$FIXTURE/xa
AUDIO=/unused/audio
MODEL_ID=fixture
PROVIDER=fixture
provider_preconnect() { HTTP_WARM_URL=https://fixture.invalid/models; }
'''+('printf() { [ "${2:-}" != 6 ] || return 1; command printf "$@"; }\n' if prepare_failure else '')+'voice_worker\nprintf \'%s\' "$SESSION" > "$FIXTURE/session-path"\n')
        env = {**os.environ, 'FIXTURE':str(self.p), 'BROKER_MODE':mode,
               'PATH':str(self.bin)+':'+os.environ['PATH']}
        r = subprocess.run(['sh',str(script)],env=env,capture_output=True,timeout=7)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertFalse(Path((self.p/'session-path').read_text()).exists())
        if prepare_failure:
            self.assertFalse((self.p/'server-state').exists())
            return []
        state = json.loads((self.p/'server-state').read_text())
        with self.assertRaises(ProcessLookupError): os.kill(state['pid'],0)
        self.assertFalse(Path(state['directory']).parent.exists())
        return [json.loads(x)['event'] for x in (self.p/'events').read_text().splitlines()]

    def test_greeting_runs_while_warming_first_question_queues_and_next_reuses(self):
        events = self.run_worker()
        self.assertEqual((self.p/'results').read_text(),'0|200\n0|200\n')
        self.assertEqual((self.p/'response').read_text().count('warmed-answer'),2)
        self.assertEqual(events.count('server-start'),1)
        self.assertEqual(events.count('warm-done'),1)
        self.assertLess(events.index('listen-ready'),events.index('post-client-start'))
        self.assertLess(events.index('post-client-start'),events.index('warm-done'))
        self.assertLess(events.index('warm-done'),events.index('post-accepted'))
        self.assertFalse((self.p/'curl-calls').exists())

    def test_old_helper_exit_falls_back_without_question_handoff(self):
        events = self.run_worker('old-helper')
        self.assertEqual((self.p/'results').read_text(),'0|200\n')
        self.assertEqual((self.p/'curl-calls').read_text(),'fallback\n')
        self.assertNotIn('post-client-start',events)

    def test_live_broker_startup_timeout_never_opens_curl_connection(self):
        events = self.run_worker('never-ready')
        self.assertEqual((self.p/'results').read_text(),'1|000\n')
        self.assertFalse((self.p/'curl-calls').exists())
        self.assertNotIn('post-client-start',events)
        self.assertIn('server-stopped',events)

    def test_session_exit_cancels_warm_and_removes_private_material(self):
        events = self.run_worker('cancel-warm')
        self.assertIn('listen-ready',events)
        self.assertIn('server-stopped',events)
        self.assertNotIn('warm-done',events)
        self.assertFalse((self.p/'curl-calls').exists())

    def test_optional_private_material_failure_preserves_greeting_and_curl(self):
        self.run_worker('prepare-failure', prepare_failure=True)
        self.assertTrue((self.p/'greeting').exists())
        self.assertEqual((self.p/'results').read_text(),'0|200\n')
        self.assertEqual((self.p/'curl-calls').read_text(),'fallback\n')

    def test_provider_hooks_are_public_same_origin_and_never_run_authentication(self):
        for provider, url in [('chatgpt','https://api.openai.com/v1/models'),
                              ('grok','https://cli-chat-proxy.grok.com/v1/models'),
                              ('deepseek','https://chat.deepseek.com/')]:
            source = ROOT/('deepseek-login.sh' if provider=='deepseek' else provider+'-account.sh')
            text = source.read_text().split('case "${1:-}" in')[0]
            r = subprocess.run(['sh','-c',text+'''
prepare() { exit 51; };ensure_access() { exit 52; };provider_headers() { exit 53; }
provider_preconnect
printf '%s' "$HTTP_WARM_URL"
'''],capture_output=True,timeout=2)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertEqual(r.stdout.decode(),url)


if __name__ == '__main__': unittest.main()

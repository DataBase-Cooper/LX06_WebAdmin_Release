"""Actual shell transport selection and owned session cleanup; no live hosts.

The C broker tests cover real connection reuse/protocol/cancellation. These
fixtures test the adapters' integration, including safe pre-send fallback.
"""
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parent
FAKE_HTTP='''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
p=Path(sys.argv[3]);root=Path(os.environ['FIXTURE']);rc=int(os.environ.get('BROKER_RC','0'))
assert sys.argv[1]=='--request'
assert p.stat().st_mode&0o777==0o700
for name in ['method','url','headers','body','timeout']:
 f=p/name;assert f.is_file() and not f.is_symlink() and f.stat().st_mode&0o777==0o600
record={'argv':sys.argv[1:],'socket':sys.argv[2],'method':(p/'method').read_text(),'url':(p/'url').read_text(),'body':(p/'body').read_text(),'timeout':(p/'timeout').read_text()}
with (root/'broker-calls').open('a') as out:out.write(json.dumps(record)+'\\n')
(p/'status').write_text('403' if rc==22 else ('000' if rc else '200'))
if rc==0:sys.stdout.write('data: broker-one\\n\\ndata: broker-two\\n\\n')
elif rc==1:sys.stdout.write('data: partial\\n\\n')
sys.exit(rc)
'''
FAKE_CURL='''#!/usr/bin/env python3
import json,os,sys
from pathlib import Path
a=sys.argv[1:];root=Path(os.environ['FIXTURE'])
with (root/'curl-calls').open('a') as f:f.write(json.dumps(a)+'\\n')
with Path(a[a.index('--output')+1]).open('w') as f:f.write('data: fallback\\n\\n')
sys.stdout.write('200')
'''

class AccountHTTPTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.p=Path(self.tmp.name)
        self.bin=self.p/'bin';self.bin.mkdir()
        for name,body in [('model-http',FAKE_HTTP),('curl',FAKE_CURL)]:
            path=self.bin/name;path.write_text(body);path.chmod(0o700)
        self.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        self.socket_path=self.p/'broker.sock';self.sock.bind(str(self.socket_path));self.addCleanup(self.sock.close)
        self.env={**os.environ,'FIXTURE':str(self.p),'PATH':str(self.bin)+':'+os.environ['PATH'],
                  'LX06_MODEL_HTTP_HELPER':str(self.bin/'model-http'),'LX06_MODEL_HTTP_SOCKET':str(self.socket_path)}

    def call(self,rc=0,streaming=True,repeat=1):
        work=self.p/'request';work.mkdir(mode=0o700)
        (work/'auth').write_text('header = "Authorization: Bearer private-fixture"\n');(work/'auth').chmod(0o600)
        (work/'form').write_text('{"prompt":"fixture"}');(work/'form').chmod(0o600)
        source=(ROOT/'account-common.sh').read_text()+'''\n
WORK="${FIXTURE}/request"
mkfifo "$WORK/stream"
exec 6<>"$WORK/stream"
cat < "$WORK/stream" > "$FIXTURE/response" 6>&- &
reader=$!
'''
        for _ in range(repeat):
            source+='rc=0; http POST https://fixture.invalid/responses "$WORK/stream" "$WORK/auth" '+('streaming' if streaming else '')+' || rc=$?\n'
        source+='''
exec 6>&-
wait "$reader"
printf '%s|%s' "$rc" "$status" > "$FIXTURE/result"
find "$WORK" -name 'http.*' > "$FIXTURE/leftovers"
'''
        r=subprocess.run(['sh','-c',source],env={**self.env,'BROKER_RC':str(rc)},capture_output=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        self.assertNotIn(b'private-fixture',r.stdout+r.stderr)
        self.assertEqual((self.p/'leftovers').read_text(),'')
        return (self.p/'result').read_text(),(self.p/'response').read_text()

    def test_streams_use_same_session_socket_and_private_request_files(self):
        result,body=self.call(repeat=2)
        self.assertEqual(result,'0|200');self.assertEqual(body.count('broker-one'),2)
        calls=list(map(json.loads,(self.p/'broker-calls').read_text().splitlines()))
        self.assertEqual(len(calls),2)
        self.assertTrue(all(c['socket']==str(self.socket_path) and c['method']=='POST' and c['timeout']=='120' for c in calls))
        self.assertTrue(all('private-fixture' not in str(c['argv']) and '{"prompt"' not in str(c['argv']) for c in calls))
        self.assertFalse((self.p/'curl-calls').exists())

    def test_only_pre_send_unavailable_may_fall_back(self):
        result,body=self.call(rc=75)
        self.assertEqual(result,'0|200');self.assertEqual(body,'data: fallback\n\n')
        self.assertEqual(len((self.p/'curl-calls').read_text().splitlines()),1)

    def test_connected_transport_failure_never_replays_post(self):
        result,body=self.call(rc=1)
        self.assertEqual(result,'1|000');self.assertIn('partial',body)
        self.assertFalse((self.p/'curl-calls').exists())

    def test_http_rejection_never_replays_post(self):
        result,body=self.call(rc=22)
        self.assertEqual(result,'2|403');self.assertEqual(body,'')
        self.assertFalse((self.p/'curl-calls').exists())

    def test_nonstreaming_keeps_original_transport(self):
        result,body=self.call(streaming=False)
        self.assertEqual(result,'0|200');self.assertIn('fallback',body)
        self.assertFalse((self.p/'broker-calls').exists())

    def test_native_broker_is_owned_and_reaped_with_voice_session(self):
        http=self.p/'server';voice=self.p/'voice';xa=self.p/'xa';xa.mkdir()
        http.write_text('''#!/usr/bin/env python3
import json,os,signal,socket,sys,time
from pathlib import Path
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.bind(sys.argv[2]);s.listen(1)
Path(os.environ['FIXTURE'],'server-state').write_text(json.dumps({'pid':os.getpid(),'socket':sys.argv[2]}))
signal.signal(signal.SIGTERM,lambda *_:sys.exit(0))
try:
 while True:time.sleep(.05)
finally:s.close()
''');http.chmod(0o700)
        voice.write_text('''#!/usr/bin/env python3
import os,socket,sys,time
from pathlib import Path
if sys.argv[1:]==['--protocol']:print('LX06DIALOGUE/1');raise SystemExit(0)
for _ in range(50):
 try:
  with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:s.connect(os.environ['LX06_MODEL_HTTP_SOCKET'])
  break
 except OSError:time.sleep(.01)
else:raise SystemExit(2)
Path(os.environ['FIXTURE'],'worker-env').write_text(os.environ['LX06_MODEL_HTTP_HELPER'])
''');voice.chmod(0o700)
        source=(ROOT/'account-voice.sh').read_text().rsplit('case "${1:-}" in',1)[0]
        source+='\nHTTP='+shlex.quote(str(http))+'\nLX06_DIALOGUED='+shlex.quote(str(voice))+'\nXA='+shlex.quote(str(xa))+'''
AUDIO=/unused/audio
MODEL_ID=fixture
PROVIDER=fixture
voice_worker
'''
        r=subprocess.run(['sh','-c',source],env=self.env,capture_output=True,timeout=5)
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        state=json.loads((self.p/'server-state').read_text())
        self.assertEqual((self.p/'worker-env').read_text(),str(http))
        self.assertFalse(Path(state['socket']).exists());self.assertFalse(Path(state['socket']).parent.exists())
        with self.assertRaises(ProcessLookupError):os.kill(state['pid'],0)

if __name__=='__main__':unittest.main()

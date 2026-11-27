"""Real standalone configuration probe: auth/session required, zero audio IO."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from websockets.sync.server import serve

BIN=Path(__file__).resolve().parents[1]/'build/doubao-host'


class SetupCheck(unittest.TestCase):
    def probe(self, accepted):
        events=[];errors=[]
        def handler(ws):
            try:
                # This provider's WebSocket transport uses X-Api-Key.
                self.assertEqual(ws.request.headers['X-Api-Key'],'fixture-key')
                events.append(json.loads(ws.recv(timeout=3)))
                self.assertEqual(events[0]['type'],'session.create')
                ws.send(json.dumps({'type':'session.created' if accepted else 'error'}))
                events.append(json.loads(ws.recv(timeout=3)))
                self.assertEqual(events[1],{'type':'session.close'})
            except Exception as exc:errors.append(exc)
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);sentinel=p/'audio-opened';trap=p/'trap'
            trap.write_text('#!/bin/sh\ntouch "'+str(sentinel)+'"\nexit 1\n');trap.chmod(0o700)
            with serve(handler,'127.0.0.1',0) as server:
                thread=threading.Thread(target=server.serve_forever);thread.start()
                port=server.socket.getsockname()[1]
                env={**os.environ,'DB_API_KEY':'fixture-key','DB_ENDPOINT':f'ws://127.0.0.1:{port}',
                     'DB_MIC_FIFO':str(p/'nonexistent'),'DB_PLAYBACK':'1','DB_APLAY':str(trap),
                     'DB_CONTROL_ASR':'xiaomi','DB_BARGE_ANY':'0'}
                try:r=subprocess.run([str(BIN),'--check'],env=env,capture_output=True,text=True,timeout=5)
                finally:server.shutdown();thread.join()
            self.assertFalse(sentinel.exists())
        self.assertFalse(errors,errors)
        self.assertEqual(len(events),2)
        self.assertEqual(r.returncode,0 if accepted else 1,r.stdout+r.stderr)
        self.assertNotIn('fixture-key',r.stdout+r.stderr)
        self.assertNotIn('session ready',r.stdout+r.stderr)

    def test_authenticated_session_without_mic_player_or_control_asr(self):self.probe(True)
    def test_socket_handshake_without_provider_acceptance_is_not_valid(self):self.probe(False)


if __name__=='__main__':unittest.main()

"""Existing phone connecting tone only: real reactor, delayed synthetic peer/ASR.

Not spoken greeting, cloud transport, real SDK, device audio or acoustic test.
Run on isolated POSIX tmpfs in a private network namespace.
"""
import argparse
import json
import os
import socket
import sys
import time
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_reactor as base
import test_mode_reactor as mode
from test_account_delayed_asr_ready import DelayedASR


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    args = parser.parse_args()
    assert os.geteuid() == 0 and [n for _, n in socket.if_nameindex()] == ['lo']
    assert Path(os.environ['TMPDIR']).resolve() == Path('/dev/shm')
    binary, library = args.binary.resolve(), args.library.resolve()
    manifest = json.loads((binary.parent / 'manifest.json').read_text())
    assert manifest['passed'] and manifest['fixture'] and not manifest['arm']
    assert base.sha(binary) == manifest['sha256']
    assert all(base.sha(mode.ROOT / p) == h for p, h in manifest['sources'].items())
    needle = "ack('started');time.sleep(.25)"
    assert mode.WORKER.count(needle) == 1
    worker = mode.WORKER.replace(needle, """while not (root/'release-peer').exists(): time.sleep(.005)
ack('started');time.sleep(.25)""")
    with patch.object(base, 'SpeakerASR', DelayedASR), patch.object(mode, 'WORKER', worker):
        with mode.ModeReactor(binary, library, 'phone', 'waiting-control') as r:
            assert r.asr.ready_held.wait(3)
            report = base.wait(lambda: (r.root/'phone-worker.json').exists() and
                               json.loads((r.root/'phone-worker.json').read_text()),
                               label='synthetic peer received OPEN')
            r.event('cue_pcm')
            writes = [e for e in r.ledger() if e['op'] == 'WRITE']
            assert writes and not (r.root/'release-peer').exists()
            assert not r.asr.ready_released.is_set()
            assert report['request']['action'] == 'call.start'
            assert not r.broker.requests and not r.requests()
            r.asr.emit('等一下', final=False, dialog=r.asr.dialog)
            time.sleep(.15)
            assert not [e for e in r.ledger() if e['op'] == 'DROP']
            r.asr.release()
            r.asr.emit('等一下', final=False, dialog=r.asr.dialog)
            base.wait(lambda: [e for e in r.ledger() if e['op'] == 'DROP'],
                      label='post-READY cue drop')
            assert not (r.root/'release-peer').exists()
            (r.root/'release-peer').touch()
            r.asr.emit('结束对话', final=False)
            r.finish_mode(0)
            print('PASS: synthetic OPEN received and tone played with peer and ASR READY held; pre-READY control rejected, post-READY control dropped tone')


if __name__ == '__main__':
    main()

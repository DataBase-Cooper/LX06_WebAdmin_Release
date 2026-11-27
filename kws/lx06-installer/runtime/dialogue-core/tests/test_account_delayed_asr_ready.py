"""Offline real reactor: independent TTS vs delayed ASR bridge READY.

Run in private mount/network namespaces with TMPDIR on a short POSIX tmpfs.
This is NOT DC_EVT_READY (that belongs to audio-provider adapters). Account and
single-action modes have no such uplink; this delays their ASR dialog READY.
Synthetic PCM and SDK bridge do not demonstrate acoustic interruption.
"""
import argparse
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_reactor as base
from test_mode_reactor import ModeReactor


class DelayedASR(base.SpeakerASR):
    def __enter__(self):
        self.ready_held = threading.Event()
        self.ready_released = threading.Event()
        self.ready_message = None
        return super().__enter__()

    def send(self, obj):
        if obj.get('type') == 'ready' and not self.ready_released.is_set():
            self.ready_message = obj
            self.ready_held.set()
            return
        super().send(obj)

    def release(self):
        assert self.ready_held.is_set() and self.ready_message
        self.ready_released.set()
        super().send(self.ready_message)


def case(binary, library, action):
    with patch.object(base, 'SpeakerASR', DelayedASR):
        fixture = (ModeReactor(binary, library, 'action', 'success') if action else
                   base.Reactor(binary, library, duration=4, greeting='fixture greeting'))
        with fixture as r:
            assert r.asr.ready_held.wait(3)
            r.event('first_pcm')
            writes = [x for x in r.ledger() if x['op'] == 'WRITE']
            assert writes and not r.asr.ready_released.is_set()
            expected = '操作已完成。' if action else 'fixture greeting'
            assert [q['text'] for q in r.broker.requests if q['kind'] == 2] == [expected]
            assert r.asr.audio_packets > 0, 'capture uplink must continue while READY is held'
            # Results received before READY have no accepted dialog identity.
            r.asr.emit('等一下', final=False, dialog=r.asr.dialog)
            time.sleep(.1)
            assert not [x for x in r.ledger() if x['op'] == 'DROP']
            r.asr.release()
            # A partial sent immediately after READY is serialized on the same socket.
            r.asr.emit('等一下', final=False, dialog=r.asr.dialog)
            base.wait(lambda: [x for x in r.ledger() if x['op'] == 'DROP'], label='post-READY greeting cancellation')
            if action:
                r.finish_mode(0)
            else:
                r.final('等一下，接着介绍颐和园。', r.asr.dialog)
                base.wait(lambda: r.requests(), label='complete follow-up')
                assert r.requests()[0]['input'][-1]['content'] == '接着介绍颐和园'
                r.finish()
            print(('action' if action else 'account') + ': pre-READY PCM, post-READY cancellation PASS')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', required=True, type=Path)
    p.add_argument('--library', required=True, type=Path)
    a = p.parse_args()
    assert os.geteuid() == 0 and [n for _, n in socket.if_nameindex()] == ['lo']
    assert Path(os.environ['TMPDIR']).resolve() == Path('/dev/shm')
    assert a.binary.is_file() and (a.library / 'libasound.so.2').is_file()
    case(a.binary.resolve(), a.library.resolve(), False)
    case(a.binary.resolve(), a.library.resolve(), True)

if __name__ == '__main__':
    main()

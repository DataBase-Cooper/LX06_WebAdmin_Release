"""Expected RED: real account reactor, synthetic ASR/TTS/ALSA, no device.

The ASR bridge is connected and receives audio, but its dialog identity READY
is deliberately withheld. Injected text models an early bridge result; it is NOT
proof that the real SDK emits a trustworthy result before READY. Run only in a
private mount/network namespace with TMPDIR=/dev/shm.
"""
import argparse
import os
from pathlib import Path
import socket
import time
from unittest.mock import patch

import test_reactor as base
from test_account_delayed_asr_ready import DelayedASR


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', required=True, type=Path)
    p.add_argument('--library', required=True, type=Path)
    args = p.parse_args()
    assert os.geteuid() == 0 and [name for _, name in socket.if_nameindex()] == ['lo']
    assert Path(os.environ['TMPDIR']).resolve() == Path('/dev/shm')
    with patch.object(base, 'SpeakerASR', DelayedASR):
        with base.Reactor(args.binary.resolve(), args.library.resolve(), duration=4,
                          greeting='fixture greeting') as r:
            assert r.asr.ready_held.wait(3)
            r.event('first_pcm')
            assert r.asr.audio_packets > 0
            assert not r.asr.ready_released.is_set()
            # An unrelated recognition must not act like a configured keyword.
            r.asr.emit('今天的天气怎么样', final=False, dialog=r.asr.dialog)
            time.sleep(.1)
            assert not [x for x in r.ledger() if x['op'] == 'DROP'], 'ordinary speech cancelled keyword-mode playback'
            r.asr.emit('等一下', final=False, dialog=r.asr.dialog)
            try:
                base.wait(lambda: [x for x in r.ledger() if x['op'] == 'DROP'],
                          seconds=1, label='keyword cancellation before ASR READY')
            except AssertionError as exc:
                raise AssertionError('RED: configured keyword did not stop greeting before READY') from exc
            assert not r.asr.ready_released.is_set()
            r.asr.release()
            r.final('等一下，接着介绍颐和园。', r.asr.dialog)
            base.wait(lambda: r.requests(), label='complete follow-up')
            assert r.requests()[0]['input'][-1]['content'] == '接着介绍颐和园'
            r.finish()
    print('PASS: early keyword and complete follow-up')


if __name__ == '__main__':
    main()

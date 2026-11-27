"""Behavioral acceptance: a prepared greeting is played without synthesis.

Offline only: run in private network AND mount namespaces with a fresh tmpfs
mounted on /tmp; pass a freshly built DC_CORE_HOST_FIXTURE binary. This is an
expected failure until the unified reactor consumes the legacy PCM cache.
"""
import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile

from test_reactor import Reactor, wait


def main():
    if os.geteuid() != 0 or [n for _, n in socket.if_nameindex()] != ['lo']:
        raise RuntimeError('requires root and an isolated network namespace')
    # Require the launcher to create a dedicated tmpfs, not DrvFS or live /tmp.
    mounts = Path('/proc/self/mountinfo').read_text().splitlines()
    if not any(' /tmp ' in line and ' - tmpfs lx06-greeting-test ' in line for line in mounts):
        raise RuntimeError('requires private /tmp mounted as lx06-greeting-test')
    cache = Path('/tmp/lx06-model-tts-cache')
    cache.mkdir(mode=0o700)
    text = 'fixture greeting'
    pcm = struct.pack('<h', 3141) * 3200
    wav = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', len(pcm)+36, b'WAVE', b'fmt ',
                      16, 1, 1, 16000, 32000, 2, 16, b'data', len(pcm)) + pcm
    entry = cache / ('g-' + hashlib.sha256(text.encode()).hexdigest() + '.wav')
    entry.write_bytes(wav); entry.chmod(0o600)
    assert cache.stat().st_mode & 0o777 == 0o700 and entry.stat().st_mode & 0o777 == 0o600
    with tempfile.TemporaryDirectory(prefix='greeting-fake-') as d:
        lib = Path(d)
        subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        str(Path(__file__).with_name('fake-asound.c')), '-o', str(lib/'libasound.so.2')], check=True)
        with Reactor(Path(sys.argv[1]).resolve(), lib, duration=.2, greeting=text) as r:
            writes = wait(lambda: [e for e in r.ledger() if e['op'] == 'WRITE'], label='greeting playback')
            synth = [q for q in r.broker.requests if q['kind'] == 2]
            assert not synth, 'prepared greeting unexpectedly submitted to TTS'
            assert writes[0]['sample'] == 3141, 'prepared PCM was not played'
            r.finish()
    print('PASS: prepared greeting played without synthesis')


if __name__ == '__main__':
    main()

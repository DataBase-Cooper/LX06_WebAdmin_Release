"""Synthetic-only identity authority/cache. Never use outside offline tests.

The private fixture key and HMAC model an authenticated response; they do not
represent a real provider, peer authentication, or a production cache format.
"""
import hashlib
import hmac
import json
import struct

KEY = b'public-test-only-key-not-a-credential'
PCM = struct.pack('<h', 3141) * 3200

class Backend:
    def __init__(self, root):
        self.cache = root / 'synthetic-greeting.json'

    @staticmethod
    def sign(payload):
        return hmac.new(KEY, payload, hashlib.sha256).hexdigest()

    def upload(self, text, revision):
        if not text or not isinstance(revision, int) or revision <= 0:
            raise ValueError('invalid synthetic identity')
        # This is a synthetic upload event, not the webd upload handler.
        payload = json.dumps({'provider':'fixture', 'account':'fixture',
                              'voice':'fixture-voice', 'voice_revision':revision,
                              'profile_revision':1, 'epoch':1, 'rate':16000,
                              'channels':1, 'format':'s16le', 'text':text,
                              'pcm':PCM.hex()}, sort_keys=True, separators=(',', ':')).encode()
        self.cache.write_bytes(payload + b'\n' + self.sign(payload).encode())

    def lookup(self, text, revision):
        try:
            payload, tag = self.cache.read_bytes().rsplit(b'\n', 1)
            if not hmac.compare_digest(self.sign(payload).encode(), tag): return None
            item = json.loads(payload)
            required = {'provider':'fixture', 'account':'fixture', 'voice':'fixture-voice',
                        'voice_revision':revision, 'profile_revision':1, 'epoch':1,
                        'rate':16000, 'channels':1, 'format':'s16le', 'text':text}
            if any(item.get(k) != v for k, v in required.items()): return None
            pcm = bytes.fromhex(item['pcm'])
            return pcm if pcm == PCM else None
        except (OSError, ValueError, TypeError, KeyError):
            return None


def run_reactor(binary):
    """Real core -> fixture broker -> synthetic cached WAV -> fake ALSA.

    The core still issues a normal synthesis request; the backend resolves it
    from the fixture cache. This is NOT core-side cache consumption.
    """
    import os
    import socket
    import subprocess
    import tempfile
    import test_reactor as reactor
    from pathlib import Path
    if os.geteuid() != 0 or [n for _, n in socket.if_nameindex()] != ['lo']:
        raise RuntimeError('requires isolated network namespace')
    if not any(' /tmp ' in line and ' - tmpfs lx06-greeting-test ' in line
               for line in Path('/proc/self/mountinfo').read_text().splitlines()):
        raise RuntimeError('requires private lx06-greeting-test tmpfs')
    text = 'fixture greeting'
    with tempfile.TemporaryDirectory(prefix='greeting-backend-') as tmp:
        root = Path(tmp)
        backend = Backend(root)
        backend.upload(text, 7)
        assert backend.lookup(text, 7) == PCM
        original_broker, original_decoder = reactor.Broker, reactor.DECODER
        class CacheBroker(original_broker):
            def handle(self, connection):
                with connection:
                    try:
                        data = connection.recv(8192)
                        if not data: return
                        magic, version, kind, rid, generation, length, code = reactor.HEADER.unpack(data[:32])
                        assert (magic, version, rid, code, len(data)) == (reactor.MAGIC, 1, 0, 0, 32 + length)
                        request_text = data[32:].decode()
                        self.requests.append(dict(kind=kind, generation=generation, text=request_text))
                        if kind == 1:
                            reply = reactor.HEADER.pack(reactor.MAGIC, 1, 16, 1, generation or 1, 0, 0)
                        else:
                            assert kind == 2
                            pcm = backend.lookup(request_text, 7)
                            assert pcm == PCM, 'cache miss or identity mismatch'
                            wav = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', len(pcm)+36, b'WAVE', b'fmt ', 16, 1, 1, 16000, 32000, 2, 16, b'data', len(pcm)) + pcm
                            (self.path.parent / 'fixture.wav').write_bytes(wav)
                            url = b'https://fixture.invalid/cached'
                            reply = reactor.HEADER.pack(reactor.MAGIC, 1, 17, 2, generation, len(url), 0) + url
                        connection.send(reply)
                    except BaseException as error:
                        self.errors.append(repr(error))
        reactor.Broker = CacheBroker
        reactor.DECODER = '''#!/usr/bin/python3\nimport os,sys\nfrom pathlib import Path\nassert sys.stdin.buffer.read() == b'https://fixture.invalid/cached'\nsys.stdout.buffer.write((Path(os.environ['DC_FIXTURE_DIRECTORY'])/'fixture.wav').read_bytes())\n'''
        try:
            subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                            str(Path(__file__).with_name('fake-asound.c')), '-o', str(root/'libasound.so.2')], check=True)
            with reactor.Reactor(binary.resolve(), root, duration=.2, greeting=text) as r:
                writes = reactor.wait(lambda: [e for e in r.ledger() if e['op'] == 'WRITE'], label='cached greeting playback')
                assert writes[0]['sample'] == 3141, writes[0]
                assert [q['text'] for q in r.broker.requests if q['kind'] == 2] == [text]
                assert not r.broker.errors, r.broker.errors
                r.finish()
        finally:
            reactor.Broker, reactor.DECODER = original_broker, original_decoder

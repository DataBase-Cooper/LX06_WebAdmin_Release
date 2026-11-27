"""EXPECTED RED: the real v1 worker cannot resume a causally fenced PC reply.

Run only in an isolated Linux network+mount namespace, with loopback enabled and
TMPDIR on a private root-owned tmpfs.  Synthetic credentials and PCM only.  This
is not a PC application, device test, or proof of producer-side provenance.
"""
import hashlib
import json
import os
from pathlib import Path
import tempfile

import test_phone as t


def main():
    assert os.geteuid() == 0 and os.environ.get('LX06_PHONE_NETNS') == '1'
    with tempfile.TemporaryDirectory(prefix='pc-fence-') as name:
        root = Path(name)
        root.chmod(0o700)
        binary = root / 'worker' / 'lx06-phone-worker'
        t.execute(['bash', t.ADAPTER / 'build_phone.sh', 'host'],
                  env=dict(os.environ, DC_PHONE_BUILD_DIR=str(binary.parent)))
        ca, cakey, key, csr, cert, ext = [root / x for x in
            ('ca.pem', 'ca-key.pem', 'key.pem', 'request.csr', 'cert.pem', 'ext')]
        t.execute(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                   '-keyout', cakey, '-out', ca, '-days', '1', '-subj', '/CN=Fixture CA'])
        t.execute(['openssl', 'req', '-new', '-newkey', 'rsa:2048', '-nodes',
                   '-keyout', key, '-out', csr, '-subj', '/CN=Fixture peer'])
        ext.write_text('subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n')
        t.execute(['openssl', 'x509', '-req', '-in', csr, '-CA', ca, '-CAkey', cakey,
                   '-CAcreateserial', '-out', cert, '-days', '1', '-extfile', ext])
        public = t.execute(['openssl', 'x509', '-in', cert, '-pubkey', '-noout'])
        pin = hashlib.sha256(t.execute(['openssl', 'pkey', '-pubin', '-outform', 'DER'], input=public)).hexdigest()
        t.COMMAND_DEADLINE = int(t.time.time() * 1000) + 15000

        def peer(server, s):
            _, b = t.read_frame(s)
            assert t.unpack(b)['type'] == 1
            caps = dict(virtual_mic=True, rtc_capture=True, call_lifecycle=True,
                        reply_lifecycle=True, turn_fence=True, automatic_ui=False)
            s.sendall(t.frame(t.ack()) + t.frame(t.ready(True, capabilities=caps)) +
                      t.frame(t.packet(36, seq=7)))
            while True:
                _, b = t.read_frame(s)
                p = t.unpack(b)
                if p['type'] == 6 and p['sequence'] == 7:
                    assert p['generation'] == 9
                    s.sendall(t.frame(t.packet(37, b'\x11\x22' * 16, gen=9, seq=7,
                                              flags=2, rate=16000, channels=1)))
                if p['type'] == 3:
                    assert (p['epoch'], p['generation'], p['sequence']) == (t.EPOCH, 9, 7)
                    s.sendall(t.frame(t.packet(39, epoch=p['epoch'], gen=p['generation'], seq=p['sequence'])))
                if p['type'] == 5:
                    return

        server = t.Server(cert, key, peer)
        config = root / 'pairing.json'
        config.write_text(json.dumps(dict(endpoint=f'wss://127.0.0.1:{server.port}{t.PATH}',
                                          token=t.TOKEN, ca_file=str(ca), spki_sha256=pin)))
        config.chmod(0o600)
        worker = t.Worker(binary, config)
        try:
            worker.send(t.packet(1, t.envelope()))
            worker.until(32)
            assert worker.until(36)['sequence'] == 7
            worker.send(t.packet(6, gen=9, seq=7))
            audio = worker.until(37)
            assert (audio['generation'], audio['sequence'], audio['payload']) == (9, 7, b'\x11\x22' * 16)
            worker.send(t.packet(3, gen=9, seq=7))
            ack = worker.until(39)
            assert (ack['generation'], ack['sequence']) == (9, 7)
            worker.send(t.packet(4, gen=10))
            event = worker.read(3)
            assert event is not None, 'worker produced no RESUME result'
            assert event['type'] == 41, (
                'missing authenticated producer drain boundary: real v1 worker returned '
                f'type={event["type"]} payload={event["payload"]!r}, not RESUMED; '
                'turn_fence capability and matching CANCEL_ACK do not establish a fence')
        finally:
            worker.close()
            server.join()


if __name__ == '__main__':
    main()

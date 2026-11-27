"""Offline direct-broker / controlled decoder / WAV-to-PCM boundaries.

Only a synthetic UNIX broker and local fixtures are used. The final test also
uses the real native-url-audio C/minimp3 decoder against a loopback HTTP server.
No SDK/account/device/cloud or speaker is involved. Run inside unshare --net.
"""
import ctypes as C
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

from test_account import Tag, now

ROOT = Path(__file__).resolve().parent
EXPERIMENTS = ROOT.parent / 'model-adapter/experiments'
HEADER = struct.Struct('<IHHQQII')
MAGIC = 0x31544e4c


class Config(C.Structure):
    _fields_ = [('socket_path', C.c_char_p), ('decoder_path', C.c_char_p), ('broker_pid', C.c_int)]


class Event(C.Structure):
    _fields_ = [('type', C.c_int), ('tag', Tag), ('segment_sequence', C.c_uint64),
                ('at_ms', C.c_uint64), ('error', C.c_int), ('rate', C.c_uint)]


class PCM(C.Structure):
    _fields_ = [('tag', Tag), ('segment_sequence', C.c_uint64), ('rate', C.c_uint),
                ('data', C.c_void_p), ('bytes', C.c_size_t)]


class Broker:
    def __init__(self, path):
        self.path = path
        self.requests = []
        self.options = {}
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.socket.bind(str(path))
        self.socket.listen(8)
        self.socket.settimeout(.05)
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with connection:
                connection.settimeout(.3)
                try:
                    record = connection.recv(8192)
                    if not record:
                        continue
                    magic, version, kind, rid, generation, length, code = HEADER.unpack(record[:32])
                    assert (magic, version, rid, code) == (MAGIC, 1, 0, 0)
                    assert len(record) == 32 + length
                    self.requests.append((kind, generation, record[32:]))
                    if self.options.get('silent'):
                        time.sleep(.15)
                        continue
                    if kind == 1:
                        response = HEADER.pack(MAGIC, 1, 16, len(self.requests), generation or 1, 0, 0)
                    else:
                        assert kind == 2 and length <= 512
                        payload = self.options.get('url', b'https://fixture.invalid/audio?signature=SYNTHETIC')
                        response = HEADER.pack(MAGIC, 1, 17, len(self.requests),
                                               generation + self.options.get('wrong_generation', 0),
                                               len(payload), self.options.get('code', 0)) + payload
                        if self.options.get('status'):
                            response = HEADER.pack(MAGIC, 1, 18, len(self.requests), generation, 0, 3)
                        if self.options.get('truncated'):
                            response = response[:19]
                    if self.options.get('ancillary'):
                        descriptor = os.open('/dev/null', os.O_RDONLY)
                        try:
                            connection.sendmsg([response], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                                               struct.pack('i', descriptor))])
                        finally:
                            os.close(descriptor)
                    else:
                        connection.send(response)
                except (OSError, ValueError):
                    pass  # cancellations deliberately close an owned request

    def close(self):
        self.stopped.set()
        self.socket.close()
        self.thread.join(timeout=1)


class TTSTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='dc-tts-build-')
        cls.libpath = Path(cls.build.name) / 'tts.so'
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fPIC', '-shared', str(ROOT / 'tts.c'),
                        '-o', str(cls.libpath)], check=True)
        cls.lib = C.CDLL(str(cls.libpath), use_errno=True)
        cls.lib.dc_tts_create.argtypes = [C.POINTER(Config)]
        cls.lib.dc_tts_create.restype = C.c_void_p
        cls.lib.dc_tts_destroy.argtypes = [C.c_void_p]
        cls.lib.dc_tts_shutdown.argtypes = [C.c_void_p, C.c_uint64]
        cls.lib.dc_tts_quiescent.argtypes = [C.c_void_p]
        cls.lib.dc_tts_warm.argtypes = [C.c_void_p, Tag, C.c_uint64]
        cls.lib.dc_tts_submit.argtypes = [C.c_void_p, Tag, C.c_uint64, C.c_char_p, C.c_uint64]
        cls.lib.dc_tts_activate.argtypes = [C.c_void_p, Tag, C.c_uint64, C.c_uint64]
        cls.lib.dc_tts_cancel.argtypes = [C.c_void_p, Tag, C.c_uint64]
        cls.lib.dc_tts_poll.argtypes = [C.c_void_p, C.c_uint64, C.POINTER(Event)]
        cls.lib.dc_tts_peek_pcm.argtypes = [C.c_void_p, C.POINTER(PCM)]
        cls.lib.dc_tts_consume_pcm.argtypes = [C.c_void_p, Tag, C.c_uint64, C.c_size_t]
        cls.lib.dc_tts_next.argtypes = [C.c_void_p, C.POINTER(Tag), C.POINTER(C.c_uint64), C.POINTER(C.c_int)]
        cls.lib.dc_tts_tick.argtypes = [C.c_void_p, C.c_uint64]
        for name in ('slots', 'decoder_active', 'retired'):
            getattr(cls.lib, 'dc_tts_' + name).argtypes = [C.c_void_p]
        cls.lib.dc_tts_storage_bytes.restype = C.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='dc-tts-test-')
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.broker = Broker(self.path / 'broker.sock')
        self.addCleanup(self.broker.close)
        self.mode = dict(channels=1, rate=16000, samples=[1000, -1000, 32767, -32768] * 100)
        self.write_mode()
        self.decoder = self.path / 'decoder'
        self.decoder.write_text('#!/usr/bin/env python3\n'
            'import json,os,signal,struct,sys,time\nfrom pathlib import Path\n'
            + 'root=Path(' + repr(str(self.path)) + ')\n'
            'url=sys.stdin.buffer.read()\n'
            'assert url.startswith((b"https://",b"http://")) and len(sys.argv)==1\n'
            'assert "MODEL_AUDIO_READY_FD" not in os.environ\n'
            'with (root/"seen").open("a") as f: f.write(json.dumps(dict(argv=sys.argv,bytes=len(url)))+"\\n")\n'
            'mode=json.loads((root/"mode").read_text())\n'
            'if mode.get("ignore_term"): signal.signal(signal.SIGTERM,signal.SIG_IGN)\n'
            'channels=mode["channels"];rate=mode["rate"]\n'
            'data=struct.pack("<"+"h"*len(mode["samples"]),*mode["samples"])\n'
            'size=0x7ffff000 if mode.get("streaming") else len(data)+mode.get("length_delta",0)\n'
            'header=struct.pack("<4sI4s4sIHHIIHH4sI",b"RIFF",size+36,b"WAVE",b"fmt ",16,1,channels,rate,rate*channels*2,channels*2,16,b"data",size)\n'
            'if mode.get("bad_header"): header=b"BAD!"+header[4:]\n'
            'if mode.get("odd"): data+=b"x"\n'
            'wire=header+data\n'
            'for at in range(0,len(wire),mode.get("chunk",4096)):\n'
            ' sys.stdout.buffer.write(wire[at:at+mode.get("chunk",4096)]);sys.stdout.buffer.flush();time.sleep(mode.get("hold",0))\n'
            'time.sleep(mode.get("tail",0))\n'
            'sys.exit(mode.get("exit_code",0))\n')
        self.decoder.chmod(0o700)
        self.tag = Tag(7, 1, 11)
        self.t = self.lib.dc_tts_create(C.byref(Config(str(self.broker.path).encode(), str(self.decoder).encode(), os.getpid())))
        self.assertTrue(self.t)
        self.addCleanup(self.shutdown)

    def write_mode(self):
        (self.path / 'mode').write_text(json.dumps(self.mode))

    def shutdown(self):
        if not self.t:
            return
        self.lib.dc_tts_shutdown(self.t, now())
        until = time.monotonic() + 1
        while self.lib.dc_tts_retired(self.t) and time.monotonic() < until:
            self.lib.dc_tts_tick(self.t, now())
            time.sleep(.005)
        self.assertEqual(self.lib.dc_tts_destroy(self.t), 0)
        self.t = None

    def submit(self, sequence=1):
        return self.lib.dc_tts_submit(self.t, self.tag, sequence, '北京故宫位于北京市中心。'.encode(), now())

    def step(self, events, chunks=None, activate=True):
        event = Event()
        rc = self.lib.dc_tts_poll(self.t, now(), C.byref(event))
        self.assertGreaterEqual(rc, 0)
        if rc:
            events.append((event.type, event.segment_sequence, event.error, event.tag.reply_generation))
        if activate and not self.lib.dc_tts_decoder_active(self.t):
            tag, sequence, ready = Tag(), C.c_uint64(), C.c_int()
            if self.lib.dc_tts_next(self.t, C.byref(tag), C.byref(sequence), C.byref(ready)) == 1 and ready.value:
                self.assertEqual(self.lib.dc_tts_activate(self.t, tag, sequence.value, now()), 0)
        if chunks is not None:
            view = PCM()
            if self.lib.dc_tts_peek_pcm(self.t, C.byref(view)):
                self.assertTrue(view.bytes % 2 == 0 and view.bytes <= 4096)
                chunks.append((view.segment_sequence, view.rate, C.string_at(view.data, view.bytes)))
                self.assertEqual(self.lib.dc_tts_consume_pcm(self.t, view.tag, view.segment_sequence, view.bytes), 0)

    def run_to_end(self, timeout=5):
        events, chunks = [], []
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            self.step(events, chunks)
            if not self.lib.dc_tts_slots(self.t):
                return events, chunks
            time.sleep(.001)
        self.fail('TTS fixture did not complete')

    def wait_urls(self, count):
        events = []
        until = time.monotonic() + 2
        while time.monotonic() < until:
            self.step(events, activate=False)
            if len([e for e in events if e[0] == 1]) == count:
                return events
            time.sleep(.001)
        self.fail('no prefetched URL')

    def test_warm_and_two_prefetched_slots_use_no_decoder_until_activate(self):
        self.assertEqual(self.lib.dc_tts_warm(self.t, self.tag, now()), 0)
        self.assertEqual(self.submit(1), 0)
        self.assertEqual(self.submit(2), 0)
        self.assertEqual(self.submit(3), -1)
        events = self.wait_urls(2)
        self.assertFalse((self.path / 'seen').exists())
        self.assertIn(4, [e[0] for e in events])
        events, chunks = self.run_to_end()
        self.assertEqual([e[:2] for e in events if e[0] == 2], [(2, 1), (2, 2)])
        expected = struct.pack('<' + 'h' * len(self.mode['samples']), *self.mode['samples'])
        for sequence in (1, 2):
            self.assertEqual(b''.join(c[2] for c in chunks if c[0] == sequence), expected)
        self.assertEqual(len((self.path / 'seen').read_text().splitlines()), 2)
        self.assertEqual(self.lib.dc_tts_storage_bytes() < 64 * 1024, True)

    def test_fragmented_header_pcm_frames_and_stereo_downmix(self):
        self.mode.update(channels=2, samples=[32767, 32767, -32768, -32768, 30000, -30000, 1000, 2000] * 20,
                         chunk=3, streaming=True)
        self.write_mode()
        self.assertEqual(self.submit(), 0)
        events, chunks = self.run_to_end()
        self.assertEqual([e[0] for e in events if e[0] in (2, 3)], [2])
        self.assertTrue(all(c[1] == 16000 for c in chunks))
        expected = struct.pack('<hhhh', 32767, -32768, 0, 1500) * 20
        self.assertEqual(b''.join(c[2] for c in chunks), expected)

    def test_pcm_backpressure_peek_is_stable_and_output_stays_bounded(self):
        self.mode['samples'] = [1234, -1234] * 40000
        self.write_mode()
        self.assertEqual(self.submit(), 0)
        self.wait_urls(1)
        self.assertEqual(self.lib.dc_tts_activate(self.t, self.tag, 1, now()), 0)
        events = []
        until = time.monotonic() + .3
        while time.monotonic() < until:
            self.step(events, activate=False)
            time.sleep(.001)
        a, b = PCM(), PCM()
        self.assertEqual(self.lib.dc_tts_peek_pcm(self.t, C.byref(a)), 1)
        saved = C.string_at(a.data, a.bytes)
        for _ in range(5):
            self.step(events, activate=False)
        self.assertEqual(self.lib.dc_tts_peek_pcm(self.t, C.byref(b)), 1)
        self.assertEqual((b.data, b.bytes, C.string_at(b.data, b.bytes)), (a.data, a.bytes, saved))
        events, chunks = self.run_to_end()
        self.assertEqual([e[0] for e in events if e[0] in (2, 3)], [2])
        self.assertEqual(len(b''.join(c[2] for c in chunks)), 160000)

    def test_invalid_wav_and_incomplete_pcm_are_errors(self):
        for generation, option in enumerate((dict(bad_header=True), dict(length_delta=2),
                                                dict(odd=True), dict(exit_code=7)), 1):
            self.tag = Tag(7, generation, 11)
            self.mode.update(bad_header=False, length_delta=0, odd=False, exit_code=0)
            self.mode.update(option)
            self.write_mode()
            self.assertEqual(self.submit(), 0)
            events, _ = self.run_to_end()
            self.assertEqual([e[0] for e in events if e[0] in (2, 3)], [3])

    def test_wrong_generation_malformed_status_and_ancillary_are_rejected(self):
        for generation, options in enumerate((dict(wrong_generation=1), dict(truncated=True),
                                                 dict(status=True), dict(ancillary=True),
                                                 dict(url=b'file:///private')), 1):
            self.broker.options = options
            self.tag = Tag(7, generation, 11)
            self.assertEqual(self.submit(), 0)
            events, chunks = self.run_to_end()
            self.assertEqual([e[0] for e in events if e[0] in (2, 3)], [3])
            self.assertEqual(chunks, [])
        self.assertFalse((self.path / 'seen').exists())

    def test_cancel_clears_pcm_immediately_and_stale_cancel_preserves_followup(self):
        self.mode.update(ignore_term=True, tail=3)
        self.write_mode()
        self.assertEqual(self.submit(), 0)
        events = []
        until = time.monotonic() + 2
        view = PCM()
        while time.monotonic() < until:
            self.step(events)
            if self.lib.dc_tts_peek_pcm(self.t, C.byref(view)):
                break
            time.sleep(.001)
        self.assertTrue(view.bytes)
        self.assertEqual(self.lib.dc_tts_cancel(self.t, Tag(8, 1, 11), now()), 0)
        began = time.monotonic()
        self.assertEqual(self.lib.dc_tts_cancel(self.t, self.tag, now()), 1)
        self.assertLess(time.monotonic() - began, .02)
        self.assertEqual(self.lib.dc_tts_peek_pcm(self.t, C.byref(PCM())), 0)
        self.assertEqual(self.lib.dc_tts_slots(self.t), 0)
        self.assertEqual(self.lib.dc_tts_cancel(self.t, self.tag, now()), 0)
        old = self.tag
        self.tag = Tag(7, 2, 11)
        self.mode.update(ignore_term=False, tail=0)
        self.write_mode()
        self.assertEqual(self.submit(), 0)
        self.assertEqual(self.lib.dc_tts_cancel(self.t, old, now()), 0)
        events, chunks = self.run_to_end()
        self.assertTrue(all(e[3] == 2 for e in events))
        self.assertTrue(chunks)

    def test_peer_pid_and_sequence_checks(self):
        self.assertEqual(self.lib.dc_tts_destroy(self.t), 0)
        self.t = self.lib.dc_tts_create(C.byref(Config(str(self.broker.path).encode(), str(self.decoder).encode(), os.getpid() + 10000)))
        self.assertTrue(self.t)
        self.assertEqual(self.submit(), 0)
        events, chunks = self.run_to_end()
        self.assertEqual([e[0] for e in events if e[0] in (2, 3)], [3])
        self.assertFalse(chunks)
        self.assertEqual(self.submit(), -1)  # same sequence cannot replay

    def test_url_deadline_is_bounded_without_waiting_in_poll(self):
        self.broker.options = dict(silent=True)
        self.assertEqual(self.submit(), 0)
        event = Event()
        began = time.monotonic()
        self.assertEqual(self.lib.dc_tts_poll(self.t, now() + 15001, C.byref(event)), 1)
        self.assertLess(time.monotonic() - began, .02)
        self.assertEqual((event.type, event.error), (3, 5))

    def test_shutdown_closes_generation_zero_warm_without_a_greeting(self):
        self.broker.options = dict(silent=True)
        self.assertEqual(self.lib.dc_tts_warm(self.t, Tag(7, 0, 0), now()), 0)
        self.assertFalse(self.lib.dc_tts_quiescent(self.t))
        self.assertEqual(self.lib.dc_tts_destroy(self.t), -1)
        self.lib.dc_tts_shutdown(self.t, now())
        self.assertTrue(self.lib.dc_tts_quiescent(self.t))
        self.lib.dc_tts_shutdown(self.t, now())
        self.assertTrue(self.lib.dc_tts_quiescent(self.t))

    def test_controlled_real_url_decoder_http_mp3_to_exact_mono_pcm(self):
        # Only loopback is enabled inside the test's private network namespace.
        subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=True)
        binary = self.path / 'native-url-audio'
        subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(EXPERIMENTS / 'native-url-audio.c'), '-ldl', '-o', str(binary)], check=True)
        mp3 = self.path / 'fixture.mp3'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                        'sine=frequency=440:duration=0.5:sample_rate=16000', '-ac', '1',
                        '-codec:a', 'libmp3lame', '-b:a', '32k', '-write_xing', '0', str(mp3)], check=True)
        payload = mp3.read_bytes()
        baseline = subprocess.run([str(binary), '--decode-stdin'], input=payload, capture_output=True, check=True)

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            self.assertEqual(self.lib.dc_tts_destroy(self.t), 0)
            self.t = self.lib.dc_tts_create(C.byref(Config(str(self.broker.path).encode(), str(binary).encode(), os.getpid())))
            self.assertTrue(self.t)
            self.broker.options = dict(url=('http://127.0.0.1:' + str(server.server_port) + '/synthetic?signature=PUBLIC').encode())
            self.assertEqual(self.submit(), 0)
            events, chunks = self.run_to_end()
            self.assertEqual([e[0] for e in events if e[0] in (2, 3)], [2])
            self.assertEqual(b''.join(c[2] for c in chunks), baseline.stdout[44:])
            self.assertTrue(all(c[1] == 16000 for c in chunks))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=1)


if __name__ == '__main__':
    unittest.main(verbosity=2)

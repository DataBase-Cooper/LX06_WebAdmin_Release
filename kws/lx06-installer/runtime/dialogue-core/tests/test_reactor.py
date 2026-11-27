"""Real unified reactor with synthetic UNIX ASR/TTS/account/ALSA endpoints.

Run as root inside `unshare --net`; no cloud, speaker, DATA, firmware or door.
Only the ownership permit is compiled with DC_CORE_HOST_FIXTURE. Production
main, ASR+Opus, IO, session policy, account parser, TTS and VAD are unmodified.
Reports retain exact source/binary hashes and distinguish this from acoustics.
"""
import argparse
import ctypes as C
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'runtime/common/tests'))
from fake_asr import SpeakerASR

HEADER = struct.Struct('<IHHQQII')
MAGIC = 0x31544e4c
MODEL = '''import json,os,signal,sys,time
from pathlib import Path
root=Path(os.environ['DC_FIXTURE_DIRECTORY'])
request=json.load(sys.stdin)
with (root/'requests').open('a') as f: f.write(json.dumps(request,ensure_ascii=False)+'\\n')
mode=json.loads((root/'mode.json').read_text())
def emit(text): print(json.dumps({'delta':text},ensure_ascii=False),flush=True)
if mode['pattern']=='gap':
 emit('第一段用于检查实际播放。');time.sleep(.85);emit('第二段继续播放。')
else: emit('这是用于检查打断后能够继续对话的合成回答。')
print('{"done":true}',flush=True)
'''
DECODER = '''#!/usr/bin/python3
import json,os,signal,struct,sys
from pathlib import Path
from urllib.parse import urlsplit,parse_qs
root=Path(os.environ['DC_FIXTURE_DIRECTORY'])
query=parse_qs(urlsplit(sys.stdin.buffer.read().decode()).query)
sample=int(query['sample'][0]);frames=int(query['frames'][0])
birth=int(Path('/proc/self/stat').read_text().rsplit(')',1)[1].split()[19])
with (root/'decoder-pids').open('a') as f: f.write(str(os.getpid())+' '+str(birth)+'\\n')
if query.get('ignore_term')==['1']:signal.signal(signal.SIGTERM,signal.SIG_IGN)
size=frames*2
sys.stdout.buffer.write(struct.pack('<4sI4s4sIHHIIHH4sI',b'RIFF',size+36,b'WAVE',b'fmt ',16,1,1,16000,32000,2,16,b'data',size));sys.stdout.buffer.flush()
chunk=struct.pack('<h',sample)*160
for i in range(0,frames,160):
 sys.stdout.buffer.write(chunk[:min(160,frames-i)*2]);sys.stdout.buffer.flush()
'''


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wait(predicate, seconds=3, label='condition'):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(.005)
    raise AssertionError('timed out: ' + label)


def lines(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line]


class Broker:
    def __init__(self, root, duration, warm_silent, ignore_term):
        self.path = root / 'tts.sock'
        self.duration, self.warm_silent, self.ignore_term = duration, warm_silent, ignore_term
        self.requests, self.closed, self.errors, self.children = [], [], [], []
        self.lock = threading.Lock()
        self.stopped = threading.Event()
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.socket.bind(str(self.path)); self.path.chmod(0o600)
        self.socket.listen(8); self.socket.settimeout(.05)
        self.thread = threading.Thread(target=self.serve, daemon=True); self.thread.start()

    def serve(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            thread = threading.Thread(target=self.handle, args=(connection,), daemon=True)
            self.children.append(thread); thread.start()

    def handle(self, connection):
        with connection:
            connection.settimeout(.05)
            try:
                data = connection.recv(8192)
                if not data:
                    return  # bootstrap SO_PEERCRED readiness probe, no SDK request
                magic, version, kind, rid, generation, length, code = HEADER.unpack(data[:32])
                assert (magic, version, rid, code) == (MAGIC, 1, 0, 0)
                assert len(data) == length + 32
                with self.lock:
                    index = len(self.requests)
                    record = dict(kind=kind, generation=generation, bytes=length,
                                  text=data[32:].decode(), at_ns=time.monotonic_ns())
                    self.requests.append(record)
                    number = sum(r['kind'] == 2 for r in self.requests)
                if kind == 1:
                    if self.warm_silent:
                        while not self.stopped.is_set():
                            try:
                                if not connection.recv(1):
                                    self.closed.append(index); return
                            except socket.timeout:
                                pass
                        return
                    response = HEADER.pack(MAGIC, 1, 16, index + 1, generation or 1, 0, 0)
                else:
                    assert kind == 2
                    payload = ('https://fixture.invalid/audio?sample=' + str(1000 + number) +
                               '&frames=' + str(int(self.duration * 16000)) +
                               '&ignore_term=' + str(int(self.ignore_term))).encode()
                    response = HEADER.pack(MAGIC, 1, 17, index + 1, generation, len(payload), 0) + payload
                connection.send(response)
            except (OSError, ValueError):
                pass  # owned cancellation closes requests deliberately
            except BaseException as error:
                self.errors.append(type(error).__name__)

    def close(self):
        self.stopped.set(); self.socket.close(); self.thread.join(1)
        for thread in self.children:
            thread.join(1)


class Reactor:
    last_failure = None
    def __init__(self, binary, library, pattern='long', duration=4, warm_silent=False,
                 first_voice=False, ignore_term=False, clock_step=False, greeting=''):
        self.binary, self.library = binary, library
        self.pattern, self.duration, self.warm_silent = pattern, duration, warm_silent
        self.first_voice, self.ignore_term = first_voice, ignore_term
        self.clock_step = clock_step
        self.greeting = greeting
        self.events, self.light, self.errors = [], [], []
        self.asr_numeric_diagnostics = 0
        self.stopped = threading.Event(); self.process = None

    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='dc-reactor-')
        self.root = Path(self.temporary.name); self.root.chmod(0o700)
        self.mic = self.root / 'mic'; self.mic.mkdir(mode=0o700)
        (self.root / 'mode.json').write_text(json.dumps(dict(pattern=self.pattern)))
        (self.root / 'request.py').write_text(MODEL)
        self.script = self.root / 'account.sh'
        self.script.write_text('#!/bin/sh\nexec /usr/bin/python3 ' + shlex.quote(str(self.root / 'request.py')) + '\n')
        self.decoder = self.root / 'decoder'; self.decoder.write_text(DECODER); self.decoder.chmod(0o700)
        self.asr = SpeakerASR().__enter__()
        self.broker = Broker(self.root, self.duration, self.warm_silent, self.ignore_term)
        self.light_send, self.light_read = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.light_read.settimeout(.05)
        env = dict(os.environ, DC_HOST_FIXTURE='1', DC_FIXTURE_DIRECTORY=str(self.root),
                   DC_MIC_DIRECTORY=str(self.mic), DC_MIC_FORMAT='LXA2', DC_PCM_DEVICE='synthetic',
                   DC_FAKE_ALSA_LEDGER=str(self.root / 'alsa'), DC_MAX_SECONDS='30',
                   LD_LIBRARY_PATH=str(self.library), MODEL_GREETING=self.greeting, MODEL_BARGE_ANY='0',
                   MODEL_INTERRUPT='等一下,等等,停一下,你说得不对,你说的不对',
                   MODEL_GOODBYE='结束对话,拜拜了哦', MODEL_MIC_GAIN_DB='0',
                   LX06_CONTROL_ASR_SOCKET=self.asr.path, LX06_NATIVE_TTS_SOCKET=str(self.broker.path),
                   LX06_NATIVE_TTS_DECODER=str(self.decoder), LX06_DIALOG_FD=str(self.light_send.fileno()),
                   LX06_DIALOG_CHANNEL=os.readlink('/proc/self/fd/' + str(self.light_send.fileno())))
        if self.clock_step:
            preload = str(self.library / 'libfake-clock.so')
            if json.loads((self.binary.parent / 'manifest.json').read_text())['sanitized']:
                asan = subprocess.check_output(['gcc', '-print-file-name=libasan.so'], text=True).strip()
                preload = asan + ':' + preload
            env['LD_PRELOAD'] = preload
        self.process = subprocess.Popen([self.binary, '--account', self.script, 'synthetic-selected-model'],
                                        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                        pass_fds=[self.light_send.fileno()], start_new_session=True)
        self.log_thread = threading.Thread(target=self.read_log, daemon=True); self.log_thread.start()
        self.light_thread = threading.Thread(target=self.read_light, daemon=True); self.light_thread.start()
        wait(lambda: (self.mic / 'account.a2').exists() or self.process.poll() is not None,
             label='reactor subscription')
        assert self.process.poll() is None, self.events
        self.mic_thread = threading.Thread(target=self.send_mic, daemon=True); self.mic_thread.start()
        wait(lambda: self.asr.audio_packets > 0, label='actual Opus upload')
        return self

    def read_log(self):
        for raw in self.process.stderr:
            line = raw.decode(errors='replace').strip()
            match = re.fullmatch(r'dialogue-core: event=(\w+) value=(\d+) at=(\d+)', line)
            if match:
                self.events.append(dict(event=match[1], value=int(match[2]), at_ms=int(match[3])))
            elif line:
                try:
                    diagnostic = json.loads(line)
                except ValueError:
                    diagnostic = {}
                if diagnostic.get('type') == 'question_rollover_numeric':
                    self.asr_numeric_diagnostics += 1
                else:
                    self.errors.append(line[:200])

    def read_light(self):
        while not self.stopped.is_set():
            try:
                data = self.light_read.recv(16)
                self.light.append(dict(code=data[0], at_ns=time.monotonic_ns()))
            except socket.timeout:
                pass
            except OSError:
                break

    def send_mic(self):
        sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)
        birth = int(Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19])
        next_at = time.monotonic(); sequence = 0
        try:
            while not self.stopped.is_set() and self.process.poll() is None:
                sequence += 1
                samples = [int(4000 * math.sin(2 * math.pi * 440 * i / 16000))
                           for i in range(160)] if sequence == 1 and self.first_voice else [0] * 160
                pcm = struct.pack('<160h', *samples)
                packet = struct.pack('<IHHIIIIIIQQQ', 0x3241584c, 2, 56, 56 + len(pcm),
                                     16000, 160, 1, os.getpid(), 0, birth, sequence,
                                     int(time.monotonic() * 1000)) + pcm
                try:
                    sender.sendto(packet, str(self.mic / 'account.a2'))
                except OSError as error:
                    if self.process.poll() is None and not self.stopped.is_set():
                        # Session close removes its socket before process exit.
                        if error.errno not in (2, 111):
                            self.errors.append('microphone-send-' + str(error.errno))
                    break
                next_at += .01
                self.stopped.wait(max(0, next_at - time.monotonic()))
        finally:
            sender.close()

    def event(self, name, count=1, seconds=3):
        return wait(lambda: [e for e in self.events if e['event'] == name]
                    if len([e for e in self.events if e['event'] == name]) >= count else None,
                    seconds, 'event-' + name)

    def ledger(self):
        return lines(self.root / 'alsa')

    def requests(self):
        return lines(self.root / 'requests')

    def final(self, text, dialog=None):
        return self.asr.emit(text, final=True, dialog=dialog)

    def question(self):
        self.final('请用两句话介绍北京故宫。')
        self.event('first_pcm')

    def assert_alive(self):
        assert self.process.poll() is None, (self.events, self.errors)
        assert not [e for e in self.events if e['event'] in ('account_error', 'tts_error', 'player_error', 'cleanup_timeout')]

    def finish(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        assert self.process.wait(3) == 0, (self.events, self.errors)
        self.log_thread.join(1)
        wait(lambda: any(l['code'] == 0 for l in self.light), label='explicit CLOSED light')
        states = [e['value'] for e in self.events if e['event'] == 'state']
        assert states.count(6) == states.count(0) == 1, states
        assert sum(l['code'] == 0 for l in self.light) == 1
        assert not self.broker.errors and not self.errors, (self.broker.errors, self.errors)

    def snapshot(self):
        return dict(events=self.events, light_codes=[l['code'] for l in self.light],
                    accepted_frames=sum(e['frames'] for e in self.ledger() if e['op'] == 'WRITE'),
                    drops=sum(e['op'] == 'DROP' for e in self.ledger()),
                    account_requests=len(self.requests()), asr_opus_packets=self.asr.audio_packets,
                    broker_requests=len(self.broker.requests), asr_numeric_diagnostics=self.asr_numeric_diagnostics,
                    clock_step_for_close_boundary=self.clock_step)

    def __exit__(self, *args):
        try:
            if args[0]:
                Reactor.last_failure = dict(snapshot=self.snapshot(), errors=self.errors)
            if self.process and self.process.poll() is None:
                self.process.send_signal(signal.SIGTERM)
                try:
                    self.process.wait(3)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL); self.process.wait(1)
            self.stopped.set()
            for name in ('mic_thread', 'light_thread', 'log_thread'):
                thread = getattr(self, name, None)
                if thread:
                    thread.join(1)
            self.light_send.close(); self.light_read.close()
            self.broker.close(); self.asr.__exit__(*args)
            if self.process.stderr:
                self.process.stderr.close()
        finally:
            self.temporary.cleanup()


def complete_final(binary, library):
    with Reactor(binary, library) as r:
        dialog = r.asr.emit('请用两句话', final=False)
        time.sleep(.12)
        assert not r.requests(), 'partial was incorrectly submitted'
        r.final('请用两句话介绍北京故宫。', dialog)
        wait(lambda: r.requests(), label='complete account question')
        assert r.requests()[0]['input'][-1]['content'] == '请用两句话介绍北京故宫'
        r.event('first_pcm'); r.finish(); return r.snapshot()


def first_frame(binary, library):
    with Reactor(binary, library, first_voice=True) as r:
        wait(lambda: r.asr.audio_packets > 2)
        packet = r.asr.audio_by_dialog[r.asr.start_packets[0][1]][0]
        opus = C.CDLL('libopus.so.0'); error = C.c_int()
        opus.opus_decoder_create.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]
        opus.opus_decoder_create.restype = C.c_void_p
        opus.opus_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_void_p, C.c_int, C.c_int]
        opus.opus_decoder_destroy.argtypes = [C.c_void_p]
        decoder = opus.opus_decoder_create(16000, 1, C.byref(error)); assert decoder and not error.value
        pcm = (C.c_int16 * 960)()
        try:
            samples = opus.opus_decode(decoder, packet, len(packet), pcm, 960, 0)
            assert samples > 0
            energy = sum(int(pcm[i]) ** 2 for i in range(samples))
            assert energy > 100000, 'first framed voice PCM was discarded'
        finally:
            opus.opus_decoder_destroy(decoder)
        r.finish(); result = r.snapshot(); result['first_opus_energy'] = energy; return result


def segment_gap(binary, library):
    with Reactor(binary, library, pattern='gap', duration=.12) as r:
        r.question(); r.event('pcm_resumed', seconds=4)
        wait(lambda: any(e['event'] == 'state' and e['value'] == 2 for e in r.events[4:]),
             seconds=4, label='whole reply listening')
        assert not any(l['code'] == 0 for l in r.light)
        assert len([q for q in r.broker.requests if q['kind'] == 2]) == 2
        states = [e['value'] for e in r.events if e['event'] == 'state']
        first = states.index(4)
        assert 3 in states[first + 1:] and states.count(4) >= 2, states
        r.finish(); return r.snapshot()


def interrupt(binary, library, repeated=False, stale=False):
    with Reactor(binary, library, ignore_term=stale) as r:
        r.question()
        dialog = wait(lambda: r.asr.dialog if r.asr.dialog and r.asr.dialog != r.asr.last_sent else None,
                      label='fresh control recognition turn')
        start = time.monotonic_ns()
        r.asr.emit('等一下', final=False, dialog=dialog)
        drops = wait(lambda: [e for e in r.ledger() if e['op'] == 'DROP'], label='local immediate drop')
        delay_ms = (drops[0]['at_ns'] - start) / 1e6
        assert 0 <= delay_ms < 100, delay_ms
        if repeated:
            for phrase in ('等一下，停一下', '等一下，停一下，你说得不对', '等等，等一下，停一下'):
                r.asr.emit(phrase, final=False, dialog=dialog); time.sleep(.02)
        assert len([e for e in r.ledger() if e['op'] == 'DROP']) == 1
        final = '等一下，停一下，你说得不对，继续介绍天坛。' if repeated else '等一下，接着介绍颐和园。'
        expected = '继续介绍天坛' if repeated else '接着介绍颐和园'
        r.final(final, dialog)
        wait(lambda: len(r.requests()) == 2, label='next complete question')
        assert r.requests()[1]['input'][-1]['content'] == expected
        r.event('first_pcm', count=2)
        if stale:
            r.asr.emit('结束对话', final=True, dialog=dialog)
            time.sleep(.15)
        r.assert_alive()
        ledger = r.ledger(); drop_ns = drops[0]['at_ns']
        assert not [e for e in ledger if e['op'] == 'WRITE' and e['at_ns'] > drop_ns and e['sample'] == 1001]
        assert len([e for e in ledger if e['op'] == 'DROP']) == 1
        r.finish(); result = r.snapshot(); result['control_to_drop_ms'] = delay_ms
        result['control_drops_before_close'] = 1; return result


def goodbye(binary, library):
    with Reactor(binary, library) as r:
        r.question()
        r.asr.emit('结束对话，拜拜了哦，结束对话', final=False)
        wait(lambda: r.process.poll() is not None, label='end-word closes session')
        r.finish()
        assert len([e for e in r.ledger() if e['op'] == 'DROP']) == 1
        return r.snapshot()


def warm_zero(binary, library):
    with Reactor(binary, library, warm_silent=True, clock_step=True) as r:
        wait(lambda: r.broker.requests, label='generation zero warm')
        assert r.broker.requests[0]['kind'] == 1 and r.broker.requests[0]['generation'] == 0
        r.finish(); wait(lambda: r.broker.closed, label='warm socket closed')
        assert not r.requests()
        return r.snapshot()


def process_exit_status(binary, library):
    # Execute the real reactor in an isolated subprocess. Both endpoints are
    # deliberately absent; the failed startup must traverse reactor cleanup.
    with tempfile.TemporaryDirectory(prefix='dc-exit-') as directory:
        root = Path(directory)
        env = dict(os.environ, DC_HOST_FIXTURE='1', DC_TRACE_STARTUP='1',
                   DC_MIC_DIRECTORY=str(root / 'missing' / 'mic'),
                   LX06_CONTROL_ASR_SOCKET=str(root / 'missing-asr.sock'),
                   DC_PCM_DEVICE='synthetic', LD_LIBRARY_PATH=str(library))
        failed = subprocess.run([binary, '--account', str(root / 'unused'), 'fixture'],
                                env=env, capture_output=True, timeout=8)
        stderr = failed.stderr.decode(errors='replace')
        assert failed.returncode == 1, (failed.returncode, stderr)
        assert 'dialogue-core: startup_stage=10\n' in stderr, stderr
        protocol = subprocess.run([binary, '--protocol'], env=env, capture_output=True, timeout=3)
        assert protocol.returncode == 0 and protocol.stdout == b'LX06DIALOGUE/1\n'
        invalid = subprocess.run([binary, '--invalid'], env=env, capture_output=True, timeout=3)
        assert invalid.returncode == 2, invalid.returncode
        return dict(failed_startup_status=failed.returncode, protocol_status=protocol.returncode,
                    invalid_arguments_status=invalid.returncode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-name', required=True)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0 or [name for _, name in socket.if_nameindex()] != ['lo']:
        raise RuntimeError('run as root inside a private unshare --net namespace')
    output = ROOT / 'build' / args.output_name; output.mkdir(exist_ok=False)
    report = dict(passed=False, actual_device=False, actual_audio=False, cloud_requests=0,
                  ownership_bypass='host fixture only', cases=[], sources={})
    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    if args.binary:
        binary = args.binary.resolve()
    else:
        build = ROOT / 'build' / (args.output_name + '-core')
        command = ['/usr/bin/python3', ROOT / 'runtime/dialogue-core/build.py', '--fixture', '--output', build]
        if args.sanitize:
            command.append('--sanitize')
        subprocess.run(command, check=True, stdout=subprocess.PIPE)
        binary = build / 'lx06-dialogued'
    manifest = json.loads((binary.parent / 'manifest.json').read_text())
    report['binary_manifest'] = str(binary.parent / 'manifest.json')
    report['binary_manifest_sha256'] = sha(binary.parent / 'manifest.json')
    report['build_source_unchanged'] = manifest['source_unchanged']
    if not (manifest['passed'] and manifest['fixture'] and not manifest['arm']):
        report['failure'] = dict(phase='build-manifest', source_unchanged=manifest['source_unchanged'])
        save()
        raise AssertionError('requires an unchanged successful host fixture build')
    assert sha(binary) == manifest['sha256']
    library = output / 'fake'; library.mkdir()
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared']
    if manifest['sanitized']:
        flags += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(['gcc', *flags, HERE / 'fake-asound.c', '-o', library / 'libasound.so.2'], check=True)
    subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                    '-DDC_FAKE_CLOCK', HERE / 'fake-asound.c', '-o', library / 'libfake-clock.so'], check=True)
    report.update(binary_sha256=sha(binary), binary_manifest=str(binary.parent / 'manifest.json'),
                  binary_manifest_sha256=sha(binary.parent / 'manifest.json'),
                  fake_asound_sha256=sha(library / 'libasound.so.2'),
                  fake_clock_sha256=sha(library / 'libfake-clock.so'), sanitized=manifest['sanitized'])
    report['sources'] = {str(path.relative_to(ROOT)): sha(path) for path in
                         [HERE / 'fake-asound.c', Path(__file__), ROOT / 'runtime/common/tests/fake_asr.py',
                          ROOT / 'runtime/dialogue-core/build.py']}
    save()
    cases = [('process-exit-status', process_exit_status),
             ('complete-final', complete_final), ('first-frame-preserved', first_frame),
             ('two-segment-gap', segment_gap), ('single-interrupt-followup', interrupt),
             ('mixed-repeat-interrupt', lambda b, l: interrupt(b, l, repeated=True)),
             ('mixed-goodbye', goodbye), ('generation-zero-warm-close', warm_zero),
             ('stale-final-and-pcm', lambda b, l: interrupt(b, l, stale=True))]
    for name, function in cases:
        try:
            snapshot = function(binary, library)
            report['cases'].append(dict(case=name, passed=True, **snapshot)); save()
        except BaseException as error:
            report['failure'] = dict(case=name, type=type(error).__name__, message=str(error)[-3000:],
                                     fixture=Reactor.last_failure); save()
            raise
    report['source_unchanged'] = all(sha(ROOT / path) == value for path, value in report['sources'].items())
    report['binary_sources_current'] = all(sha(ROOT / path) == value for path, value in manifest['sources'].items())
    report['passed'] = report['source_unchanged'] and report['binary_sources_current']; save()
    print(json.dumps(dict(passed=report['passed'], cases=len(report['cases']), report_sha256=sha(output / 'report.json'))))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

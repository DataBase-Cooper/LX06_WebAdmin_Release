"""Offline real supervisor/reactor/player with failed synthetic ALSA drop.

Run in a private mount and network namespace with TMPDIR on POSIX tmpfs.
The synthetic drop does not establish physical silence.
"""
import os
import errno
import importlib.util
import json
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from test_audio_reactor import AudioReactor, wait

assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net')
with tempfile.TemporaryDirectory(prefix='dc-supervised-') as tmp:
    root = Path(tmp)
    # Compile the current production translation units, never a stored binary.
    spec = importlib.util.spec_from_file_location('core_build', HERE.parent / 'build.py')
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    core = root / 'lx06-dialogued'
    subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-DDC_CORE_HOST_FIXTURE', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie',
                    '-I' + str(HERE.parent / 'vendor/libfvad/include'),
                    '-I' + str(HERE.parent / 'vendor/libfvad/src'),
                    *build.sources(), '-ldl', '-lopus', '-lm', '-o', core], check=True)
    (root / 'manifest.json').write_text(json.dumps(dict(sanitized=True, fixture=True,
        sha256=hashlib.sha256(core.read_bytes()).hexdigest())))
    adapter_dir = root / 'adapter'
    subprocess.run(['bash', HERE.parent / 'adapters/build.sh', 'host'], check=True,
                   env=dict(os.environ, DC_ADAPTER_BUILD_DIR=str(adapter_dir)))
    adapter = adapter_dir / 'lx06-transport-adapter'
    owner = root / 'lease'
    supervisor = root / 'session-run'
    library = root / 'lib'
    library.mkdir()
    src = (HERE / 'fake-asound.c').read_text()
    old = 'p->queued=0;p->at=ns();record(p,"DROP",0,0);return 0;}'
    assert src.count(old) == 1
    src = src.replace(old, 'record(p,"DROP",0,0);return -EIO;}')
    fake = root / 'fake.c'
    fake.write_text(src)
    subprocess.run(['gcc','-O1','-g','-Wall','-Wextra','-Werror','-fPIC','-shared',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',fake,
                    '-o',library/'libasound.so.2'],check=True)
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror',
                    '-DLX06_SESSION_FILE="'+str(owner)+'"',
                    ROOT/'hooks/xiaoai/session-run.c','-o',supervisor],check=True)
    real_popen = subprocess.Popen
    def supervised(args, *a, **kw):
        if str(args[0]) == str(core):
            args = [supervisor, 'run', 'test', *args]
        return real_popen(args, *a, **kw)
    subprocess.Popen = supervised
    class DiagnosticReactor(AudioReactor):
        def __enter__(self):
            try:
                return super().__enter__()
            except Exception:
                if self.process:
                    self.process.wait(timeout=3)
                    self.log_thread.join(1)
                    print('startup status', self.process.returncode, 'errors', self.errors, file=sys.stderr)
                raise
    try:
        with DiagnosticReactor(core, library, adapter, 'doubao', '0') as r:
            assert subprocess.run([supervisor,'busy']).returncode == 0
            lease = owner.read_bytes()
            assert len(lease) >= 12
            child = struct.unpack_from('=I', lease, 8)[0]
            assert child > 1 and Path('/proc',str(child)).exists()
            assert len(list(Path('/proc',str(child),'task').iterdir())) >= 2, 'player worker missing'
            r.final('请用两句话介绍北京故宫。')
            wait(lambda:r.cloud.commits == 1, label='question commit')
            r.cloud.reply(1,long=True)
            r.event('first_pcm')
            dialog = wait(lambda:r.asr.dialog if r.asr.dialog and r.asr.dialog != r.asr.last_sent else None, label='interrupt utterance')
            r.asr.emit('等一下',final=False,dialog=dialog)
            assert r.event('player_drop_error')[0]['value'] == errno.EIO
            assert len(r.event('cancel_queued')) == 1
            assert any(e['op']=='DROP' for e in r.ledger())
            assert not any(e['event']=='player_dropped' for e in r.events)
            result = r.process.wait(timeout=6)
            r.log_thread.join(1)
            assert result == 1, (result,r.events,r.errors)
            assert not any(e['event']=='player_dropped' for e in r.events)
            assert subprocess.run([supervisor,'busy']).returncode == 1
            assert owner.stat().st_size == 0
            assert not Path('/proc',str(child)).exists(), 'reactor or worker not reaped'
            assert not any('AddressSanitizer' in x or 'runtime error:' in x for x in r.errors), r.errors
            print('PASS real supervisor/reactor: failed drop, no STOPPED, bounded exit, lease released; physical silence unknown')
    finally:
        subprocess.Popen = real_popen

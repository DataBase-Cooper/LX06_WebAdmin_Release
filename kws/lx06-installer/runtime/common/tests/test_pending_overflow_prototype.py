"""Offline experiment only: transform temporary copies of the actual ASR TUs.

Run on Linux with gcc/libopus and TMPDIR under a private scratch location.
No production file is modified. The candidate covers deferred-feed overflow,
not general feed atomicity, retry scheduling, or a real user-visible reporter.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SOURCES = (ROOT / 'runtime/common/control_asr.c',
           ROOT / 'runtime/dialogue-core/asr/control_asr.c')


def candidate(text):
    old = 'static void defer_audio(lx06_control_asr *a,const void *pcm,size_t n) {'
    assert text.count(old) == 1
    text = text.replace(old, old.replace('void defer_audio', 'int defer_audio'))
    start = text.index('static int defer_audio(')
    end = text.index('\n}', start) + 2
    f = text[start:end]
    needle = ' size_t capacity=pending_capacity(a);\n'
    assert f.count(needle) == 1
    f = f.replace(needle, needle + ' if(n>capacity-a->pending_used){errno=ENOBUFS;return -2;}\n')
    assert f.count('a->pending_used+=n;return;') == 1
    f = f.replace('a->pending_used+=n;return;', 'a->pending_used+=n;return 0;')
    f = f[:-2] + '\n return 0;\n}'
    text = text[:start] + f + text[end:]
    call = 'defer_audio(a,pcm,n);return 0;'
    assert text.count(call) == 4
    return text.replace(call, 'return defer_audio(a,pcm,n);')


def run():
    with tempfile.TemporaryDirectory(prefix='asr-overflow-prototype-') as tmp:
        tmp = Path(tmp)
        for idx, source in enumerate(SOURCES):
            original = source.read_text()
            for variant, text in (('original', original), ('candidate', candidate(original))):
                copy = tmp / f'{idx}-{variant}.c'
                copy.write_text(text)
                exe = tmp / f'{idx}-{variant}'
                cmd = ['gcc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                       '-I', str(source.parent), f'-DCONTROL_ASR_SOURCE="{copy}"',
                       str(HERE / 'pending_overflow_prototype.c'),
                       str(ROOT / 'runtime/doubao/src/json.c'),
                       '-Wl,--wrap=malloc', '-lopus', '-o', str(exe)]
                subprocess.run(cmd, check=True)
                for profile, oom in ((0, 0), (1, 0), (1, 1)):
                    env = os.environ.copy()
                    env.pop('LX06_CONTROL_ASR_TRACE', None)
                    p = subprocess.run([str(exe), str(profile), str(oom)], env=env,
                                       capture_output=True, text=True, timeout=5)
                    print(source.parent.name, variant, profile, oom,
                          'rc=', p.returncode, (p.stdout+p.stderr).strip(), flush=True)
                    expected = 1 if variant == 'original' else 0
                    if p.returncode != expected:
                        raise RuntimeError('unexpected test result')


if __name__ == '__main__':
    run()

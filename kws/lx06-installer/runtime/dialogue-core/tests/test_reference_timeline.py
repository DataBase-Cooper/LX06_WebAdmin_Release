"""Build the offline production wrapper/guard/player timeline on Linux."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

CORE = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie'] if a.sanitize else []
sources = ['tests/test_reference_timeline.c', 'io/player.c', 'media.c', 'session.c',
           'control.c', 'tts.c', 'audio_route.c', 'protocol.c', 'voice_guard.c',
           '../doubao/src/text_match.c', '../doubao/src/json.c']
sources += [str(p.relative_to(CORE)) for p in sorted((CORE / 'vendor/libfvad/src').rglob('*.c'))]
with tempfile.TemporaryDirectory(prefix='dc-reference-') as directory:
    binary = Path(directory) / 'fixture'
    subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                    '-Ivendor/libfvad/include', '-Ivendor/libfvad/src', *flags,
                    *sources, '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(binary)],
                   cwd=CORE, check=True)
    subprocess.run([binary], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))

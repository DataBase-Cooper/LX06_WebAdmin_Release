"""Linux offline fault injection, actual core wrapper/player/voice guard."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

core = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie'] if args.sanitize else []
sources = ['tests/test_player_reference_fault.c', 'io/player.c', 'media.c', 'session.c',
           'control.c', 'tts.c', 'audio_route.c', 'protocol.c', 'voice_guard.c',
           '../doubao/src/text_match.c', '../doubao/src/json.c']
sources += [str(p.relative_to(core)) for p in sorted((core / 'vendor/libfvad/src').rglob('*.c'))]
with tempfile.TemporaryDirectory(prefix='dc-player-fault-') as directory:
    binary = Path(directory) / 'fixture'
    subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                    '-Ivendor/libfvad/include', '-Ivendor/libfvad/src', *flags,
                    *sources, '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(binary)],
                   cwd=core, check=True)
    subprocess.run([binary], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))

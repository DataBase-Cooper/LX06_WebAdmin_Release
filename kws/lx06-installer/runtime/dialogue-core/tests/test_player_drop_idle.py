"""Offline real player drop-failure and drained-idle regressions."""
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
with tempfile.TemporaryDirectory(prefix='dc-drop-idle-') as directory:
    binary = Path(directory) / 'fixture'
    subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', *flags, 'tests/test_player_drop_idle.c',
                    'io/player.c', '-Wl,--wrap=poll', '-pthread', '-o', str(binary)],
                   cwd=core, check=True)
    for case in ('drop', 'idle', 'errno'):
        subprocess.run([binary, case], check=True,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'), timeout=10)

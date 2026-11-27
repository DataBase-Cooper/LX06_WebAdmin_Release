"""Offline real reactor/player stop regression; Linux host, no device or network."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
CORE = HERE.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie'] if args.sanitize else []
    sources = ['tests/test_reactor_player_stop.c', 'io/player.c', 'media.c',
               'session.c', 'control.c', 'tts.c', 'audio_route.c', 'protocol.c',
               '../doubao/src/text_match.c', '../doubao/src/json.c']
    with tempfile.TemporaryDirectory(prefix='dc-stop-') as directory:
        binary = Path(directory) / 'fixture'
        command = ['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                   *flags, *sources, '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(binary)]
        subprocess.run(command, cwd=CORE, check=True)
        subprocess.run([binary], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))


if __name__ == '__main__':
    main()

"""Offline Linux socketpair integration; use TMPDIR on a private scratch filesystem."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

CORE = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie'] if args.sanitize else []
    sources = ['tests/test_cancel_ipc.c', 'io/player.c', 'media.c', 'session.c',
               'control.c', 'tts.c', 'account.c', 'audio_route.c', 'protocol.c', 'mode_gate.c',
               '../doubao/src/text_match.c', '../doubao/src/json.c']
    with tempfile.TemporaryDirectory(prefix='dc-cancel-ipc-') as directory:
        binary = Path(directory) / 'fixture'
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                        *flags, *sources, '-Wl,--gc-sections', '-pthread', '-lm',
                        '-o', str(binary)], cwd=CORE, check=True)
        subprocess.run([binary], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))
        for fixture in ('tests/test_repeated_interrupt.c', 'tests/test_capture_overflow.c'):
            sources[0] = fixture
            subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                            *flags, *sources, '-Wl,--gc-sections', '-pthread', '-lm',
                            '-o', str(binary)], cwd=CORE, check=True)
            subprocess.run([binary], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))


if __name__ == '__main__':
    main()

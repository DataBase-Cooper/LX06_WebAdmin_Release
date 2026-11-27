"""Offline production recognition/cue regression; synthetic ASR input only."""
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
    sources = ['tests/test_phone_recognition.c', 'io/player.c', 'media.c', 'session.c',
               'control.c', 'tts.c', 'account.c', 'bootstrap.c', 'audio_route.c',
               'protocol.c', '../doubao/src/text_match.c', '../doubao/src/json.c']
    with tempfile.TemporaryDirectory(prefix='dc-phone-recognition-') as directory:
        binary = Path(directory) / 'fixture'
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                        *flags, *sources, '-Wl,--gc-sections', '-pthread', '-lm', '-o', str(binary)],
                       cwd=CORE, check=True)
        for scenario in range(8):
            subprocess.run([binary, str(scenario)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))

if __name__ == '__main__':
    main()

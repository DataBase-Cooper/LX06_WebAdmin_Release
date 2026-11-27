"""Build isolated segmented experiment on Linux; set TMPDIR to scratch."""
import os
from pathlib import Path
import subprocess
import tempfile

core = Path(__file__).resolve().parent.parent
sources = sorted(str(p.relative_to(core)) for p in (core / 'vendor/libfvad/src').rglob('*.c'))
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie',
         '-Ivendor/libfvad/include', '-Ivendor/libfvad/src']
with tempfile.TemporaryDirectory(prefix='segexp-') as tmp:
    for name in ('test', 'matrix'):
        target = Path(tmp) / name
        cmd = ['gcc', *flags, f'tests/segmented_experiment_{name}.c',
               'tests/segmented_experiment_candidate.c', 'voice_guard.c',
               *sources, '-lm', '-pthread', '-o', str(target)]
        subprocess.run(cmd, cwd=core, check=True)
        subprocess.run([str(target)], check=True,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))

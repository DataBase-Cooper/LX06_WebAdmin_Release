"""Build current host fixture and run synthetic greeting test in isolated tmpfs.

Run as root in a private mount/network namespace with tmpfs source
'lx06-greeting-test' mounted on /tmp and only loopback available.
"""
import importlib.util
import os
from pathlib import Path
import socket
import subprocess

HERE = Path(__file__).resolve().parent
CORE = HERE.parent
if os.geteuid() != 0 or [name for _, name in socket.if_nameindex()] != ['lo']:
    raise RuntimeError('isolated network namespace required')
if not any(' /tmp ' in line and ' - tmpfs lx06-greeting-test ' in line
           for line in Path('/proc/self/mountinfo').read_text().splitlines()):
    raise RuntimeError('isolated lx06-greeting-test tmpfs required')
spec = importlib.util.spec_from_file_location('build_core', CORE / 'build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)
flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-fPIE', '-pthread', '-DDC_CORE_HOST_FIXTURE',
         '-I' + str(CORE / 'vendor/libfvad/include'), '-I' + str(CORE / 'vendor/libfvad/src')]
subprocess.run(['gcc', *flags, *map(str, build.sources()), '-ldl', '-lopus', '-lm',
                '-o', '/tmp/greeting-core'], check=True)
env = dict(os.environ, DC_GREETING_FIXTURE_CORE='/tmp/greeting-core', TMPDIR='/tmp',
           PYTHONDONTWRITEBYTECODE='1')
subprocess.run(['python3', str(HERE / 'test_greeting_synthetic_backend.py')], env=env, check=True)

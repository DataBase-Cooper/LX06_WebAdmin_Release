"""Offline production main exit regression, synthetic capture/ASR/route boundary."""
import os
from pathlib import Path
import subprocess
import tempfile
import sys

CORE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CORE))
from build import sources

WRAPPED = ['dc_io_capture_open_mode', 'dc_io_capture_close', 'dc_io_capture_fd',
           'dc_io_capture_stalled', 'dc_io_capture_next', 'lx06_control_asr_open_profile',
           'lx06_control_asr_close', 'lx06_control_asr_reply', 'lx06_control_asr_mic_pending',
           'lx06_control_asr_feed_stamped', 'lx06_control_asr_active_serial', 'dc_audio_route_capture']

def main():
    with tempfile.TemporaryDirectory(prefix='dc-exit-') as temp:
        exe = Path(temp) / 'core'
        subprocess.run(['gcc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-DDC_CORE_HOST_FIXTURE', '-pthread', '-I'+str(CORE/'vendor/libfvad/include'),
                        '-I'+str(CORE/'vendor/libfvad/src'), *map(str, sources()),
                        str(CORE/'tests/test_capture_exit_wrap.c'),
                        *['-Wl,--wrap='+name for name in WRAPPED], '-ldl', '-lopus', '-lm', '-o', str(exe)], check=True)
        env = dict(os.environ, DC_HOST_FIXTURE='1', DC_TRANSPORT_ADAPTER='/bin/true', DB_GREETING='')
        env.pop('TEST_GENERIC', None)
        env.pop('TEST_ROUTE_OTHER', None)
        missing = subprocess.run([exe, '--audio', 'doubao'], env=dict(env, TEST_MISSING_MIC='1'), capture_output=True, text=True, timeout=10)
        assert missing.returncode == 1, (missing.returncode, missing.stderr)
        print('PASS production main: injected capture-open ENOENT exits 1 (not a real mic producer)')
        overflow = subprocess.run([exe, '--audio', 'doubao'], env=env, capture_output=True, text=True, timeout=10)
        assert 'event=capture_overflow' in overflow.stderr, (overflow.returncode, overflow.stderr)
        assert overflow.returncode == 75, (overflow.returncode, overflow.stderr)
        generic = subprocess.run([exe, '--audio', 'doubao'], env=dict(env, TEST_GENERIC='1'), capture_output=True, text=True, timeout=10)
        assert generic.returncode == 1, (generic.returncode, generic.stderr)
        assert 'event=capture_overflow' not in generic.stderr
        route_error = subprocess.run([exe, '--audio', 'doubao'], env=dict(env, TEST_ROUTE_OTHER='1'), capture_output=True, text=True, timeout=10)
        assert route_error.returncode == 1, (route_error.returncode, route_error.stderr)
        assert 'event=capture_route_error' in route_error.stderr and 'event=capture_overflow' not in route_error.stderr
        print('PASS production main: injected ENOBUFS exit 75; capture and route EIO exit 1')

if __name__ == '__main__':
    main()

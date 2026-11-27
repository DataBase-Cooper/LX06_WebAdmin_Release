#!/usr/bin/env python3
"""Offline RED: real EXIT cleanup must not wait forever or abandon live children."""
import os
import pathlib
import signal
import subprocess
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
source = (HERE / 'account-voice.sh').read_text().split('# Audio components come from the unified firmware.')[0]
source = source.replace('/tmp/lx06-wake-', '$T/eventdir/lx06-wake-').replace('/tmp/lx06-$PROVIDER-resident.lock', '$T/lock')
shells = os.environ.get('TEST_SHELLS', '/bin/dash:/bin/bash').split(':')
failed = False
with tempfile.TemporaryDirectory(prefix='cleanup-child.', dir=os.environ['TMPDIR']) as td:
    root = pathlib.Path(td)
    (root / 'eventdir').mkdir()
    child = root / 'child'
    child.write_text('#!/usr/bin/env python3\nimport os,signal,time\nif len(os.sys.argv)>1 and os.sys.argv[1]=="--protocol": print("LX06DIALOGUE/1"); raise SystemExit\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\nopen(os.environ["T"]+"/ready","w").write(str(os.getpid()))\nwhile True: time.sleep(1)\n')
    child.chmod(0o700)
    for shell in shells:
        for kind in ('worker', 'resident'):
            for repeated in (False, True):
                for p in (root / 'ready', root / 'session'):
                    if p.is_dir(): p.rmdir()
                    elif p.exists(): p.unlink()
                fixture = root / 'fixture'
                fixture.write_text(source + '''\nmktemp() { mkdir "$T/session"; printf '%s\\n' "$T/session"; }\nvoice_http_start() { :; }\nvoice_wait_init() { :; }\nvoice_wait_event() { "$T/child" & WAIT_PID=$!; wait "$WAIT_PID"; }\nMODEL_ID=fixture\nLX06_DIALOGUED=$T/child\nPROVIDER=fixture\nif [ "$KIND" = worker ]; then voice_worker; else voice_resident; fi\n''')
                env = dict(os.environ, T=td, KIND=kind)
                proc = subprocess.Popen([shell, str(fixture)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                try:
                    deadline = time.monotonic() + 5
                    while not (root / 'ready').exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(.01)
                    if not (root / 'ready').exists():
                        print(f'ERROR {shell} {kind}: child did not start'); failed = True; continue
                    pid = int((root / 'ready').read_text())
                    os.kill(proc.pid, signal.SIGTERM)
                    time.sleep(.3)
                    if repeated and proc.poll() is None:
                        os.kill(proc.pid, signal.SIGTERM)
                    try: status = proc.wait(timeout=1)
                    except subprocess.TimeoutExpired: status = None
                    try: os.kill(pid, 0); alive = True
                    except ProcessLookupError: alive = False
                    ok = status is not None and not alive
                    print(f'{"PASS" if ok else "FAIL"} {shell} {kind} {"repeated" if repeated else "first"} TERM: wrapper={status} child_alive={alive}', flush=True)
                    failed |= not ok
                finally:
                    try: os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    proc.wait()
raise SystemExit(1 if failed else 0)

"""Explicit real-service diagnostic; no speaker playback or device changes."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import wave

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mode', choices=['0', '1'], default='0')
    args = parser.parse_args()
    key = re.search(r"DB_API_KEY='([^']+)'", (ROOT/'build/private-doubao/key-source.sh').read_text())[1]
    env = dict(os.environ, DB_API_KEY=key, DB_MIC_FIFO='/dev/stdin',
               DB_PLAYBACK='0', DB_IDLE_SECONDS='60', DB_MAX_SECONDS='65',
               DB_BARGE_ANY=args.mode)
    proc = subprocess.Popen([str(ROOT/'runtime/doubao/build/doubao-host')],
                            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    start = time.monotonic()
    rows = []
    ready = threading.Event()
    lock = threading.Lock()
    def reader():
        for line in iter(proc.stdout.readline, b''):
            text = line.decode(errors='replace').strip().replace(key, '[redacted]')
            with lock:
                rows.append({'s': round(time.monotonic()-start, 3), 'log': text})
            if 'session ready' in text:
                ready.set()
    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    audio = ROOT/'docs/evidence/20261028-wake-stability'
    clips = []
    for name in ['news-long', 'time', 'interrupt1', 'db-goodbye']:
        with wave.open(str(audio/f'source-{name}.wav'), 'rb') as wav:
            assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (16000, 1, 2)
            clips.append((name, wav.readframes(wav.getnframes())))
    try:
        assert ready.wait(15), 'cloud not ready'
        # Time offsets are from session readiness; include silence between clips.
        plan = [(4, clips[0]), (20, clips[1]), (32, clips[2]), (43, clips[3])]
        pcm = bytearray(32000*51)
        for offset, (name, data) in plan:
            pcm[offset*32000:offset*32000+len(data)] = data
        origin = time.monotonic()
        for offset in range(0, len(pcm), 640):
            proc.stdin.write(pcm[offset:offset+640])
            proc.stdin.flush()
            time.sleep(max(0, origin+(offset+640)/32000-time.monotonic()))
    except BrokenPipeError:
        pass
    finally:
        if proc.poll() is None:
            proc.terminate()
        proc.wait(5)
        thread.join(3)
        try:
            proc.stdin.close()
        except BrokenPipeError:
            pass
    lines=[row['log'] for row in rows]
    checks={'one_primary_session':lines.count('doubao: session ready')==1,
            'goodbye':any('goodbye; closing session' in line for line in lines),
            'clean_exit':proc.returncode==0}
    if args.mode=='0':
        checks.update(ordinary_speech_isolated=any('control ASR 现在几点' in line for line in lines),
                      ordinary_speech_not_in_primary=not any('doubao: ASR 现在几点' in line for line in lines),
                      control_canceled=any('interrupt phrase; connection retained (keyword observer)' in line for line in lines),
                      no_native_barge=not any('barge-in speech' in line for line in lines),
                      main_received_question=any('doubao: ASR 请给我播报' in line for line in lines))
    else:
        checks['native_barge']=any('barge-in speech' in line for line in lines)
        checks['no_observer']=not any('keyword observer ready' in line for line in lines)
    report = dict(mode=args.mode, rc=proc.returncode, checks=checks, passed=all(checks.values()), rows=rows)
    dest = ROOT/'build/doubao-barge-fix'/f'cloud-mode-{args.mode}-{int(time.time())}.json'
    dest.parent.mkdir(exist_ok=True)
    dest.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k!='rows'}, ensure_ascii=False), flush=True)
    if not report['passed']:raise SystemExit(1)


if __name__ == '__main__':
    main()

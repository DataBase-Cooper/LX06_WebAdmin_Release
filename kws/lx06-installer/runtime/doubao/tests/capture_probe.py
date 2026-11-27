"""Replay a diagnostic microphone excerpt to the real service, without speakers."""
import array
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time
import wave

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'docs/evidence/20261028-wake-stability/rc7-doubao'
path=sorted(OUT.glob('interrupt-mic-*.wav'))[-1]
parser=argparse.ArgumentParser()
parser.add_argument('--gain-db',type=float,default=0)
parser.add_argument('--source',type=Path)
parser.add_argument('--start-seconds',type=float,default=9)
parser.add_argument('--seconds',type=float,default=6)
args=parser.parse_args()
if args.source:path=args.source
key=re.search(r"DB_API_KEY='([^']+)'",(ROOT/'build/private-doubao/key-source.sh').read_text())[1]
with wave.open(str(path),'rb') as w:
    w.setpos(int(args.start_seconds*16000));pcm=w.readframes(int(args.seconds*16000))
samples=array.array('h');samples.frombytes(pcm)
gain=10**(args.gain_db/20)
pcm=array.array('h',(max(-32768,min(32767,round(s*gain))) for s in samples)).tobytes()
env=dict(os.environ,DB_API_KEY=key,DB_MIC_FIFO='/dev/stdin',DB_PLAYBACK='0',DB_IDLE_SECONDS='10',DB_MAX_SECONDS='25')
p=subprocess.Popen([str(ROOT/'runtime/doubao/build/doubao-host')],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env)
lines=[];ready=threading.Event()
def read():
    for line in iter(p.stdout.readline,b''):
        s=line.decode(errors='replace').replace(key,'[redacted]');lines.append(s)
        if 'session ready' in s:ready.set()
t=threading.Thread(target=read,daemon=True);t.start();assert ready.wait(15)
data=bytes(64000)+pcm+bytes(16000*2*12)
try:
    start=time.monotonic()
    for offset in range(0,len(data),640):
        p.stdin.write(data[offset:offset+640]);p.stdin.flush()
        time.sleep(max(0,start+(offset+640)/32000-time.monotonic()))
except BrokenPipeError:pass
finally:
    if p.poll() is None:p.terminate()
    p.wait(5);t.join(3)
    try:p.stdin.close()
    except BrokenPipeError:pass
report={'source':path.name,'gain_db':args.gain_db,'rc':p.returncode,'log':''.join(lines)}
(OUT/('capture-cloud-probe-'+path.stem+'-'+str(args.gain_db)+'.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2))
print(json.dumps(report,ensure_ascii=False))

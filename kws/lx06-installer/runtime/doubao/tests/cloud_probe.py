"""Real service smoke test. Credentials are read from the personal script, never printed."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[3]
key_script=ROOT/'build/private-doubao/key-source.sh'
key=re.search(r"DB_API_KEY='([^']+)'",key_script.read_text())[1]
env=dict(os.environ,DB_API_KEY=key,DB_MIC_FIFO='/dev/zero',DB_PACE_INPUT='1',DB_PLAYBACK='0',DB_IDLE_SECONDS='3',DB_MAX_SECONDS='18')
start=time.monotonic()
proc=subprocess.run([str(ROOT/'runtime/doubao/build/doubao-host')],env=env,capture_output=True,timeout=30)
output=proc.stdout.decode(errors='replace')+proc.stderr.decode(errors='replace')
output=output.replace(key,'[redacted]')
report={'rc':proc.returncode,'elapsed_s':time.monotonic()-start,'ready':'session ready' in output,'audio':'audio done bytes=' in output,'log':output}
dest=ROOT/'docs/evidence/20261028-wake-stability/rc7-doubao'
dest.mkdir(exist_ok=True)
(dest/'cloud-probe.json').write_text(json.dumps(report,ensure_ascii=False,indent=2))
print(json.dumps(report,ensure_ascii=False))
sys.exit(0 if report['ready'] and report['audio'] else 1)

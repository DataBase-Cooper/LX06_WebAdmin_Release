#!/usr/bin/env python3
"""Offline diagnostics; requires gcc, pkg-config and libspeexdsp-dev on Linux."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile

CORE = Path(__file__).resolve().parent.parent
AEC = CORE / '../../../scratch/xz-client-rewrite/src'
p = argparse.ArgumentParser()
p.add_argument('--seed', type=int, default=431)
p.add_argument('--quick', action='store_true')
p.add_argument('--stress', action='store_true')
a = p.parse_args()
if not 0 < a.seed < 2**32 - 200000:
    p.error('seed must be in 1..2**32-200001')
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'speexdsp'], text=True).split()
version = subprocess.check_output(['pkg-config', '--modversion', 'speexdsp'], text=True).strip()
sources = [CORE/'tests/doubletalk_matrix.c', CORE/'voice_guard.c', AEC/'aec.c']
sources += sorted((CORE/'vendor/libfvad/src').rglob('*.c'))
hashes = {str(f.relative_to(CORE) if f.is_relative_to(CORE) else 'legacy-aec.c'): hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}
with tempfile.TemporaryDirectory(prefix='dc-matrix-') as tmp:
    exe = Path(tmp)/'matrix'
    cmd = ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(CORE/'vendor/libfvad/include'), '-I'+str(CORE/'vendor/libfvad/src'), '-I'+str(AEC), *map(str, sources), *flags, '-lm', '-pthread', '-o', str(exe)]
    subprocess.run(cmd, check=True)
    raw = subprocess.check_output([str(exe), str(a.seed), str(int(a.quick)), str(int(a.stress))], text=True)
rows = []
for line in raw.splitlines():
    ff,nf,delay,gain,kind,peak,guard,speex,legacy,voiced,echo,sr,lr,sp,lp,scenario = line.split(',')
    rows.append({'scenario': ['oracle','late80','early80','drift160','path_gain_shift'][int(scenario)], 'far_family': int(ff), 'near_family': int(nf), 'delay_samples': int(delay), 'echo_gain': float(gain), 'class': ['far','mixed','near'][int(kind)], 'peak': int(peak), 'decisions': {'guard': int(guard), 'speex_vad': int(speex), 'legacy_vad': int(legacy)}, 'guard_voiced_frames': int(voiced), 'guard_echo_frames': int(echo), 'speex_residual_ratio': float(sr), 'legacy_residual_ratio': float(lr), 'speex_near_projection': float(sp), 'legacy_near_projection': float(lp)})
summary = {}
for name in ('guard','speex_vad','legacy_vad'):
    summary[name] = {}
    for kind in ('far','mixed','near'):
        cases = [r for r in rows if r['class']==kind and r['scenario']=='oracle']
        n = sum(r['decisions'][name] for r in cases)
        summary[name][kind] = {'triggered': n, 'total': len(cases), 'fraction': n/len(cases)}
for metric in ('speex_residual_ratio', 'legacy_residual_ratio', 'speex_near_projection', 'legacy_near_projection'):
    for kind in ('far','mixed','near'):
        vals=sorted(r[metric] for r in rows if r['class']==kind and r['scenario']=='oracle')
        summary.setdefault('distributions', {}).setdefault(metric, {})[kind] = {'min': vals[0], 'median': statistics.median(vals), 'max': vals[-1]}
# Digest is a reproducibility fingerprint of the generated run, not a claim about raw PCM.
scenario_summary = {}
for scenario in sorted({r['scenario'] for r in rows}):
    selected = [r for r in rows if r['scenario'] == scenario]
    scenario_summary[scenario] = {}
    for name in ('guard','speex_vad','legacy_vad'):
        scenario_summary[scenario][name] = {}
        for kind in ('far','mixed','near'):
            cases = [r for r in selected if r['class'] == kind]
            triggered = sum(r['decisions'][name] for r in cases)
            scenario_summary[scenario][name][kind] = {'triggered':triggered, 'total':len(cases)}
        s = scenario_summary[scenario][name]
        s['false_accepts'] = s['far']['triggered']
        s['misses'] = {k:s[k]['total']-s[k]['triggered'] for k in ('mixed','near')}
report={'seed':a.seed, 'quick':a.quick, 'stress':a.stress, 'signal_digest':hashlib.sha256((str(a.seed)+'\n'+raw).encode()).hexdigest(), 'speexdsp_version':version, 'source_sha256':hashes, 'cases':rows, 'summary':summary, 'scenario_summary':scenario_summary, 'limitations':'Synthetic only; modeled AEC reference, synchronous guard writes; near-only starts at evaluation; VAD persistence >=80ms; no thresholds calibrated or real acoustic alignment.'}
print(json.dumps(report, sort_keys=True, indent=2))

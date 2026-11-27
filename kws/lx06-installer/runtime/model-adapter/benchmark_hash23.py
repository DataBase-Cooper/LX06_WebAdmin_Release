"""Synthetic old/new local benchmark. Never sends a platform challenge.

The test-only C harness contains a frozen original and current solver under
the same compiler options. Separate child processes preserve their nice=15.
An ARM binary can be built with tests/build-hash23-bench.sh and run separately
in an approved idle device window; this command executes only local binaries.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent
def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',type=Path)
    parser.add_argument('--iterations',type=int,default=60000)
    parser.add_argument('--prefix',type=int,default=32)
    parser.add_argument('--repeats',type=int,default=5)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if not 1<=args.iterations<=200000 or not 15<=args.prefix<=200 or not 1<=args.repeats<=20:parser.error('invalid bounded workload')
    with tempfile.TemporaryDirectory() as tmp:
        binary=args.binary or Path(tmp)/'benchmark'
        if not args.binary:
            subprocess.run(['gcc','-O2','-std=c11','-Wall','-Wextra','-Werror',str(ROOT/'tests/hash23-bench.c'),'-o',str(binary)],check=True)
        rows=[]
        # Alternate order to avoid favoring either implementation during host
        # frequency/load drift. Real device runs must also retain headroom.
        for repeat in range(args.repeats):
            for variant in (['old','new'] if repeat%2==0 else ['new','old']):
                run=subprocess.run([str(binary.resolve()),variant,str(args.iterations-1),str(args.prefix)],capture_output=True,text=True,check=True,timeout=20)
                row=json.loads(run.stdout)
                if row['answer']!=args.iterations-1 or row['nice']<15:raise RuntimeError('solver result/priority mismatch')
                rows.append(row)
        summary={}
        for variant in ['old','new']:
            selected=[r for r in rows if r['variant']==variant]
            summary[variant]={key:statistics.median(r[key] for r in selected) for key in ['wall_ms','cpu_ms']}
        summary['cpu_speedup']=summary['old']['cpu_ms']/summary['new']['cpu_ms']
        summary['wall_speedup']=summary['old']['wall_ms']/summary['new']['wall_ms']
        result=dict(workload='public synthetic salt, fixed expiry; no network',iterations=args.iterations,prefix_bytes=args.prefix,rows=rows,summary=summary)
        text=json.dumps(result,ensure_ascii=False,indent=2)
        if args.output:args.output.write_text(text,encoding='utf-8')
        print(text)

if __name__=='__main__':main()

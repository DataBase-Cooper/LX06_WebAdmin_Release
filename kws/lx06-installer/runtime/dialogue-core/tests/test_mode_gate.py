"""Internal mode-gate and inherited-FD tests. No device, account or actions."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(args,**kw):
 p=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=30,**kw)
 assert not p.returncode,(p.returncode,p.stdout[-1000:],p.stderr[-3000:])
 return p.stdout
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--output',required=True);args=ap.parse_args();out=Path(args.output).resolve()
 assert out.parent==(ROOT/'build').resolve() and not out.exists();out.mkdir()
 sources=[HERE.parent/'mode_gate.c',HERE.parent/'protocol.c',Path(__file__).with_suffix('.c')]
 evidence=sources+[HERE.parent/'mode_gate.h',HERE.parent/'protocol.h',HERE.parent/'types.h',HERE.parent/'io/compat.h',Path(__file__)]
 report=dict(passed=False,actual_device=False,actual_audio=False,cloud_requests=False,actions_executed=False,sources={p.relative_to(ROOT).as_posix():sha(p) for p in evidence})
 try:
  for label,extra in [('host',[]),('sanitized',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
   binary=out/label;run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',*extra,*sources,'-o',binary]);report[label]=json.loads(run(['unshare','--net',binary],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1')))
  report['source_unchanged']=all(sha(ROOT/p)==v for p,v in report['sources'].items());report['passed']=report['source_unchanged']
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps({k:v for k,v in report.items() if k!='sources'}))
if __name__=='__main__':main()

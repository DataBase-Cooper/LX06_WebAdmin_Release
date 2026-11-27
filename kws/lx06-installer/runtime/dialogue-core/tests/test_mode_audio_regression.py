"""Four actual-main/transport regressions for the changed child FD boundary.

Only synthetic loopback clouds, ASR and ALSA inside a private network namespace.
"""
import argparse,json,os,socket,subprocess,sys
from pathlib import Path
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2];sys.path.insert(0,str(HERE))
from test_audio_reactor import exercise
from test_reactor import Reactor,sha
def main():
 p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--binary',required=True);p.add_argument('--adapter',required=True);a=p.parse_args()
 out=Path(a.output).resolve();binary=Path(a.binary).resolve();adapter=Path(a.adapter).resolve()
 assert os.geteuid()==0 and [n for _,n in socket.if_nameindex()]==['lo'] and os.readlink('/proc/self/ns/net')!=os.readlink('/proc/1/ns/net')
 assert out.parent==(ROOT/'build').resolve() and not out.exists();out.mkdir();library=out/'fake';library.mkdir()
 subprocess.run(['ip','link','set','lo','up'],check=True)
 manifest=json.loads((binary.parent/'manifest.json').read_text());assert manifest['passed'] and manifest['fixture'] and manifest['sanitized'] and sha(binary)==manifest['sha256']
 subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fPIC','-shared','-fsanitize=address,undefined','-fno-sanitize-recover=all',HERE/'fake-asound.c','-o',library/'libasound.so.2'],check=True)
 evidence=[Path(__file__),HERE/'test_audio_reactor.py',HERE/'test_reactor.py',HERE/'fake-asound.c',ROOT/'runtime/common/tests/fake_asr.py']
 report=dict(passed=False,actual_device=False,actual_audio=False,cloud_requests=False,actions_executed=False,binary_sha256=sha(binary),manifest_sha256=sha(binary.parent/'manifest.json'),adapter_sha256=sha(adapter),sources={p.relative_to(ROOT).as_posix():sha(p) for p in evidence},cases=[])
 def save():(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 try:
  for provider in ('xiaozhi','doubao'):
   for mode in ('0','1'):
    result=exercise(binary,library,adapter,provider,mode);report['cases'].append(dict(passed=True,**result));save()
  report['binary_sources_current']=all(sha(ROOT/p)==v for p,v in manifest['sources'].items());report['source_unchanged']=all(sha(ROOT/p)==v for p,v in report['sources'].items());report['passed']=report['binary_sources_current'] and report['source_unchanged'];save()
 except BaseException as e:report['failure']=dict(type=type(e).__name__,message=str(e)[-3000:],fixture=Reactor.last_failure);save();raise
 print(json.dumps(dict(passed=report['passed'],cases=len(report['cases']),report_sha256=sha(out/'report.json'))));assert report['passed']
if __name__=='__main__':main()

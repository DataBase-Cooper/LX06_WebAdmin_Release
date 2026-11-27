"""Actual main phone/action integration against local synthetic endpoints.

No phone, account, PCM device or real business action. The phone worker is a
Unix protocol fixture; true metadata deliberately still cannot enable media.
"""
import argparse,json,os,socket,subprocess,sys,time
from pathlib import Path
from unittest.mock import patch
HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[2]
sys.path.insert(0,str(HERE))
import test_reactor as base
from test_reactor import Reactor,wait,sha

WORKER=r'''#!/usr/bin/python3
import fcntl,json,os,socket,struct,time
from pathlib import Path
root=Path(os.environ['DC_FIXTURE_DIRECTORY'])
fds=[]
for fd in range(256):
 try:fcntl.fcntl(fd,fcntl.F_GETFD);fds.append(fd)
 except OSError:pass
wire=struct.Struct('<IHHIIQQQIHH')
s=socket.socket(fileno=3);s.settimeout(12);raw=s.recv(9000);h=wire.unpack(raw[:48]);request=json.loads(raw[48:])
assert h[:3]==(0x31474344,1,1) and h[3]==len(raw)-48 and h[4]==0 and h[5]==int(request['epoch'])
assert request['action']=='call.start' and request['target']=='doubao' and request['params']=={}
assert request['wake_source']=='official_asr' and request['wake_text']=='给豆包打电话' and request['script_id']=='phone_doubao.sh'
report=dict(fds=fds,request=request,opened_at_ns=time.monotonic_ns());(root/'phone-worker.json').write_text(json.dumps(report))
def emit(kind,value=None):
 data=json.dumps(value,separators=(',',':')).encode() if value is not None else b''
 s.send(wire.pack(0x31474344,1,kind,len(data),0,h[5],0,0,0,0,0)+data)
def ack(status):emit(35,dict(v=2,kind='command_ack',epoch=request['epoch'],command_id=request['command_id'],status=status,code='accepted',replayed=False))
ack('started');time.sleep(.25)
pattern=os.environ['DC_MODE_PATTERN'];report['response_at_ns']=time.monotonic_ns();(root/'phone-worker.json').write_text(json.dumps(report))
if pattern=='false-capability':emit(35,dict(v=1,kind='phone_status',production_capable=False))
elif pattern=='early-ready':emit(32)
elif pattern=='completed-only':ack('completed');emit(32)
elif pattern=='production-ready':
 ack('completed');emit(35,dict(v=1,kind='phone_status',production_capable=True));emit(32)
elif pattern=='failed-ack':ack('failed')
elif pattern=='untrusted-text':emit(35,dict(v=1,kind='assistant',text='not a reply'))
elif pattern not in ('waiting-control','timeout'):raise AssertionError('fixture pattern')
while True:
 raw=s.recv(9000)
 if not raw:break
 message=wire.unpack(raw[:48]);assert message[2]==5,'No MIC/RESUME/GRANT before genuine media support'
 break
'''

class ModeReactor(Reactor):
 def __init__(self,binary,library,mode,pattern):
  super().__init__(binary,library,duration=2);self.mode=mode;self.mode_pattern=pattern
 def __enter__(self):
  original=subprocess.Popen
  def launch(args,**kw):
   receiver,sender=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
   sentinel=os.open('/dev/null',os.O_RDONLY)
   try:
    request=dict(v=1,script_id='phone_doubao.sh' if self.mode=='phone' else 'door_open.sh',wake_source='official_asr',wake_text='给豆包打电话' if self.mode=='phone' else '打开玻璃门',action='call.start' if self.mode=='phone' else 'door.open')
    request.update(target='doubao') if self.mode=='phone' else request.update(status=self.mode_pattern)
    sender.send(json.dumps(request,ensure_ascii=False).encode())
    worker=self.root/'phone-worker';worker.write_text(WORKER);worker.chmod(0o700)
    kw['env'].update(DC_PHONE_WORKER=str(worker),DC_MODE_PATTERN=self.mode_pattern)
    kw['pass_fds']=[*kw.get('pass_fds',[]),receiver.fileno(),sentinel]
    return original([args[0],'--phone-fd' if self.mode=='phone' else '--action-result-fd',str(receiver.fileno())],**kw)
   finally:receiver.close();sender.close();os.close(sentinel)
  with patch.object(base.subprocess,'Popen',launch):return super().__enter__()
 def finish_mode(self,expected):
  wait(lambda:self.process.poll() is not None,seconds=12,label='bounded internal mode close')
  assert self.process.returncode==expected,(self.process.returncode,self.events,self.errors)
  self.log_thread.join(1);wait(lambda:any(l['code']==0 for l in self.light),label='mode closed light')
  states=[e['value'] for e in self.events if e['event']=='state']
  assert states.count(6)==states.count(0)==1,states
  assert not self.errors and not self.broker.errors,(self.errors,self.broker.errors)
  assert not self.requests(),'Internal modes must not invoke model account scripts'
  if self.mode=='phone':
   assert not any(s in (2,3,4) for s in states),states
   assert not self.broker.requests,'Phone metadata must never reach TTS'
   assert json.loads((self.root/'phone-worker.json').read_text())['fds']==[0,1,2,3]
  # The synthetic ASR may answer a queued PING after the core has deliberately
  # closed its private socket. Keep all other fixture errors fatal; report this
  # exact reset only after explicit CLOSED and the expected phone exit reason.
  if self.mode=='phone':
   reason='phone_connect_timeout' if self.mode_pattern=='timeout' else 'phone_media_unavailable' if self.mode_pattern=='production-ready' else 'phone_refused'
   if self.mode_pattern!='waiting-control':assert any(e['event']==reason for e in self.events)
  assert all(e=='unexpected socket failure' for e in self.asr.errors),self.asr.errors
  reset_count=len(self.asr.errors);self.asr.errors.clear()
  result=self.snapshot();result['expected_asr_peer_close_resets']=reset_count
  return result

def phone_case(binary,library,pattern):
 with ModeReactor(binary,library,'phone',pattern) as r:
  r.event('cue_pcm');assert any(e['op']=='WRITE' for e in r.ledger())
  if pattern=='waiting-control':
   before=time.monotonic_ns();r.asr.emit('等一下',final=False)
   drop=wait(lambda:next((e for e in r.ledger() if e['op']=='DROP'),None),label='cue local control drop')
   delay=(drop['at_ns']-before)/1e6;assert 0<=delay<100,delay
   r.assert_alive();r.asr.emit('请介绍故宫',final=True);time.sleep(.08);r.assert_alive()
   assert len([e for e in r.ledger() if e['op']=='DROP'])==1
   r.asr.emit('结束对话',final=False)
   result=r.finish_mode(0);result['control_to_drop_ms']=delay
  else:
   result=r.finish_mode(1)
   assert any(e['op']=='DROP' for e in r.ledger()),'refusal/timeout must drop cue on sole player'
  return result
def action_case(binary,library,status,interrupt=False):
 with ModeReactor(binary,library,'action',status) as r:
  r.event('first_pcm');assert any(e['value']==4 for e in r.events if e['event']=='state')
  if interrupt:
   start=time.monotonic_ns();r.asr.emit('等一下',final=False)
   drop=wait(lambda:next((e for e in r.ledger() if e['op']=='DROP'),None),label='action result local stop')
   delay=(drop['at_ns']-start)/1e6;assert 0<=delay<100,delay
  result=r.finish_mode(0)
  expected={'success':'操作已完成。','failed':'操作未成功，请检查。','unknown':'尚未确认操作结果，请先检查。'}[status]
  assert [q['text'] for q in r.broker.requests if q['kind']==2]==[expected]
  result['actions_executed']=False
  if interrupt:result['control_to_drop_ms']=delay
  return result
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--output',required=True);ap.add_argument('--binary',required=True);a=ap.parse_args();out=Path(a.output).resolve();binary=Path(a.binary).resolve()
 assert os.geteuid()==0 and [n for _,n in socket.if_nameindex()]==['lo']
 assert out.parent==(ROOT/'build').resolve() and not out.exists();out.mkdir();library=out/'fake';library.mkdir()
 manifest=json.loads((binary.parent/'manifest.json').read_text());assert manifest['passed'] and manifest['fixture'] and not manifest['arm'] and sha(binary)==manifest['sha256']
 flags=['-std=c11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared']
 if manifest['sanitized']:flags+=['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all']
 subprocess.run(['gcc',*flags,HERE/'fake-asound.c','-o',library/'libasound.so.2'],check=True)
 evidence=[Path(__file__),HERE/'test_reactor.py',HERE/'fake-asound.c',ROOT/'runtime/common/tests/fake_asr.py']
 report=dict(passed=False,actual_device=False,actual_audio=False,cloud_requests=False,actions_executed=False,binary_sha256=sha(binary),binary_manifest_sha256=sha(binary.parent/'manifest.json'),sources={p.relative_to(ROOT).as_posix():sha(p) for p in evidence},cases=[])
 def save():(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 try:
  for pattern in ('false-capability','early-ready','completed-only','production-ready','failed-ack','untrusted-text','waiting-control','timeout'):
   report['cases'].append(dict(name='phone-'+pattern,passed=True,**phone_case(binary,library,pattern)));save()
  for status,interrupt in [('success',False),('failed',False),('unknown',False),('success',True)]:
   report['cases'].append(dict(name='action-'+status+('-interrupt' if interrupt else ''),passed=True,**action_case(binary,library,status,interrupt)));save()
  report['binary_sources_current']=all(sha(ROOT/p)==v for p,v in manifest['sources'].items());report['source_unchanged']=all(sha(ROOT/p)==v for p,v in report['sources'].items());report['passed']=report['binary_sources_current'] and report['source_unchanged'];save()
 except BaseException as e:report['failure']=dict(type=type(e).__name__,message=str(e)[-3000:],fixture=Reactor.last_failure);save();raise
 print(json.dumps(dict(passed=report['passed'],cases=len(report['cases']),report_sha256=sha(out/'report.json'))))
 assert report['passed']
if __name__=='__main__':main()

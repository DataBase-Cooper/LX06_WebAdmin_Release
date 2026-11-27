"""Actual core reactor + production phone dispatcher via inherited Unix socket.
No phone, network transport, device, cloud, or actual audio is used.
Run as root inside a private network namespace and a private scratch tmpfs.
"""
import json, os, socket, subprocess, sys, time
from pathlib import Path
from unittest.mock import patch
HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import test_reactor as base
from test_reactor import Reactor,wait

class PhoneReactor(Reactor):
 def __init__(self,binary,library,worker,order):
  super().__init__(binary,library,duration=2);self.worker=worker;self.order=order
 def __enter__(self):
  original=subprocess.Popen
  def launch(args,**kw):
   receiver,sender=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
   try:
    request=dict(v=1,script_id='phone_doubao.sh',wake_source='official_asr',wake_text='synthetic wake',action='call.start',target='doubao')
    sender.send(json.dumps(request).encode())
    kw['env'].update(DC_PHONE_WORKER=str(self.worker),DC_REPLAY_ORDER=self.order)
    kw['pass_fds']=[*kw.get('pass_fds',[]),receiver.fileno()]
    return original([args[0],'--phone-fd',str(receiver.fileno())],**kw)
   finally:receiver.close();sender.close()
  with patch.object(base.subprocess,'Popen',launch):return super().__enter__()

def run(binary,library,worker,order):
 with PhoneReactor(binary,library,worker,order) as r:
  r.event('cue_pcm')
  if order=='historical':
   wait(lambda:len([e for e in r.events if e['event']=='phone_metadata'])>=3,seconds=5,label='historical replay metadata')
   time.sleep(.2);r.assert_alive()
   assert not any(e['event'] in ('phone_refused','phone_media_unavailable') for e in r.events),r.events
   r.asr.emit('结束对话',final=False);expected=0
  else:
   wait(lambda:r.process.poll() is not None,seconds=5,label='genuine completion unavailable')
   expected=1
  wait(lambda:r.process.poll() is not None,seconds=8,label='core close')
  assert r.process.returncode==expected,(r.process.returncode,r.events)
  r.log_thread.join(1)
  events=[e['event'] for e in r.events]
  assert 'phone_refused' not in events,events
  assert ('phone_media_unavailable' in events)==(order!='historical'),events
  assert len([e for e in events if e=='phone_metadata'])>=(2 if order=='genuine_old' else 3),events
  assert not r.requests() and not r.broker.requests
  states=[e['value'] for e in r.events if e['event']=='state']
  assert not any(s in (2,3,4) for s in states),states
  assert not r.errors and not r.broker.errors,(r.errors,r.broker.errors)
  assert all(e=='unexpected socket failure' for e in r.asr.errors),r.asr.errors
  r.asr.errors.clear()
  print(order,'PASS',flush=True)

if __name__=='__main__':
 assert os.geteuid()==0 and [n for _,n in socket.if_nameindex()]==['lo']
 assert len(sys.argv)==4
 for order in ('genuine','historical','genuine_old'):run(Path(sys.argv[1]),Path(sys.argv[2]),Path(sys.argv[3]),order)

"""Network-isolated loopback fixtures: real WS, codecs, fd3 ABI and stock ARM.
Only synthetic credentials/PCM are used. No device or account is contacted.
"""
import argparse,base64,ctypes,hashlib,json,math,os,queue,shutil,socket,struct,subprocess,tempfile,threading,time
from pathlib import Path

HERE=Path(__file__).resolve().parents[1];ROOT=HERE.parents[2]
HEADER=struct.Struct('<IHHIIQQQIHH');MAGIC=0x31474344
OPEN,MIC,CANCEL,RESUME,CLOSE,GRANT=range(1,7)
GREET=8
READY,PARTIAL,FINAL,TEXT,BEGIN,AUDIO,END,ACK,ERROR,RESUMED=range(32,42)
EPOCH=41
def run(args,**kwargs):
 out=subprocess.run(list(map(str,args)),capture_output=True,**kwargs)
 assert out.returncode==0,(args,out.stdout[-1200:],out.stderr[-1200:])
 return out
def exact(sock,n):
 out=b''
 while len(out)<n:
  got=sock.recv(n-len(out));assert got,'unexpected fixture EOF';out+=got
 return out
class WebSocket:
 def __init__(self,sock):self.sock=sock;sock.settimeout(4)
 def receive(self):
  first,second=exact(self.sock,2);n=second&127
  if n==126:n=struct.unpack('>H',exact(self.sock,2))[0]
  if n==127:n=struct.unpack('>Q',exact(self.sock,8))[0]
  assert first&128 and second&128 and n<=524288
  mask=exact(self.sock,4);raw=exact(self.sock,n)
  return first&15,bytes(v^mask[i%4] for i,v in enumerate(raw))
 def object(self):
  kind,data=self.receive();assert kind==1;return json.loads(data)
 def send(self,raw,kind=1):
  if isinstance(raw,dict):raw=json.dumps(raw,ensure_ascii=False).encode()
  head=bytes((128|kind,len(raw))) if len(raw)<126 else bytes((128|kind,126))+struct.pack('>H',len(raw)) if len(raw)<=65535 else bytes((128|kind,127))+struct.pack('>Q',len(raw))
  self.sock.sendall(head+raw)
 def quiet(self):
  self.sock.settimeout(.06)
  try:self.sock.recv(1);raise AssertionError('adapter sent an unrequested cloud command')
  except socket.timeout:pass
  finally:self.sock.settimeout(4)

class Fixture:
 def __init__(self,prefix,provider,mode='guarded',ogg=False,settings=None):
  self.prefix=prefix;self.provider=provider;self.mode=mode;self.ogg=ogg;self.settings=settings or {}
 def __enter__(self):
  self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(1)
  self.ready=queue.Queue();self.failure=[]
  def server():
   try:
    conn,_=self.listener.accept();request=b'';conn.settimeout(4)
    while not request.endswith(b'\r\n\r\n'):request+=exact(conn,1)
    lines=request.decode().split('\r\n');headers={k.lower():v.strip() for k,v in (line.split(':',1) for line in lines[1:] if ':' in line)}
    if self.provider=='doubao':assert headers.get('x-api-key')=='fixture-key' and 'authorization' not in headers,headers
    else:assert headers.get('authorization')=='Bearer fixture-key' and headers.get('device-id')=='fixture-device' and headers.get('client-id')=='fixture-client' and 'x-api-key' not in headers,headers
    accept=base64.b64encode(hashlib.sha1((headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
    conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
    self.ready.put(WebSocket(conn))
   except BaseException as error:self.failure.append((self.provider,repr(error)));self.ready.put(None)
  self.thread=threading.Thread(target=server,daemon=True);self.thread.start()
  self.core,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET);self.core.settimeout(4)
  env={k:v for k,v in os.environ.items() if not k.startswith(('XZ_','DB_','DC_','LX06_'))}
  env.update(DC_ENDPOINT='ws://127.0.0.1:'+str(self.listener.getsockname()[1])+'/fixture',DC_CREDENTIAL='fixture-key',DC_DEVICE_ID='fixture-device',DC_CLIENT_ID='fixture-client',DC_UPLINK_MODE=self.mode,DB_AUDIO_FORMAT='ogg_opus' if self.ogg else 'pcm_s16le')
  env.update(self.settings)
  self.process=subprocess.Popen(['/bin/sh','-c','exec 3<&"$1"; shift; exec "$@"','fixture',str(child.fileno()),*self.prefix,'--provider',self.provider],pass_fds=(child.fileno(),),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  child.close();self.send(OPEN)
  self.cloud=self.ready.get(timeout=6)
  if self.cloud is None:
   stdout,stderr=self.process.communicate(timeout=10)
   self.core.close();self.listener.close();self.thread.join(timeout=1)
   raise AssertionError((self.prefix,self.failure,self.process.returncode,stdout,stderr))
  first=self.cloud.object()
  if self.provider=='xiaozhi':
   assert first['type']=='hello' and first['audio_params']=={'format':'opus','sample_rate':16000,'channels':1,'frame_duration':60}
   assert bool(first.get('features',{}).get('aec'))==(self.mode=='native')
   self.cloud.send(dict(type='hello',version=1,transport='websocket',session_id='fixture-session',audio_params=dict(format='opus',sample_rate=24000,channels=1,frame_duration=60)))
  else:
   assert first['type']=='session.create' and first['session']['model']=='1.2.6.1'
   assert first['session']['audio']['input']['format']['rate']==16000
   self.cloud.send(dict(type='session.created'))
  self.expect(READY);self.send(RESUME)
  if self.provider=='xiaozhi':
   listen=self.cloud.object();assert listen['type']=='listen' and listen['state']=='start' and listen['mode']==('realtime' if self.mode=='native' else 'manual')
  return self
 def send(self,kind,data=b'',generation=0,sequence=0,flags=0,epoch=EPOCH):
  self.core.send(HEADER.pack(MAGIC,1,kind,len(data),flags,epoch,generation,sequence,16000 if kind==MIC else 0,1 if kind==MIC else 0,0)+data)
 def receive(self):
  raw=self.core.recv(8240);assert raw,'adapter exited before expected event'
  h=HEADER.unpack(raw[:48]);assert h[0:2]==(MAGIC,1) and h[5]==EPOCH and h[4]&~7==0 and h[3]==len(raw)-48
  return dict(type=h[2],flags=h[4],generation=h[6],sequence=h[7],rate=h[8],channels=h[9],data=raw[48:])
 def expect(self,kind):
  event=self.receive();assert event['type']==kind,(kind,event);return event
 def quiet(self):
  self.core.settimeout(.06)
  try:
   raw=self.core.recv(8240)
   raise AssertionError(('unexpected core output',self.prefix,self.provider,self.ogg,HEADER.unpack(raw[:48]) if len(raw)>=48 else len(raw),self.process.poll()))
  except socket.timeout:pass
  finally:self.core.settimeout(4)
 def start_reply(self,index):
  self.cloud.send(dict(type='tts',state='start') if self.provider=='xiaozhi' else dict(type='response.output_audio.started',response_id='r'+str(index)))
 def assistant(self,text,index):
  self.cloud.send(dict(type='tts',state='sentence_start',text=text) if self.provider=='xiaozhi' else dict(type='response.output_text.delta',delta=text,response_id='r'+str(index)))
 def audio(self,data,index):
  self.cloud.send(data,2) if self.provider=='xiaozhi' else self.cloud.send(dict(type='response.output_audio.delta',delta=base64.b64encode(data).decode(),response_id='r'+str(index)))
 def done(self,index):
  self.cloud.send(dict(type='tts',state='stop') if self.provider=='xiaozhi' else dict(type='response.output_audio.done',response_id='r'+str(index)))
 def __exit__(self,kind,value,traceback):
  try:
   if self.process.poll() is None:
    if kind is None:
     self.send(CLOSE);self.process.wait(timeout=5);assert self.process.returncode==0,self.process.stderr.read()
    else:self.process.kill();self.process.wait(timeout=5)
   self.core.close();self.cloud.sock.close();self.listener.close();self.thread.join(timeout=1)
   assert not self.failure,self.failure
   stdout,stderr=self.process.communicate(timeout=2)
   assert not stdout and not stderr,(stdout,stderr)
  finally:
   if self.process.poll() is None:self.process.kill();self.process.wait(timeout=2)

opus=ctypes.CDLL('libopus.so.0')
opus.opus_encoder_create.restype=ctypes.c_void_p;opus.opus_encoder_create.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.POINTER(ctypes.c_int)]
opus.opus_encode.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_int32]
opus.opus_encoder_destroy.argtypes=[ctypes.c_void_p]
opus.opus_decoder_create.restype=ctypes.c_void_p;opus.opus_decoder_create.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.POINTER(ctypes.c_int)]
opus.opus_decode.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int32,ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
opus.opus_decoder_destroy.argtypes=[ctypes.c_void_p]
def encoded():
 error=ctypes.c_int();encoder=opus.opus_encoder_create(24000,1,2048,ctypes.byref(error));assert encoder and not error.value
 pcm=(ctypes.c_int16*1440)(*(int(5000*math.sin(i*.1)) for i in range(1440)));wire=(ctypes.c_ubyte*4096)()
 try:n=opus.opus_encode(encoder,pcm,1440,wire,4096);assert n>0;return bytes(wire[:n])
 finally:opus.opus_encoder_destroy(encoder)
def valid_uplink(raw):
 error=ctypes.c_int();decoder=opus.opus_decoder_create(16000,1,ctypes.byref(error));assert decoder and not error.value
 pcm=(ctypes.c_int16*960)()
 try:assert opus.opus_decode(decoder,raw,len(raw),pcm,960,0)==960
 finally:opus.opus_decoder_destroy(decoder)

def uplink(fixture):
 data=b''.join(struct.pack('<h',i%2000-1000) for i in range(960))
 for at in range(0,len(data),320):fixture.send(MIC,data[at:at+320],flags=2)
 if fixture.provider=='xiaozhi':
  kind,wire=fixture.cloud.receive();assert kind==2;valid_uplink(wire)
 else:assert b''.join(base64.b64decode(fixture.cloud.object()['audio']) for _ in range(3))==data
def full(prefix,provider,mode,ogg,oggdata):
 with Fixture(prefix,provider,mode,ogg) as f:
  uplink(f)
  for text in ('等一下，等一下。','结束对话，拜拜了哦。'):
   f.cloud.send(dict(type='stt',text=text) if provider=='xiaozhi' else dict(type='conversation.item.input_audio_transcription.completed',transcript=text))
   assert f.expect(FINAL)['data'].decode()==text
   f.cloud.quiet() # Words remain ordinary provider transcripts for the core.
  raw=encoded() if provider=='xiaozhi' else oggdata if ogg else struct.pack('<h',1234)*1440
  f.start_reply(1);assert f.expect(BEGIN)['sequence']==1
  f.assistant('合成音频测试。',1);f.audio(raw,1);f.quiet()
  f.send(GRANT,generation=101,sequence=1)
  text=f.expect(TEXT);audio=f.expect(AUDIO)
  assert text['generation']==audio['generation']==101 and audio['sequence']==1 and audio['rate']==24000 and audio['channels']==1 and audio['flags']==2
  total=len(audio['data'])
  while total<(5760 if ogg else 2880):total+=len(f.expect(AUDIO)['data'])
  f.send(CANCEL,generation=101,sequence=1)
  first=f.cloud.object();assert first['type']==('abort' if provider=='xiaozhi' else 'response.cancel')
  if provider=='xiaozhi':assert f.cloud.object()['type']=='listen'
  else:
   f.audio(raw,1);f.cloud.send(dict(type='response.canceled',response_id='r1'));f.expect(ACK)
   f.cloud.send(dict(type='response.done',response_id='r1'))
  resumed=f.expect(RESUMED);assert resumed['sequence']==1 and resumed['generation']==101
  f.send(CANCEL,generation=101,sequence=1);f.cloud.quiet()
  uplink(f)
  f.start_reply(2);begin=f.expect(BEGIN);assert begin['sequence']==2
  if provider=='doubao':f.cloud.send(dict(type='response.canceled',response_id='r1'));f.cloud.send(dict(type='response.done',response_id='r1'))
  else:f.cloud.send(dict(type='abort',reason='wake_word_detected'))
  f.audio(raw,2);f.done(2);f.quiet();f.send(GRANT,generation=102,sequence=2)
  count=0
  while True:
   event=f.receive();assert event['generation']==102 and event['sequence']==2,event
   if event['type']==END:break
   assert event['type']==AUDIO;count+=len(event['data'])
  assert count==(5760 if ogg else 2880),count
  f.send(RESUME,'请用两句话介绍北京故宫。'.encode(),flags=1);commit=f.cloud.object();assert commit['type']==('listen' if provider=='xiaozhi' else 'input_audio_buffer.commit')
  if provider=='xiaozhi':assert commit['state']=='stop'
  f.cloud.quiet() # Optional QUESTION text never submits a second request.
 return dict(provider=provider,mode=mode,ogg=ogg,passed=True)
def hold_bound(prefix,provider):
 with Fixture(prefix,provider) as f:
  f.start_reply(1);f.expect(BEGIN)
  raw=encoded() if provider=='xiaozhi' else struct.pack('<h',100)*1440
  for _ in range(14):
   try:f.audio(raw,1)
   except (BrokenPipeError,ConnectionResetError):break
  f.process.wait(timeout=5);assert f.process.returncode!=0
  assert f.core.recv(8240)==b''
 return dict(provider=provider,pregrant_bytes_cap=32768,passed=True)
def wrong_epoch(prefix,provider):
 with Fixture(prefix,provider) as f:
  f.send(RESUME,epoch=EPOCH+1);f.process.wait(timeout=5);assert f.process.returncode==2
 return dict(provider=provider,stale_epoch_rejected=True,passed=True)
def commit_tail(prefix,provider):
 with Fixture(prefix,provider) as f:
  tail=struct.pack('<h',4321)*80
  f.send(MIC,tail,flags=2);f.cloud.quiet()
  f.send(RESUME,'完整的问题。'.encode(),flags=1)
  if provider=='xiaozhi':
   kind,raw=f.cloud.receive();assert kind==2;valid_uplink(raw)
  else:
   data=base64.b64decode(f.cloud.object()['audio']);assert data==tail+bytes(640-len(tail))
  commit=f.cloud.object();assert commit['type']==('listen' if provider=='xiaozhi' else 'input_audio_buffer.commit')
  f.cloud.quiet()
  # The next full frame contains only this turn, never the previous tail.
  uplink(f)
 return dict(provider=provider,final_subframe_flushed_once=True,passed=True)
def repeated_cancel(prefix):
 with Fixture(prefix,'doubao') as f:
  raw=struct.pack('<h',777)*1440
  for index in (1,2):
   f.start_reply(index);assert f.expect(BEGIN)['sequence']==index
   f.send(GRANT,generation=100+index,sequence=index)
   f.audio(raw,index);f.expect(AUDIO)
   f.send(CANCEL,generation=100+index,sequence=index)
   assert f.cloud.object()['type']=='response.cancel'
   f.cloud.send(dict(type='response.canceled',response_id='r'+str(index)));f.expect(ACK)
   f.quiet() # ACK alone must not release protected upstream audio.
   f.cloud.send(dict(type='response.done',response_id='r'+str(index)))
   assert f.expect(RESUMED)['sequence']==index
  f.start_reply(3);assert f.expect(BEGIN)['sequence']==3
  f.send(GRANT,generation=103,sequence=3)
  for old in (1,2):
   f.start_reply(old);f.audio(raw,old);f.done(old)
   f.cloud.send(dict(type='response.canceled',response_id='r'+str(old)))
   f.cloud.send(dict(type='response.done',response_id='r'+str(old)))
  f.quiet()
  f.audio(raw,3);audio=f.expect(AUDIO)
  assert audio['generation']==103 and audio['sequence']==3
  f.done(3);assert f.expect(END)['sequence']==3
 return dict(provider='doubao',two_cancellations_and_late_old_data=True,ack_does_not_resume=True,passed=True)
def connection_probe(prefix,reply):
 listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);failure=[];observed=[]
 def serve():
  try:
   connection,_=listener.accept();connection.settimeout(4);request=b''
   while not request.endswith(b'\r\n\r\n'):request+=exact(connection,1)
   headers={k.lower():v.strip() for k,v in (line.split(':',1) for line in request.decode().split('\r\n')[1:] if ':' in line)}
   assert headers.get('x-api-key')=='fixture-key'
   accept=base64.b64encode(hashlib.sha1((headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
   connection.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
   cloud=WebSocket(connection);first=cloud.object();assert first['type']=='session.create';observed.append(first['type'])
   cloud.send(dict(type=reply))
   # Verification never submits MIC, commit, text or response requests.
   closing=cloud.object();assert closing=={'type':'session.close'},closing;observed.append(closing['type'])
   connection.close()
  except BaseException as error:failure.append(repr(error))
 thread=threading.Thread(target=serve,daemon=True);thread.start()
 env={k:v for k,v in os.environ.items() if not k.startswith(('XZ_','DB_','DC_','LX06_'))}
 env.update(DC_ENDPOINT='ws://127.0.0.1:'+str(listener.getsockname()[1])+'/fixture',DC_CREDENTIAL='fixture-key')
 result=subprocess.run(prefix+['--check','--provider','doubao'],env=env,capture_output=True,timeout=8)
 thread.join(timeout=5);listener.close();assert not thread.is_alive() and not failure,failure
 accepted=reply in ('session.created','session.updated')
 assert result.returncode==(0 if accepted else 1),(reply,result.returncode,result.stdout,result.stderr)
 assert result.stdout==(b'transport: authenticated session ready\n' if accepted else b'transport: connection check failed\n')
 assert not result.stderr and observed==['session.create','session.close']
 return dict(provider='doubao',connection_probe=reply,authenticated_ready_required=True,no_audio_or_turn=True,passed=True)

def greeting(prefix,provider,cancelled=False):
 with Fixture(prefix,provider) as f:
  f.send(GREET,'我在呢，请说。'.encode(),generation=90)
  message=f.cloud.object()
  assert message['text']=='我在呢，请说。' and message['type']==('listen' if provider=='xiaozhi' else 'speech_text_buffer.commit')
  if provider=='xiaozhi':assert message['state']=='detect'
  f.start_reply(1);assert f.expect(BEGIN)['sequence']==1
  raw=encoded() if provider=='xiaozhi' else struct.pack('<h',777)*1440
  f.audio(raw,1)
  if cancelled:
   f.send(CANCEL,generation=90,sequence=1)
   assert f.cloud.object()['type']==('abort' if provider=='xiaozhi' else 'response.cancel')
   if provider=='xiaozhi':assert f.cloud.object()['state']=='start'
   else:f.cloud.send(dict(type='response.done',response_id='r1'))
   resumed=f.expect(RESUMED);assert resumed['generation']==90 and resumed['sequence']==1
   f.quiet()
  else:
   f.send(GRANT,generation=90,sequence=1);assert f.expect(AUDIO)['generation']==90
   f.done(1);assert f.expect(END)['generation']==90
  f.send(MIC,struct.pack('<h',222)*320,flags=2)
  f.send(RESUME,flags=1)
  # XZ pads the final20ms to one60ms packet; DB sends one20ms append.
  if provider=='xiaozhi':assert f.cloud.receive()[0]==2
  else:assert f.cloud.object()['type']=='input_audio_buffer.append'
  assert f.cloud.object()['type']==('listen' if provider=='xiaozhi' else 'input_audio_buffer.commit')
  f.start_reply(2);assert f.expect(BEGIN)['sequence']==2
  f.send(GRANT,generation=91,sequence=2);f.audio(raw,2);assert f.expect(AUDIO)['generation']==91
  f.done(2);assert f.expect(END)['generation']==91
 return dict(provider=provider,greeting=True,pre_grant_cancel=cancelled,first_question_separate=True,passed=True)

def heartbeat_case(prefix,kind):
 settings=dict(XZ_PING_INTERVAL_MS='100',XZ_PONG_TIMEOUT_MS='150')
 if kind=='disabled':settings['XZ_PING_INTERVAL_MS']='0'
 with Fixture(prefix,'xiaozhi',settings=settings) as f:
  if kind=='disabled':time.sleep(.22);f.cloud.quiet()
  else:
   opcode,token=f.cloud.receive();assert opcode==9 and len(token)==8 and int.from_bytes(token,'big')==1
   if kind=='reply':
    f.cloud.send(b'wrong',10);f.cloud.send(token,10)
    opcode,next_token=f.cloud.receive();assert opcode==9 and int.from_bytes(next_token,'big')==2
    f.cloud.send(token,10) # Old PONG must not acknowledge the new outstanding ping.
    f.cloud.send(next_token,10);time.sleep(.03);assert f.process.poll() is None
   else:
    if kind=='wrong':f.cloud.send((999).to_bytes(8,'big'),10)
    f.process.wait(timeout=2);assert f.process.returncode==1
 return dict(provider='xiaozhi',heartbeat=kind,passed=True)

def connect_deadline(prefix,provider,phase):
 listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(4)
 complete=threading.Event();failures=[]
 def server():
  try:
   connection,_=listener.accept();connection.settimeout(3)
   with connection:
    request=b''
    while not request.endswith(b'\r\n\r\n'):request+=exact(connection,1)
    if phase=='session':
     headers={k.lower():v.strip() for k,v in (line.split(':',1) for line in request.decode().split('\r\n')[1:] if ':' in line)}
     accept=base64.b64encode(hashlib.sha1((headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
     connection.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
     assert WebSocket(connection).object()['type']==('hello' if provider=='xiaozhi' else 'session.create')
    else:
     # A trickled HTTP header defeats a per-read timeout; the worker's one
     # deadline must still bound the entire connection and provider readiness.
     for byte in b'HTTP/1.1 101 Switching Protocols\r\n':
      connection.sendall(bytes((byte,)))
      if complete.wait(.08):break
    complete.wait(3)
  except OSError:
   if not complete.is_set():failures.append('fixture_socket_error')
  except BaseException as error:failures.append(repr(error))
 thread=threading.Thread(target=server,daemon=True);thread.start()
 core,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET)
 env={k:v for k,v in os.environ.items() if not k.startswith(('XZ_','DB_','DC_','LX06_'))}
 env.update(DC_ENDPOINT='ws://127.0.0.1:'+str(listener.getsockname()[1])+'/fixture',DC_CREDENTIAL='fixture-key',DC_DEVICE_ID='fixture-device',DC_CLIENT_ID='fixture-client')
 env[('XZ' if provider=='xiaozhi' else 'DB')+'_CONNECT_TIMEOUT_MS']='1000'
 start=time.monotonic()
 p=subprocess.Popen(['/bin/sh','-c','exec 3<&"$1"; shift; exec "$@"','fixture',str(child.fileno()),*prefix,'--provider',provider],pass_fds=(child.fileno(),),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 child.close();core.send(HEADER.pack(MAGIC,1,OPEN,0,0,EPOCH,0,0,0,0,0))
 stdout,stderr=p.communicate(timeout=3);elapsed=time.monotonic()-start
 complete.set();thread.join(1);core.close();listener.close()
 assert p.returncode==124 and not stdout and not stderr and .8<elapsed<2.0,(p.returncode,elapsed,stdout,stderr)
 assert not thread.is_alive() and not failures,failures
 return dict(provider=provider,deadline_phase=phase,total_elapsed_ms=round(elapsed*1000,2),passed=True)

def invalid_settings(prefix):
 count=0
 for name,values in [('XZ_CONNECT_TIMEOUT_MS',['','999','60001','-1','1000x']),('DB_CONNECT_TIMEOUT_MS',['0','999999999999999999999']),('XZ_PING_INTERVAL_MS',['99','300001','-1']),('XZ_PONG_TIMEOUT_MS',['0','99','60001'])]:
  for value in values:
   env={k:v for k,v in os.environ.items() if not k.startswith(('XZ_','DB_','DC_','LX06_'))};env[name]=value
   p=subprocess.run(prefix+['--provider','doubao' if name.startswith('DB_') else 'xiaozhi'],env=env,capture_output=True,timeout=3)
   assert p.returncode==2 and not p.stdout and not p.stderr,(name,value,p.returncode,p.stdout,p.stderr);count+=1
 return dict(invalid_settings=count,strict_bounds=True,no_network=True,passed=True)

def main():
 parser=argparse.ArgumentParser();parser.add_argument('--output',required=True);args=parser.parse_args()
 out=Path(args.output).resolve();assert not (out/'fixture-report.json').exists()
 assert os.readlink('/proc/self/ns/net')!=os.readlink('/proc/1/ns/net'),'run under unshare --net'
 run(['ip','link','set','lo','up'])
 sources={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in ('runtime/doubao/src/main.c','runtime/doubao/src/player.c','runtime/doubao/doubao_watch.sh.tpl')}
 origins=json.loads((HERE/'support-origin.json').read_text())['files']
 deltas={item['path']:item for item in json.loads((HERE/'support-local-deltas.json').read_text())['files']}
 for item in origins:
  delta=deltas.get(item['path']);assert not delta or delta['base_sha256']==item['sha256']
  assert hashlib.sha256((HERE/item['path']).read_bytes()).hexdigest()==(delta['sha256'] if delta else item['sha256'])
  assert hashlib.sha256((ROOT.parent/item['origin']).read_bytes()).hexdigest()==item['origin_sha256']
 report=dict(passed=False,device_access=False,cloud_requests=False,loopback_synthetic_only=True,results={})
 for mode in ('host','sanitize','arm'):
  reference=json.loads((out/mode/'source-reference.json').read_text())
  assert reference['binary']['sha256']==hashlib.sha256((out/mode/'lx06-transport-adapter').read_bytes()).hexdigest()
  assert reference['sources']=={p:hashlib.sha256((HERE/p).read_bytes()).hexdigest() for p in reference['sources']}
 with tempfile.TemporaryDirectory(prefix='dc-adapter-') as temporary:
  td=Path(temporary);tree=td/'root'
  run(['unsquashfs','-no-progress','-d',tree,ROOT/'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'])
  shutil.copyfile(shutil.which('qemu-arm-static'),tree/'qemu-arm-static');(tree/'qemu-arm-static').chmod(0o755)
  (tree/'tmp').mkdir(exist_ok=True)
  (tree/'proc').mkdir(exist_ok=True)
  (tree/'proc/meminfo').write_text('MemTotal: 65536 kB\nMemAvailable: 32768 kB\n')
  (tree/'dev').mkdir(exist_ok=True)
  # The host's device policy refuses mknod entropy devices in a temporary
  # chroot. Fixture-only regular files provide host-generated OS randomness;
  # neither product bytes nor the real device's entropy source are changed.
  for name in ('random','urandom'):
   path=tree/'dev'/name
   if path.exists() or path.is_symlink():path.unlink()
   path.write_bytes(os.urandom(65536));path.chmod(0o600)
  shutil.copyfile(out/'arm/lx06-transport-adapter',tree/'tmp/adapter');(tree/'tmp/adapter').chmod(0o755)
  shutil.copyfile(ROOT.parent/'scratch/libopus.so.0.6.1',tree/'usr/lib/libopus.so.0')
  run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=0.12','-ac','1','-c:a','libopus',td/'fixture.ogg'])
  oggdata=(td/'fixture.ogg').read_bytes()
  for label,prefix in [('host',[str(out/'host/lx06-transport-adapter')]),('sanitize',[str(out/'sanitize/lx06-transport-adapter')]),('stock_arm',['chroot',str(tree),'/qemu-arm-static','/tmp/adapter'])]:
   probe=run(prefix+['--protocol']);assert probe.stdout==b'LX06DCTRANSPORT/1\n' and not probe.stderr
   results=[]
   for mode in ('guarded','native'):
    for provider,ogg in (('xiaozhi',False),('doubao',False),('doubao',True)):results.append(full(prefix,provider,mode,ogg,oggdata))
   for provider in ('xiaozhi','doubao'):results.extend((hold_bound(prefix,provider),wrong_epoch(prefix,provider),commit_tail(prefix,provider)))
   results.append(repeated_cancel(prefix))
   for reply in ('session.created','session.updated','error','response.output_audio.started'):results.append(connection_probe(prefix,reply))
   for provider in ('xiaozhi','doubao'):
    for cancelled in (False,True):results.append(greeting(prefix,provider,cancelled))
    for phase in ('upgrade','session'):results.append(connect_deadline(prefix,provider,phase))
   for kind in ('reply','missing','wrong','disabled'):results.append(heartbeat_case(prefix,kind))
   results.append(invalid_settings(prefix))
   report['results'][label]=results
 report['original_sources_unchanged']=sources=={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sources};assert report['original_sources_unchanged']
 report['passed']=True
 report['binaries']={mode:dict(sha256=hashlib.sha256((out/mode/'lx06-transport-adapter').read_bytes()).hexdigest(),bytes=(out/mode/'lx06-transport-adapter').stat().st_size) for mode in ('host','sanitize','arm')}
 report['source_references']={mode:hashlib.sha256((out/mode/'source-reference.json').read_bytes()).hexdigest() for mode in ('host','sanitize','arm')}
 (out/'fixture-report.json').write_text(json.dumps(report,indent=2)+'\n')
 print(json.dumps(dict(passed=True,host_cases=1+len(report['results']['host']),stock_arm_cases=1+len(report['results']['stock_arm']),original_sources_unchanged=True,device_access=False,cloud_requests=False)))
if __name__=='__main__':main()

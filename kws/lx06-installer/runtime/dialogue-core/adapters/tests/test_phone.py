"""Phone worker synthetic TLS/WSS integration, private netns/loopback only.

No phone, speaker, actual audio, account, Internet or call is used. Probe
diagnostics deliberately cannot satisfy production readiness. Synthetic wake
text is compared only in memory and is not included in reports or exceptions.
"""
import argparse,base64,hashlib,json,os,queue,select,shutil,socket,ssl,struct
import subprocess,tempfile,threading,time
from pathlib import Path

HERE=Path(__file__).resolve().parent
ADAPTER=HERE.parent
ROOT=ADAPTER.parents[2]
HEADER=struct.Struct('<IHHIIQQQIHH')
TOKEN='synthetic_test_pairing_token_1234567890'
EPOCH=123
COMMAND='synthetic_cmd_123'
PATH='/lx06/phone-audio/v1'
COMMAND_DEADLINE=int(time.time()*1000)+15000

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def execute(args,**kw):
    p=subprocess.run(list(map(str,args)),capture_output=True,**kw)
    if p.returncode:raise RuntimeError('fixture command failed')
    return p.stdout
def packet(typ,payload=b'',gen=0,seq=0,epoch=EPOCH,flags=0,rate=0,channels=0):
    if isinstance(payload,dict):payload=json.dumps(payload,separators=(',',':'),ensure_ascii=False).encode()
    return HEADER.pack(0x31474344,1,typ,len(payload),flags,epoch,gen,seq,rate,channels,0)+payload
def unpack(b):
    assert len(b)>=HEADER.size,'short local packet'
    h=HEADER.unpack_from(b);assert len(b)==HEADER.size+h[3],'local size'
    return dict(type=h[2],flags=h[4],epoch=h[5],generation=h[6],sequence=h[7],rate=h[8],channels=h[9],payload=b[HEADER.size:])
def envelope(**changes):
    p=dict(v=2,deadline_unix_ms=str(COMMAND_DEADLINE),epoch=str(EPOCH),command_id=COMMAND,script_id='phone_doubao.sh',wake_source='official_asr',wake_text='合成测试唤醒',action='probe.attach',target='doubao',params={})
    p.update(changes);return p
def ack(status='completed',**changes):
    p=dict(v=2,kind='command_ack',epoch=str(EPOCH),command_id=COMMAND,status=status,code='probe_attached',replayed=False);p.update(changes);return packet(35,p)
def ready(production=False,**changes):
    p=dict(probe_only=not production,call_ready=production,capabilities=dict(virtual_mic=True,rtc_capture=True,call_lifecycle=production,reply_lifecycle=production,turn_fence=False,automatic_ui=False))
    p.update(changes);return packet(32,p)
def read_exact(s,n):
    out=b''
    while len(out)<n:
        b=s.recv(n-len(out))
        if not b:raise EOFError
        out+=b
    return out
def read_frame(s):
    h=read_exact(s,2);assert h[0]&128 and h[1]&128,'client frame mask/final'
    n=h[1]&127
    if n==126:n=struct.unpack('!H',read_exact(s,2))[0];assert n>=126
    assert n!=127 and n<=8240,'client frame size'
    mask=read_exact(s,4);b=read_exact(s,n)
    return h[0]&15,bytes(v^mask[i&3] for i,v in enumerate(b))
def frame(b,opcode=2,fin=True,masked=False,noncanonical=False):
    h=bytes([(128 if fin else 0)|opcode])
    if len(b)<126 and not noncanonical:h+=bytes([len(b)|(128 if masked else 0)])
    else:h+=bytes([126|(128 if masked else 0)])+struct.pack('!H',len(b))
    if masked:h+=b'\0'*4
    return h+b

class Server:
    def __init__(self,cert,key,handler,upgrade='normal'):
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(1);self.listener.settimeout(6)
        self.port=self.listener.getsockname()[1];self.error=None;self.received=queue.Queue();self.connected=threading.Event();self.accepted=False
        self.context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);self.context.maximum_version=ssl.TLSVersion.TLSv1_2;self.context.minimum_version=ssl.TLSVersion.TLSv1_2;self.context.load_cert_chain(cert,key)
        def serve():
            try:
                raw,_=self.listener.accept();raw.settimeout(6)
                with self.context.wrap_socket(raw,server_side=True) as s:
                    b=b''
                    while not b.endswith(b'\r\n\r\n'):
                        b+=read_exact(s,1);assert len(b)<8192
                    lines=b.decode('ascii').split('\r\n');assert lines[0]==f'GET {PATH} HTTP/1.1'
                    headers=dict(line.split(': ',1) for line in lines[1:] if line)
                    assert headers['Authorization']=='Bearer '+TOKEN,'pairing authentication'
                    assert TOKEN not in lines[0],'token URL'
                    accept=base64.b64encode(hashlib.sha1((headers['Sec-WebSocket-Key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                    if upgrade=='wrong_accept':accept='wrong'
                    response=f'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n'
                    if upgrade=='duplicate':response+='Upgrade: websocket\r\n'
                    s.sendall((response+'\r\n').encode());self.accepted=True;self.connected.set()
                    if upgrade!='normal':return
                    handler(self,s)
            except (EOFError,ssl.SSLError,ConnectionError,socket.timeout,OSError) as e:
                # Expected in refused TLS, malformed input and explicit CLOSE.
                self.transport_end=type(e).__name__
            except BaseException as e:self.error=e
            finally:self.connected.set();self.listener.close()
        self.thread=threading.Thread(target=serve,daemon=True);self.thread.start()
    def join(self):
        self.thread.join(7);assert not self.thread.is_alive(),'server shutdown deadline'
        if self.error:raise AssertionError('mock server assertion') from self.error

class Worker:
    def __init__(self,binary,config,launch=None,extra_env=None):
        self.socket,child=socket.socketpair(socket.AF_UNIX,socket.SOCK_SEQPACKET);self.socket.settimeout(3)
        childfd=child.fileno();parentfd=self.socket.fileno()
        def prepare():os.dup2(childfd,3)
        env=dict(os.environ,PHONE_PAIRING_CONFIG=str(config),ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
        env.update(extra_env or {})
        self.p=subprocess.Popen((launch or [])+[str(binary)],env=env,pass_fds=tuple(set([childfd,3])),preexec_fn=prepare,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        child.close();self.events=[]
    def send(self,b):self.socket.send(b)
    def read(self,timeout=2):
        self.socket.settimeout(timeout)
        try:b=self.socket.recv(8240)
        except socket.timeout:return None
        if not b:return None
        p=unpack(b);self.events.append(p);return p
    def until(self,typ,timeout=3):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            p=self.read(max(.01,end-time.monotonic()))
            if p and p['type']==typ:return p
            if self.p.poll() is not None and not p:break
        raise AssertionError('expected packet type not received')
    def close(self,expected=None):
        if self.p.poll() is None:
            try:self.send(packet(5))
            except OSError:pass
        try:stdout,stderr=self.p.communicate(timeout=4)
        except subprocess.TimeoutExpired:self.p.kill();self.p.communicate();raise AssertionError('worker shutdown deadline')
        self.socket.close();assert not stdout and not stderr,'worker emitted raw output'
        if expected is not None:assert self.p.returncode==expected,'unexpected worker exit'
        return self.p.returncode

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True);parser.add_argument('--mode',choices=['host','sanitize','arm'],default='host');parser.add_argument('--binary',type=Path);args=parser.parse_args()
    if os.environ.get('LX06_PHONE_NETNS')!='1':
        env=dict(os.environ,LX06_PHONE_NETNS='1')
        command=['unshare','--net','/bin/sh','-c','ip link set lo up && exec /usr/bin/python3 "$@"','phone-netns',__file__,*os.sys.argv[1:]]
        p=subprocess.run(command,env=env);raise SystemExit(p.returncode)
    assert os.geteuid()==0,'root-only transport fixture'
    output=Path(args.output).resolve();assert output.parent==(ROOT/'build').resolve() and not output.exists();output.mkdir()
    paths=[ADAPTER/'phone.c',ADAPTER/'phone.h',ADAPTER/'phone_worker.c',ADAPTER/'build_phone.sh',ROOT/'runtime/dialogue-core/protocol.c',ROOT/'runtime/dialogue-core/protocol.h',ROOT/'runtime/dialogue-core/io/compat.h',ADAPTER/'support/xiaozhi/ws_frame.c',ADAPTER/'support/xiaozhi/ws_frame.h',Path(__file__),HERE/'phone_clock_fixture.c']
    report=dict(passed=False,actual_device=False,cloud_requests=0,actual_audio=False,private_network_namespace=True,mode=args.mode,sources={str(p.relative_to(ROOT)):sha(p) for p in paths},cases=[])
    def save():(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    save()
    try:
        binary=args.binary.resolve() if args.binary else output/'worker'/'lx06-phone-worker'
        if not args.binary:execute(['bash',ADAPTER/'build_phone.sh',args.mode],env=dict(os.environ,DC_PHONE_BUILD_DIR=str(binary.parent)))
        clock_binary=output/'clock-worker'/'lx06-phone-worker'
        execute(['bash',ADAPTER/'build_phone.sh',args.mode],env=dict(os.environ,DC_PHONE_BUILD_DIR=str(clock_binary.parent),DC_PHONE_CLOCK_FIXTURE='1'))
        report['test_only_clock_binary']=dict(sha256=sha(clock_binary),production=False)
        launch=None
        with tempfile.TemporaryDirectory(prefix='dc-phone-') as tmp:
            tmp=Path(tmp);tmp.chmod(0o700)
            ca=tmp/'ca.pem';key=tmp/'key.pem';csr=tmp/'csr';cert=tmp/'cert.pem';cakey=tmp/'ca-key.pem'
            execute(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',cakey,'-out',ca,'-days','1','-subj','/CN=Offline fixture CA'])
            execute(['openssl','req','-new','-newkey','rsa:2048','-nodes','-keyout',key,'-out',csr,'-subj','/CN=Offline fixture'])
            ext=tmp/'ext';ext.write_text('subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n')
            execute(['openssl','x509','-req','-in',csr,'-CA',ca,'-CAkey',cakey,'-CAcreateserial','-out',cert,'-days','1','-extfile',ext])
            public=execute(['openssl','x509','-in',cert,'-pubkey','-noout']);der=execute(['openssl','pkey','-pubin','-outform','DER'],input=public);pin=hashlib.sha256(der).hexdigest()
            config=tmp/'pairing.json'
            stock=None
            if args.mode=='arm':
                stock=tmp/'stock';execute(['unsquashfs','-no-progress','-d',stock,ROOT/'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'])
                # QEMU prefix loads the stock loader/libraries; all syscalls and
                # network stay in the same isolated host namespace. No chrooted
                # speaker or SDK is opened by the network-only executable.
                launch=['qemu-arm','-L',str(stock)]
            def setup(handler,**changes):
                global COMMAND_DEADLINE
                COMMAND_DEADLINE=int(time.time()*1000)+15000
                server=Server(cert,key,handler,changes.pop('upgrade','normal'))
                c=dict(endpoint=f'wss://127.0.0.1:{server.port}{PATH}',token=TOKEN,ca_file=str(ca),spki_sha256=pin);c.update(changes);config.write_text(json.dumps(c));config.chmod(0o600)
                return server,Worker(binary,config,launch)
            def opened(s):
                op,b=read_frame(s);assert op==2 and unpack(b)['type']==1
                assert json.loads(unpack(b)['payload'])==envelope(),'structured wake transport'
            def metadata(server,s):
                opened(s);s.sendall(frame(ack())+frame(ready()))
                while True:
                    op,b=read_frame(s)
                    if op==2:server.received.put(unpack(b))
                    if op==2 and unpack(b)['type']==5:return
            def case(name,fn):
                fn();report['cases'].append(dict(name=name,passed=True));save()
            def test_probe():
                s,w=setup(metadata);w.send(packet(1,envelope()));a=w.until(35);assert json.loads(a['payload'])['kind']=='command_ack' and a['generation']==a['sequence']==0
                status=w.until(35);assert json.loads(status['payload'])==dict(v=1,kind='phone_status',production_capable=False)
                assert w.read(.2) is None and not any(e['type'] in (32,37) for e in w.events),'probe became production'
                w.send(packet(4,gen=1));assert w.until(40)['payload']==b'turn_fence_unavailable'
                w.send(packet(3));c=s.received.get(timeout=2);assert c['type']==3
                w.send(packet(3));assert w.read(.1) is None
                w.close(0);s.join()
            case('probe_metadata_no_ready_no_pcm_no_fence_resume',test_probe)
            for name,kwargs in [('wrong_spki',dict(spki_sha256='00'*32)),('wrong_ca',dict(ca_file=str(cert))),('bad_upgrade_accept',dict(upgrade='wrong_accept')),('duplicate_upgrade_header',dict(upgrade='duplicate'))]:
                def refused(kwargs=kwargs):
                    s,w=setup(metadata,**kwargs);w.send(packet(1,envelope()));w.until(40);assert w.close()!=0;s.join();assert not any(e['type']==32 for e in w.events)
                case(name,refused)
            def readiness_order():
                gate=threading.Event()
                def handler(server,s):
                    opened(s);s.sendall(frame(ready(True)));gate.wait(3);s.sendall(frame(ack()));read_frame(s)
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(35);assert w.read(.15) is None,'READY before completed ACK';gate.set();w.until(35);w.until(32);w.close(0);s.join()
            case('production_ready_requires_matching_completed_ack_and_caps',readiness_order)
            for label,diagnostic in [('missing_reply_lifecycle',ready(True,capabilities=dict(virtual_mic=True,rtc_capture=True,call_lifecycle=True,reply_lifecycle=False,turn_fence=False,automatic_ui=False))),('call_not_ready',ready(True,call_ready=False)),('probe_claims_ready',ready(True,probe_only=True))]:
                def insufficient(diagnostic=diagnostic):
                    def handler(server,s):
                        opened(s);s.sendall(frame(ack())+frame(diagnostic)+frame(packet(37,b'00',gen=0,seq=1,flags=2,rate=16000,channels=1)))
                        while True:
                            _,b=read_frame(s)
                            if unpack(b)['type']==5:return
                    s,w=setup(handler);w.send(packet(1,envelope()));w.until(35);status=w.until(35);assert not json.loads(status['payload'])['production_capable'];assert w.read(.1) is None;assert not any(e['type'] in (32,37) for e in w.events);w.close(0);s.join()
                case(label,insufficient)
            for label,change in [('wrong_ack_id',dict(command_id='wrong_command_id')),('wrong_ack_epoch',dict(epoch='124')),('wrong_ack_kind',dict(kind='text')),('old_ack_schema',dict(v=1)),('replayed_not_boolean',dict(replayed='false'))]:
                def bad_ack(change=change):
                    def handler(server,s):opened(s);s.sendall(frame(ack(**change)));read_frame(s)
                    s,w=setup(handler);w.send(packet(1,envelope()));w.until(40);assert w.close()!=0;s.join()
                case(label,bad_ack)
            for label,raw in [('duplicate_json_key',b'{"v":1,"v":1}'),('invalid_utf8',b'{"wake_text":"\xff"}'),('unpaired_surrogate',b'{"wake_text":"\\ud800"}'),('unknown_open_field',json.dumps(envelope(extra=1)).encode()),('arbitrary_target',json.dumps(envelope(target='arbitrary_app')).encode()),('arbitrary_action',json.dumps(envelope(action='shell.exec')).encode()),('wrong_epoch_string',json.dumps(envelope(epoch='124')).encode()),('path_script_id',json.dumps(envelope(script_id='../invalid.sh')).encode()),('nested_params',json.dumps(envelope(params={'a':{'b':{'c':{'d':{'e':1}}}}})).encode())]:
                def bad_open(raw=raw):
                    config.write_text(json.dumps(dict(endpoint='wss://127.0.0.1:9'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin)));config.chmod(0o600)
                    w=Worker(binary,config,launch);w.send(packet(1,raw));w.close(2)
                case(label,bad_open)
            for label,change in [('old_command_schema',dict(v=1)),('deadline_not_string',dict(deadline_unix_ms=123)),('deadline_integer_overflow',dict(deadline_unix_ms='9223372036854775808'))]:
                def invalid_deadline_schema(change=change):
                    config.write_text(json.dumps(dict(endpoint='wss://127.0.0.1:9'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin)));config.chmod(0o600)
                    w=Worker(binary,config,launch);w.send(packet(1,envelope(**change)));w.close(2)
                case(label,invalid_deadline_schema)
            for label,mutation in [('public_ipv4',dict(endpoint='wss://8.8.8.8:443'+PATH)),('dns_endpoint',dict(endpoint='wss://example.invalid:443'+PATH)),('plain_ws',dict(endpoint='ws://127.0.0.1:9'+PATH)),('url_token',dict(endpoint='wss://127.0.0.1:9'+PATH+'?token='+TOKEN)),('short_token',dict(token='short'))]:
                def bad_config(mutation=mutation):
                    c=dict(endpoint='wss://127.0.0.1:9'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin);c.update(mutation);config.write_text(json.dumps(c));config.chmod(0o600)
                    w=Worker(binary,config,launch);w.close(2)
                case(label,bad_config)
            def permissions():
                config.chmod(0o644);w=Worker(binary,config,launch);w.close(2);config.chmod(0o600)
                link=tmp/'pairing-link';link.symlink_to(config);w=Worker(binary,link,launch);w.close(2)
            case('private_config_permissions_and_symlink_refusal',permissions)
            for label,bad in [('fragmented_frame',frame(ack(),fin=False)),('masked_server_frame',frame(ack(),masked=True)),('text_frame',frame(b'bad',opcode=1)),('noncanonical_length',frame(b'x',noncanonical=True)),('zero_channel_pcm',frame(packet(37,b'00',flags=2,rate=16000,channels=0))),('duplicate_ack_json',frame(packet(35,b'{"v":1,"v":1}')))]:
                def malformed(bad=bad):
                    def handler(server,s):opened(s);s.sendall(bad);read_frame(s)
                    s,w=setup(handler);w.send(packet(1,envelope()));w.until(40);assert w.close()!=0;s.join()
                case(label,malformed)
            def audio_cancel():
                sent=threading.Event();cancelled=threading.Event()
                def handler(server,s):
                    opened(s);s.sendall(frame(ack())+frame(ready(True))+frame(packet(36,seq=7)))
                    while True:
                        _,b=read_frame(s);p=unpack(b)
                        if p['type']==6 and p['sequence']==7:break
                    assert p['generation']==9
                    s.sendall(frame(packet(37,b'\x11\x22'*320,gen=9,seq=7,flags=2,rate=16000,channels=1)));sent.set()
                    while True:
                        _,b=read_frame(s);p=unpack(b)
                        if p['type']==3:break
                    assert p['epoch']==EPOCH and p['generation']==9 and p['sequence']==7;cancelled.set()
                    # Stale, same-generation output retains its identity and
                    # cannot reappear after cancellation or fake RESUME. ACK the
                    # identity of the actual CANCEL received over the wire.
                    s.sendall(frame(packet(37,b'\x33\x44'*320,gen=9,seq=7,flags=2,rate=16000,channels=1))+frame(packet(39,epoch=p['epoch'],gen=p['generation'],seq=p['sequence'])))
                    while True:
                        _,b=read_frame(s)
                        if unpack(b)['type']==5:return
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(32);w.until(36);w.send(packet(6,gen=9,seq=7));audio=w.until(37);assert audio['generation']==9 and audio['sequence']==7 and audio['payload']==b'\x11\x22'*320
                started=time.monotonic();w.send(packet(3,gen=9,seq=7));w.until(39);assert time.monotonic()-started<.5 and cancelled.is_set(),'cancel blocked by network work'
                w.send(packet(4,gen=10));assert w.until(40)['payload']==b'turn_fence_unavailable';assert w.read(.15) is None,'stale audio after cancel'
                w.close(0);s.join()
            case('genuine_reply_grant_preserved_cancel_freezes_stale_audio',audio_cancel)
            def bad_generation():
                def handler(server,s):
                    opened(s);s.sendall(frame(ack())+frame(ready(True))+frame(packet(36,seq=1)))
                    while True:
                        _,b=read_frame(s)
                        if unpack(b)['type']==6 and unpack(b)['sequence']==1:break
                    s.sendall(frame(packet(37,b'00',gen=8,seq=1,flags=2,rate=16000,channels=1)));read_frame(s)
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(32);w.until(36);w.send(packet(6,gen=9,seq=1));w.until(40);assert not any(e['type']==37 for e in w.events);assert w.close()!=0;s.join()
            case('never_relabel_wrong_generation_audio',bad_generation)
            def no_grant_audio():
                def handler(server,s):
                    opened(s);s.sendall(frame(ack())+frame(ready(True))+frame(packet(36,seq=1))+frame(packet(37,b'00',gen=0,seq=1,flags=2,rate=16000,channels=1)));read_frame(s)
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(40);assert not any(e['type']==37 for e in w.events);assert w.close()!=0;s.join()
            case('pcm_before_core_grant_refused_without_retagging',no_grant_audio)
            def capability_downgrade():
                gate=threading.Event()
                def handler(server,s):opened(s);s.sendall(frame(ack())+frame(ready(True)));gate.wait(2);s.sendall(frame(ready()));read_frame(s)
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(32);gate.set();w.until(40);assert w.close()!=0;s.join()
            case('capability_downgrade_fails_closed',capability_downgrade)
            for status in ['failed','cancelled']:
                def terminal(status=status):
                    def handler(server,s):opened(s);s.sendall(frame(ack(status))+frame(ready(True))+frame(ack('completed')));read_frame(s)
                    s,w=setup(handler);w.send(packet(1,envelope()));w.until(40);assert not any(e['type']==32 for e in w.events);assert w.close()!=0;s.join()
                case('terminal_'+status+'_cannot_reopen',terminal)
            for suffix in [b'\0',b'\0 ']:
                def raw_nul(suffix=suffix):
                    config.write_text(json.dumps(dict(endpoint='wss://127.0.0.1:9'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin)));config.chmod(0o600)
                    w=Worker(binary,config,launch);w.send(packet(1,json.dumps(envelope()).encode()+suffix));w.close(2)
                case('raw_nul_trailing_'+str(len(suffix)),raw_nul)
            def between_nul():
                raw=json.dumps(envelope()).encode().replace(b',',b',\0',1)
                w=Worker(binary,config,launch);w.send(packet(1,raw));w.close(2)
            case('raw_nul_between_fields',between_nul)
            def stale_cancel():
                def handler(server,s):
                    opened(s);s.sendall(frame(ack())+frame(ready(True))+frame(packet(36,seq=1)))
                    while True:
                        _,b=read_frame(s);p=unpack(b)
                        if p['type']==6 and p['sequence']==1:break
                    assert p['generation']==8;s.sendall(frame(packet(38,gen=8,seq=1))+frame(packet(36,seq=2)))
                    while True:
                        _,b=read_frame(s);p=unpack(b)
                        if p['type']==6 and p['sequence']==2:break
                    assert p['generation']==9
                    while True:
                        _,b=read_frame(s);p=unpack(b)
                        assert p['type']!=3,'stale cancel forwarded'
                        if p['type']==6 and not p['sequence']:break
                    s.sendall(frame(packet(37,b'00',gen=9,seq=2,flags=2,rate=16000,channels=1)))
                    while True:
                        _,b=read_frame(s)
                        if unpack(b)['type']==5:return
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(32);w.until(36);w.send(packet(6,gen=8,seq=1));w.until(38);w.until(36);w.send(packet(6,gen=9,seq=2));w.send(packet(3,gen=8,seq=1));w.send(packet(6));a=w.until(37);assert a['generation']==9 and a['sequence']==2;w.close(0);s.join()
            case('old_cancel_cannot_freeze_new_reply',stale_cancel)
            for label,delta,reason in [('expired_deadline',-1,'deadline_exceeded'),('future_deadline_cap',31000,'deadline_out_of_range')]:
                def refused_deadline(delta=delta,reason=reason):
                    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1)
                    config.write_text(json.dumps(dict(endpoint=f'wss://127.0.0.1:{listener.getsockname()[1]}'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin)));config.chmod(0o600)
                    w=Worker(binary,config,launch);w.send(packet(1,envelope(deadline_unix_ms=str(int(time.time()*1000)+delta))))
                    result=json.loads(w.until(35)['payload']);assert result['status']=='failed' and result['code']==reason and result['replayed'] is False
                    assert not select.select([listener],[],[],.05)[0],'rejected command connected to network'
                    assert w.close(0)==0;listener.close()
                case(label,refused_deadline)
            def command_timeout():
                global COMMAND_DEADLINE
                def handler(server,s):opened(s);read_frame(s)
                s,w=setup(handler);COMMAND_DEADLINE=int(time.time()*1000)+180
                w.send(packet(1,envelope()));r=json.loads(w.until(35,2)['payload']);assert r['code']=='deadline_exceeded' and r['replayed'] is False
                assert not any(e['type']==32 for e in w.events);w.close(0);s.join()
            case('pending_command_deadline_closes_remote_without_ready',command_timeout)
            for clock_mode in ['zero','future','unavailable','mono_unavailable']:
                def untrusted_clock(clock_mode=clock_mode):
                    config.write_text(json.dumps(dict(endpoint='wss://127.0.0.1:9'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin)));config.chmod(0o600)
                    w=Worker(clock_binary,config,launch,dict(PHONE_FIXTURE_CLOCK=clock_mode));w.send(packet(1,envelope(deadline_unix_ms=str(int(time.time()*1000)+1000))))
                    r=json.loads(w.until(35)['payload']);assert r['code']=='clock_untrusted';w.close(0)
                case('untrusted_clock_'+clock_mode,untrusted_clock)
            def wall_jump():
                global COMMAND_DEADLINE
                def handler(server,s):opened(s);read_frame(s)
                s=Server(cert,key,handler)
                config.write_text(json.dumps(dict(endpoint=f'wss://127.0.0.1:{s.port}'+PATH,token=TOKEN,ca_file=str(ca),spki_sha256=pin)));config.chmod(0o600)
                w=Worker(clock_binary,config,launch,dict(PHONE_FIXTURE_CLOCK='backwards_after_first'));COMMAND_DEADLINE=int(time.time()*1000)+180
                w.send(packet(1,envelope()));start=time.monotonic();r=json.loads(w.until(35,2)['payload']);assert r['code']=='deadline_exceeded' and time.monotonic()-start<1
                w.close(0);s.join()
            case('accepted_deadline_uses_monotonic_under_wall_clock_jump',wall_jump)
            def duplicate_open():
                gate=threading.Event()
                def handler(server,s):
                    opened(s);s.sendall(frame(ack('started',code='accepted')))
                    gate.wait(2)
                    while True:
                        _,b=read_frame(s);p=unpack(b);assert p['type']!=1,'duplicate action dispatched'
                        if p['type']==6:break
                    s.sendall(frame(ack())+frame(ready(True)));read_frame(s)
                s,w=setup(handler);payload=envelope();w.send(packet(1,payload));w.until(35)
                w.send(packet(1,payload));r=json.loads(w.until(35)['payload']);assert r['status']=='started' and r['replayed'] is True
                gate.set();w.send(packet(6));w.until(32);w.close(0);s.join()
            case('duplicate_open_replays_pending_metadata_without_dispatch',duplicate_open)
            def conflicting_open():
                gate=threading.Event()
                def handler(server,s):opened(s);s.sendall(frame(ack('started',code='accepted')));gate.wait(2);s.sendall(frame(ack())+frame(ready(True)));read_frame(s)
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(35)
                w.send(packet(1,envelope(wake_text='another synthetic wake')));r=json.loads(w.until(35)['payload']);assert r['code']=='command_conflict' and r['replayed'] is False
                gate.set();w.until(32);w.close(0);s.join()
            case('conflicting_duplicate_cannot_replace_current_command',conflicting_open)
            def historical_ack():
                def handler(server,s):opened(s);s.sendall(frame(ack(replayed=True))+frame(ready(True))+frame(packet(36,seq=1)));read_frame(s)
                s,w=setup(handler);w.send(packet(1,envelope()));r=json.loads(w.until(35)['payload']);assert r['replayed'] is True
                time.sleep(.1);w.read(.1);assert not any(e['type'] in (32,36,37) for e in w.events);w.close(0);s.join()
            case('historical_replayed_completion_never_grants_ready',historical_ack)
            def completed_duplicate_after_deadline():
                global COMMAND_DEADLINE
                def handler(server,s):opened(s);s.sendall(frame(ack()));read_frame(s)
                s,w=setup(handler);COMMAND_DEADLINE=int(time.time()*1000)+500;payload=envelope();w.send(packet(1,payload));w.until(35)
                time.sleep(.55);w.send(packet(1,payload));r=json.loads(w.until(35)['payload']);assert r['status']=='completed' and r['replayed'] is True
                assert not any(e['type']==32 for e in w.events);w.close(0);s.join()
            case('completed_duplicate_after_deadline_is_metadata_only',completed_duplicate_after_deadline)
            def stalled_peer():
                def handler(server,s):opened(s);time.sleep(2.5)
                s,w=setup(handler);w.send(packet(1,envelope()));assert s.connected.wait(2)
                started=time.monotonic();w.send(packet(4,gen=1));assert w.until(40,1)['payload']==b'turn_fence_unavailable';assert time.monotonic()-started<.5
                w.close(0);s.join()
            case('control_not_blocked_by_quiet_network',stalled_peer)
            def bounded_output():
                def handler(server,s):
                    opened(s);s.sendall(b''.join(frame(ack('started')) for _ in range(2000)))
                    read_frame(s)
                s,w=setup(handler);w.socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024);w.send(packet(1,envelope()));time.sleep(.4)
                # Output is fixed eight packets plus the bounded Unix socket;
                # overflow fails closed rather than allocating or blocking.
                if w.p.poll() is None:
                    w.send(packet(4,gen=1));time.sleep(.1)
                assert w.p.poll() is not None,'unbounded output or stalled cancellation'
                assert w.close()!=0;s.join()
            case('bounded_local_backpressure_fails_closed',bounded_output)
            def bounded_input():
                gate=threading.Event()
                def handler(server,s):
                    opened(s);s.sendall(frame(ack())+frame(ready(True)));gate.wait(2)
                s,w=setup(handler);w.send(packet(1,envelope()));w.until(32);w.socket.setblocking(False)
                for seq in range(1,1000):
                    try:w.send(packet(2,b'00'*4000,seq=seq,flags=2,rate=16000,channels=1))
                    except (BlockingIOError,BrokenPipeError,ConnectionResetError):break
                time.sleep(.2);gate.set();assert w.p.poll() is not None,'MIC backpressure not bounded';assert w.close()!=0;s.join()
            case('bounded_mic_backpressure_fails_closed',bounded_input)
        report.update(binary=dict(path=str(binary),sha256=sha(binary),bytes=binary.stat().st_size),source_unchanged=all(sha(ROOT/p)==v for p,v in report['sources'].items()))
        assert report['source_unchanged'];report['passed']=True;save();print(json.dumps({k:v for k,v in report.items() if k not in ('sources','cases')},indent=2))
    except BaseException:
        save();raise
if __name__=='__main__':main()

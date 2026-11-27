"""Actual main + actual network adapter with synthetic ASR/WS/ALSA endpoints.

Host-only ownership bypass; no physical speaker, device, cloud account or SDK.
The server supplies protocol fixtures, not a second control recognizer. A slow
cancel barrier verifies local drop precedes network acknowledgement.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import signal
import shlex
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
sys.path.insert(0,str(HERE))
sys.path.insert(0,str(HERE.parent/'adapters/tests'))
from test_reactor import Reactor,SpeakerASR,wait,sha
from test_transports import WebSocket,exact,encoded,valid_uplink

class AudioCloud:
    def __init__(self,provider):
        self.provider=provider;self.events=[];self.errors=[];self.lock=threading.Lock()
        self.stopped=threading.Event();self.reply_stop=None;self.feeders=[]
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen(1);self.listener.settimeout(4)
        self.endpoint='ws://127.0.0.1:'+str(self.listener.getsockname()[1])+'/fixture'
        self.cloud=None;self.uplink_bytes=0;self.commits=0;self.cancels=0;self.closed=False;self.greetings=0
        self.thread=threading.Thread(target=self.serve,daemon=True);self.thread.start()

    def record(self,event):self.events.append(dict(event=event,at_ns=time.monotonic_ns()))
    def send(self,data,kind=1):
        with self.lock:self.cloud.send(data,kind)

    def serve(self):
        try:
            connection,_=self.listener.accept();connection.settimeout(4);request=b''
            while not request.endswith(b'\r\n\r\n'):request+=exact(connection,1)
            headers={k.lower():v.strip() for k,v in (line.split(':',1) for line in request.decode().split('\r\n')[1:] if ':' in line)}
            assert headers.get('x-api-key')=='fixture-key' if self.provider=='doubao' else headers.get('authorization')=='Bearer fixture-key'
            # RFC6455 GUID is fixed protocol data; no account identity.
            accept=base64.b64encode(hashlib.sha1((headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
            connection.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
            self.cloud=WebSocket(connection);first=self.cloud.object()
            if self.provider=='xiaozhi':
                assert first['type']=='hello'
                self.send(dict(type='hello',version=1,transport='websocket',session_id='fixture-session',audio_params=dict(format='opus',sample_rate=24000,channels=1,frame_duration=60)))
            else:
                assert first['type']=='session.create';self.send(dict(type='session.created'))
            self.record('ready')
            while not self.stopped.is_set():
                kind,raw=self.cloud.receive()
                if kind==8:break
                if kind==9:self.send(raw,10);continue
                if kind==2:
                    assert self.provider=='xiaozhi';valid_uplink(raw);self.uplink_bytes+=1920;continue
                assert kind==1;message=json.loads(raw);event=message['type']
                if event=='input_audio_buffer.append':
                    assert self.provider=='doubao';pcm=base64.b64decode(message['audio']);assert len(pcm)==640;self.uplink_bytes+=len(pcm)
                elif event=='speech_text_buffer.commit' or (event=='listen' and message['state']=='detect'):
                    assert message['text']=='我在呢，请说。';self.greetings+=1;self.record('greeting')
                elif event=='listen' and message['state']=='start':self.record('listen')
                elif event=='input_audio_buffer.commit' or (event=='listen' and message['state']=='stop'):
                    self.commits+=1;self.record('commit')
                elif event in ('abort','response.cancel'):
                    self.cancels+=1;self.record('cancel')
                    if self.reply_stop:self.reply_stop.set()
                    if self.provider=='doubao':
                        self.send(dict(type='response.canceled',response_id='r1'))
                        # Deliberately block server acknowledgement; local audio
                        # must already be silent before this network barrier.
                        time.sleep(.25);self.send(dict(type='response.done',response_id='r1'));self.record('resumed')
                elif event in ('session.close','goodbye'):
                    self.closed=True;self.record('closed');break
                else:raise AssertionError(('unexpected cloud command',message))
        except (EOFError,ConnectionError,OSError,AssertionError) as error:
            if not self.stopped.is_set() and not self.closed:self.errors.append(repr(error))

    def reply(self,index,long=False):
        self.reply_stop=threading.Event();stop=self.reply_stop
        self.send(dict(type='tts',state='start') if self.provider=='xiaozhi' else dict(type='response.output_audio.started',response_id='r'+str(index)))
        def feed():
            try:
                opus=encoded();pcm=struct.pack('<h',1000+index)*1440
                for _ in range(100 if long else 4):
                    if stop.is_set() or self.stopped.is_set():return
                    if self.provider=='xiaozhi':self.send(opus,2)
                    else:self.send(dict(type='response.output_audio.delta',response_id='r'+str(index),delta=base64.b64encode(pcm).decode()))
                    if stop.wait(.02):return
                self.send(dict(type='tts',state='stop') if self.provider=='xiaozhi' else dict(type='response.output_audio.done',response_id='r'+str(index)))
                self.record('reply_end_'+str(index))
            except (OSError,ConnectionError):pass
        thread=threading.Thread(target=feed,daemon=True);self.feeders.append(thread);thread.start()

    def close(self):
        self.stopped.set()
        if self.reply_stop:self.reply_stop.set()
        if self.cloud:
            try:self.cloud.sock.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            self.cloud.sock.close()
        self.listener.close();self.thread.join(1)
        for thread in self.feeders:thread.join(1)

class AudioReactor(Reactor):
    def __init__(self,binary,library,adapter,provider,mode,greeting=False,playback=True):
        super().__init__(binary,library);self.adapter=adapter;self.provider=provider;self.mode=mode;self.greeting=greeting;self.playback=playback

    def __enter__(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='dc-audio-reactor-');self.root=Path(self.temporary.name);self.root.chmod(0o700)
        self.mic=self.root/'mic';self.mic.mkdir(mode=0o700)
        self.asr=SpeakerASR().__enter__();self.cloud=AudioCloud(self.provider)
        self.broker=SimpleNamespace(requests=[],errors=[],close=lambda:None)
        self.light_send,self.light_read=socket.socketpair(socket.AF_UNIX,socket.SOCK_DGRAM);self.light_read.settimeout(.05)
        prefix='XZ' if self.provider=='xiaozhi' else 'DB'
        env={k:v for k,v in os.environ.items() if not k.startswith(('XZ_','DB_','DC_','LX06_'))}
        env.update(DC_HOST_FIXTURE='1',DC_FIXTURE_DIRECTORY=str(self.root),DC_MIC_DIRECTORY=str(self.mic),DC_MIC_FORMAT='LXA2',DC_PCM_DEVICE='synthetic',
            DC_FAKE_ALSA_LEDGER=str(self.root/'alsa'),DC_MAX_SECONDS='30',LD_LIBRARY_PATH=str(self.library),
            LX06_CONTROL_ASR_SOCKET=self.asr.path,LX06_DIALOG_FD=str(self.light_send.fileno()),LX06_DIALOG_CHANNEL=os.readlink('/proc/self/fd/'+str(self.light_send.fileno())),
            DC_TRANSPORT_ADAPTER=str(self.adapter),DC_ENDPOINT=self.cloud.endpoint,DC_CREDENTIAL='fixture-key',DC_DEVICE_ID='fixture-device',DC_CLIENT_ID='fixture-client',DB_AUDIO_FORMAT='pcm_s16le',
            DC_EXPECT_QUESTION_1='请用两句话介绍北京故宫',DC_EXPECT_QUESTION_2='接着介绍颐和园')
        env[prefix+'_BARGE_ANY']=self.mode
        env['XZ_WAKE_TEXT' if prefix=='XZ' else 'DB_GREETING']='我在呢，请说。' if self.greeting else ''
        env[prefix+'_PLAYBACK']='1' if self.playback else '0'
        env.update(XZ_PING_INTERVAL_MS='100',XZ_PONG_TIMEOUT_MS='1000')
        env['XZ_INTERRUPT_TEXT' if prefix=='XZ' else 'DB_INTERRUPT']='等一下,等等,停一下,你说得不对'
        env['XZ_GOODBYE_TEXT' if prefix=='XZ' else 'DB_GOODBYE']='结束对话,拜拜了哦'
        self.process=subprocess.Popen([self.binary,'--audio',self.provider],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,pass_fds=[self.light_send.fileno()],start_new_session=True)
        self.log_thread=threading.Thread(target=self.read_log,daemon=True);self.log_thread.start()
        self.light_thread=threading.Thread(target=self.read_light,daemon=True);self.light_thread.start()
        wait(lambda:(self.mic/'account.a2').exists() or self.process.poll() is not None,label='audio core subscription')
        assert self.process.poll() is None,(self.events,self.errors)
        self.mic_thread=threading.Thread(target=self.send_mic,daemon=True);self.mic_thread.start()
        wait(lambda:self.asr.audio_packets>0 and (self.cloud.greetings==1 if self.greeting else self.cloud.uplink_bytes>0),label='actual ASR and adapter ready/greeting')
        return self

    def snapshot(self):
        result=super().snapshot();result.update(provider=self.provider,mode=self.mode,cloud_commands=self.cloud.events,cloud_commits=self.cloud.commits,cloud_cancels=self.cloud.cancels,uplink_bytes=self.cloud.uplink_bytes)
        return result

    def __exit__(self,*args):
        try:super().__exit__(*args)
        finally:self.cloud.close()

def exercise(binary,library,adapter,provider,mode):
    with AudioReactor(binary,library,adapter,provider,mode) as r:
        dialog=r.asr.emit('请用两句话',final=False);time.sleep(.1)
        assert r.cloud.commits==0,'partial submitted as a complete audio turn'
        r.final('请用两句话介绍北京故宫。',dialog)
        wait(lambda:r.cloud.commits==1,label='single complete audio commit')
        r.cloud.reply(1,long=True);r.event('first_pcm')
        dialog=wait(lambda:r.asr.dialog if r.asr.dialog and r.asr.dialog!=r.asr.last_sent else None,label='control utterance')
        start=time.monotonic_ns();r.asr.emit('等一下',final=False,dialog=dialog)
        drop=wait(lambda:[e for e in r.ledger() if e['op']=='DROP'],label='local audio drop')[0]
        delay=(drop['at_ns']-start)/1e6;assert 0<=delay<100,delay
        for text in ('等一下，等等','等一下，等等，你说得不对'):
            r.asr.emit(text,final=False,dialog=dialog);time.sleep(.02)
        r.final('等一下，等等，你说得不对，接着介绍颐和园。',dialog)
        wait(lambda:r.cloud.commits==2,label='same-session complete follow-up')
        assert r.cloud.cancels==1,r.cloud.events
        assert [e['value'] for e in r.events if e['event']=='expected_question']==[1,2],r.events
        assert len([e for e in r.ledger() if e['op']=='DROP'])==1
        if provider=='doubao':
            resumed=next(e for e in r.cloud.events if e['event']=='resumed')
            assert drop['at_ns']<resumed['at_ns'],'local drop waited for remote barrier'
        r.cloud.reply(2);r.event('first_pcm',count=2)
        r.asr.emit('结束对话，拜拜了哦，结束对话',final=False)
        wait(lambda:r.process.poll() is not None,label='repeated goodbye closes once')
        r.finish();wait(lambda:r.cloud.closed,label='protocol session closes')
        assert not r.cloud.errors,r.cloud.errors
        result=r.snapshot();result.update(control_to_drop_ms=delay,complete_followup=True,repeated_control_cancelled_once=True)
        return result

def greeting_case(binary,library,adapter,provider,pattern,mode='0'):
    muted=pattern=='muted';early=pattern=='early_question'
    with AudioReactor(binary,library,adapter,provider,mode,greeting=True,playback=not muted) as r:
        assert r.cloud.commits==0
        if early:
            dialog=r.asr.emit('请用两句话',final=False);time.sleep(.04)
            r.final('请用两句话介绍北京故宫。',dialog)
            r.event('complete_question');assert r.cloud.commits==0
            r.cloud.reply(1,long=True)
            wait(lambda:r.cloud.cancels==1,label='pre-BEGIN greeting cancellation')
            wait(lambda:r.cloud.commits==1,label='whole first question after greeting barrier')
            assert not r.ledger(),'cancelled greeting reached the local player'
        else:
            r.cloud.reply(1,long=muted)
            wait(lambda:any(e['event']=='reply_end_1' for e in r.cloud.events) and r.events[-1]['event']=='state' and r.events[-1]['value']==2,seconds=4,label='greeting consumed/drained')
            if muted:assert not r.ledger(),'PLAYBACK=0 opened or wrote an ALSA device'
            else:r.event('first_pcm')
            dialog=r.asr.emit('请用两句话',final=False);time.sleep(.04)
            assert r.cloud.commits==0
            r.final('请用两句话介绍北京故宫。',dialog)
            wait(lambda:r.cloud.commits==1,label='one complete first question after greeting')
        r.cloud.reply(2,long=muted)
        if muted:
            wait(lambda:any(e['event']=='reply_end_2' for e in r.cloud.events) and r.events[-1]['event']=='state' and r.events[-1]['value']==2,seconds=4,label='muted long reply fully consumed')
            assert not r.ledger() and not any(e['event']=='first_pcm' for e in r.events)
        else:r.event('first_pcm',count=1 if early else 2)
        assert [e['value'] for e in r.events if e['event']=='expected_question']==[1]
        r.asr.emit('结束对话，拜拜了哦',final=False)
        wait(lambda:r.process.poll() is not None,label='greeting fixture goodbye')
        r.finish();wait(lambda:r.cloud.closed,label='greeting fixture protocol close')
        assert not r.cloud.errors,r.cloud.errors
        result=r.snapshot();result.update(greeting_pattern=pattern,playback_disabled=muted,first_question_separate=True)
        return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output-name',required=True);parser.add_argument('--adapter-build',required=True)
    parser.add_argument('--core-build');parser.add_argument('--case-set',choices=('all','native-greetings'),default='all');args=parser.parse_args()
    assert os.geteuid()==0 and os.readlink('/proc/self/ns/net')!=os.readlink('/proc/1/ns/net')
    subprocess.run(['ip','link','set','lo','up'],check=True)
    output=ROOT/'build'/args.output_name;output.mkdir(exist_ok=False)
    report=dict(passed=False,actual_device=False,actual_audio=False,cloud_requests=0,ownership_bypass='host fixture only',cases=[])
    def save():(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    save();build=Path(args.core_build).resolve() if args.core_build else ROOT/'build'/(args.output_name+'-core')
    if not args.core_build:subprocess.run(['python3',ROOT/'runtime/dialogue-core/build.py','--fixture','--sanitize','--output',build],check=True,stdout=subprocess.PIPE)
    binary=build/'lx06-dialogued';adapter_build=Path(args.adapter_build).resolve()
    manifest=json.loads((build/'manifest.json').read_text());assert manifest['passed'] and manifest['fixture'] and manifest['sanitized']
    library=output/'fake';library.mkdir()
    subprocess.run(['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fPIC','-shared','-fsanitize=address,undefined','-fno-sanitize-recover=all',HERE/'fake-asound.c','-o',library/'libasound.so.2'],check=True)
    report.update(binary_sha256=sha(binary),manifest_sha256=sha(build/'manifest.json'),sources={str(p.relative_to(ROOT)):sha(p) for p in [Path(__file__),HERE/'test_reactor.py',HERE/'fake-asound.c']});save()
    with tempfile.TemporaryDirectory(prefix='dc-audio-arm-') as temporary:
        tree=Path(temporary)/'root'
        subprocess.run(['unsquashfs','-no-progress','-d',tree,ROOT/'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'],check=True,stdout=subprocess.PIPE)
        shutil.copyfile(shutil.which('qemu-arm-static'),tree/'qemu-arm-static');(tree/'qemu-arm-static').chmod(0o755)
        shutil.copyfile(adapter_build/'arm/lx06-transport-adapter',tree/'tmp/adapter');(tree/'tmp/adapter').chmod(0o755)
        shutil.copyfile(ROOT.parent/'scratch/libopus.so.0.6.1',tree/'usr/lib/libopus.so.0')
        for name in ('random','urandom'):
            path=tree/'dev'/name
            if path.exists() or path.is_symlink():path.unlink()
            path.write_bytes(os.urandom(65536));path.chmod(0o600)
        wrapper=output/'stock-arm-adapter';wrapper.write_text('#!/bin/sh\nexec chroot '+shlex.quote(str(tree))+' /qemu-arm-static /tmp/adapter "$@"\n');wrapper.chmod(0o700)
        report['adapter_binaries']={mode:sha(adapter_build/mode/'lx06-transport-adapter') for mode in ('host','sanitize','arm')};save()
        for target,adapter in [('host',adapter_build/'host/lx06-transport-adapter'),('sanitize',adapter_build/'sanitize/lx06-transport-adapter'),('stock_arm',wrapper)]:
            for provider in ('xiaozhi','doubao'):
                for pattern in (('0','1','greeting','early_question','muted') if args.case_set=='all' else ('greeting','early_question','muted')):
                    try:
                        result=exercise(binary,library,adapter,provider,pattern) if pattern in ('0','1') else greeting_case(binary,library,adapter,provider,pattern,'1' if args.case_set=='native-greetings' else '0')
                        report['cases'].append(dict(passed=True,target=target,**result));save()
                    except BaseException as error:
                        report['failure']=dict(target=target,provider=provider,pattern=pattern,type=type(error).__name__,message=str(error)[-5000:],fixture=Reactor.last_failure);save();raise
    report['source_unchanged']=all(sha(ROOT/p)==digest for p,digest in report['sources'].items())
    report['binary_sources_current']=all(sha(ROOT/p)==digest for p,digest in manifest['sources'].items())
    report['passed']=report['source_unchanged'] and report['binary_sources_current'];save()
    print(json.dumps(dict(passed=report['passed'],cases=len(report['cases']),report_sha256=sha(output/'report.json'))))
    assert report['passed']

if __name__=='__main__':main()

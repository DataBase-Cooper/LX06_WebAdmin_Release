"""Reproduce cloud TTS arriving while the preceding real player is draining."""
import base64
import hashlib
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time
from test_runtime_integration import recv_exact,recv_client_frame,send_server_text,send_server_binary,OpusCodec

ROOT=Path(__file__).resolve().parents[1]

def run_case(interrupt=False,startup_delay=0,interrupt_delay=.08,player_failure=False,failure_exit=9):
    with tempfile.TemporaryDirectory() as td:
        root=Path(td);fifo=root/'mic';os.mkfifo(fifo)
        player=root/'aplay';trace=root/'trace'
        player.write_text('#!/usr/bin/python3\nimport os,sys,time\nf=open(os.environ["PLAYER_TRACE"],"a",buffering=1)\nf.write("start\\n")\ntime.sleep(float(os.environ.get("PLAYER_STARTUP_DELAY","0")))\nd=sys.stdin.buffer.read()\ntime.sleep(.35)\nassert len(d)%960==0\nreal=sum(any(d[i:i+960]) for i in range(0,len(d),960))*960\nf.write("end %d\\n"%real)\n')
        if player_failure:
            player.write_text('#!/usr/bin/python3\nimport os\nwith open(os.environ["PLAYER_TRACE"],"a") as f:f.write("start\\n")\nraise SystemExit('+str(failure_exit)+')\n')
        player.chmod(0o755)
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1)
        port=listener.getsockname()[1];errors=[];stopped=threading.Event();observed=[]
        def microphone():
            fd=os.open(fifo,os.O_RDWR)
            try:
                while not stopped.wait(.03):os.write(fd,bytes(1920))
            finally:os.close(fd)
        def peer():
            try:
                conn,_=listener.accept()
                with conn:
                    conn.settimeout(3);request=b''
                    while not request.endswith(b'\r\n\r\n'):request+=recv_exact(conn,1)
                    key=next(x.split(':',1)[1].strip() for x in request.decode().split('\r\n') if x.lower().startswith('sec-websocket-key:'))
                    accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                    conn.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n').encode())
                    recv_client_frame(conn)
                    send_server_text(conn,{'type':'hello','version':1,'transport':'websocket','session_id':'overlap','audio_params':{'format':'opus','sample_rate':24000,'channels':1,'frame_duration':60}})
                    while True:
                        op,payload=recv_client_frame(conn)
                        if op==1 and json.loads(payload).get('state')=='start':break
                    packet=OpusCodec().encode_constant(24000,1440,2000)
                    def pump(seconds):
                        end=time.monotonic()+seconds
                        while time.monotonic()<end:
                            if select.select([conn],[],[],.01)[0]:
                                op,payload=recv_client_frame(conn)
                                if op==1:observed.append(json.loads(payload))
                                if op==8:raise RuntimeError('client exited during overlapping turns')
                    send_server_text(conn,{'type':'tts','state':'start'})
                    if player_failure:
                        send_server_binary(conn,packet)
                        # Leave the stream open: failure must close promptly,
                        # without waiting for cloud stop or spawning again.
                        closed=False
                        end=time.monotonic()+2
                        while time.monotonic()<end:
                            if select.select([conn],[],[],.02)[0]:
                                if recv_client_frame(conn)[0]==8:
                                    closed=True;break
                        assert closed,'client kept a session with a failed player'
                        return
                    for _ in range(60 if startup_delay else 1):
                        send_server_binary(conn,packet)
                        if startup_delay:pump(.06)
                    send_server_text(conn,{'type':'tts','state':'stop'})
                    pump(interrupt_delay)
                    if interrupt:
                        send_server_text(conn,{'type':'stt','text':'等一下'})
                        pump(.2)
                        assert any(x.get('type')=='abort' for x in observed),observed
                    send_server_text(conn,{'type':'tts','state':'start'})
                    send_server_binary(conn,packet)
                    pump(.6)  # Old playback ends while the new cloud turn is active.
                    send_server_text(conn,{'type':'tts','state':'stop'})
                    pump(3.1 if startup_delay else .6)
                    send_server_text(conn,{'type':'tts','state':'start'})
                    send_server_binary(conn,packet)
                    send_server_text(conn,{'type':'tts','state':'stop'})
                    pump(3.1 if startup_delay else .7)
                    send_server_text(conn,{'type':'goodbye'})
                    while recv_client_frame(conn)[0]!=8:pass
            except Exception as error:errors.append(repr(error))
            finally:listener.close()
        writer=threading.Thread(target=microphone,daemon=True);writer.start()
        server=threading.Thread(target=peer,daemon=True);server.start()
        env=dict(os.environ,XZ_WS_URL=f'ws://127.0.0.1:{port}/ws',XZ_DEVICE_ID='02:00:00:00:00:01',XZ_CLIENT_ID='00000000-0000-4000-8000-000000000001',XZ_MIC_FIFO=str(fifo),XZ_PLAYBACK='1',XZ_APLAY=str(player),PLAYER_TRACE=str(trace),PLAYER_STARTUP_DELAY=str(startup_delay),XZ_INTERRUPT_TEXT='等一下',XZ_TIMELINE_FILE='-',XZ_IDLE_TIMEOUT_MS='5000',XZ_PLAYBACK_TAIL_MUTE_MS='0')
        light_rx,light_tx=socket.socketpair(socket.AF_UNIX,socket.SOCK_DGRAM)
        light_rx.settimeout(.1);light_events=[];light_stop=threading.Event()
        env.update(LX06_DIALOG_FD=str(light_tx.fileno()),LX06_DIALOG_CHANNEL=os.readlink('/proc/self/fd/'+str(light_tx.fileno())))
        def collect_lights():
            while not light_stop.is_set():
                try:light_events.append((time.monotonic(),light_rx.recv(2)[0],trace.read_text().splitlines() if trace.exists() else []))
                except socket.timeout:pass
        light_thread=threading.Thread(target=collect_lights,daemon=True);light_thread.start()
        try:result=subprocess.run([str(ROOT/'build/xz_client')],env=env,capture_output=True,text=True,timeout=16,pass_fds=(light_tx.fileno(),))
        finally:
            stopped.set();light_stop.set();light_thread.join(1);light_rx.close();light_tx.close()
        server.join(1);writer.join(1)
        log=result.stdout+result.stderr
        assert not errors,(errors,log)
        if player_failure:
            assert result.returncode==15,log
            assert 'event=PLAYBACK_ERROR exit='+str(failure_exit or 255) in log,log
            assert trace.read_text().splitlines()==['start'],trace.read_text()
            print('PASS player failure closes session without respawn')
            return
        assert result.returncode==0,log
        assert 'STATE_ERROR' not in log and 'QUEUE_ERROR' not in log,log
        if interrupt:assert 'event=ABORT_SENT reason=interrupt_word' in log,log
        else:
            assert 'PLAYBACK_DEFERRED' in log and 'PLAYBACK_PENDING_FLUSH' in log,log
            expected=['start','end 172800','start','end 2880','start','end 2880'] if startup_delay else ['start','end 2880']*3
            assert trace.read_text().splitlines()==expected,trace.read_text()
            assert not any(x.get('type')=='listen' and x.get('state')=='start' for x in observed),observed
            assert sum(state==4 for _,state,_ in light_events)==3,light_events
            # Every listening transition after the initial greeting must have
            # matching local player completion, including overlapping cloud TTS.
            for _,state,played in light_events[1:]:
                if state==2:
                    assert played and sum(x=='start' for x in played)==sum(x.startswith('end ') for x in played),light_events
            assert light_events[-1][1]==0,light_events
        print('PASS delayed ALSA startup without lost PCM' if startup_delay else 'PASS drain overlap' if not interrupt else 'PASS interrupt after cloud stop' if interrupt_delay>1 else 'PASS interrupt during drain')

if __name__=='__main__':
    run_case();run_case(True);run_case(True,interrupt_delay=2);run_case(startup_delay=2.5);run_case(player_failure=True);run_case(player_failure=True,failure_exit=0)

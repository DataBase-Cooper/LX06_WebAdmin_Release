"""Real runtime with continuous PCM and a local WebSocket peer."""
import base64
import hashlib
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time
import resource
from test_runtime_integration import recv_exact, recv_client_frame, send_server_text, send_server_binary, OpusCodec

ROOT=Path(__file__).resolve().parents[1]

def run_case(kind):
    with tempfile.TemporaryDirectory() as folder:
        fifo=Path(folder)/'mic'; os.mkfifo(fifo)
        listener=socket.socket(); listener.bind(('127.0.0.1',0)); listener.listen(1)
        errors=[]; observed={'mic':0,'pong':0}; stopped=threading.Event()
        def microphone():
            fd=os.open(fifo,os.O_RDWR)
            try:
                while not stopped.wait(.03): os.write(fd,bytes(1920))
            finally: os.close(fd)
        def server():
            try:
                conn,_=listener.accept()
                with conn:
                    conn.settimeout(3)
                    request=b''
                    while not request.endswith(b'\r\n\r\n'): request+=recv_exact(conn,1)
                    key=next(x.split(':',1)[1].strip() for x in request.decode().split('\r\n') if x.lower().startswith('sec-websocket-key:'))
                    accept=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
                    conn.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n').encode())
                    recv_client_frame(conn)
                    send_server_text(conn,{'type':'hello','version':1,'transport':'websocket','session_id':'idle-test','audio_params':{'format':'opus','sample_rate':24000,'channels':1,'frame_duration':60}})
                    packet=OpusCodec().encode_constant(24000,1440,2000) if kind=='tts' else None
                    start=time.monotonic(); sent=0; next_frame=0
                    while time.monotonic()-start<3:
                        now=time.monotonic()-start
                        if select.select([conn],[],[],.01)[0]:
                            opcode,payload=recv_client_frame(conn)
                            if opcode==8: break
                            observed['mic']+=opcode==2; observed['pong']+=opcode==10
                        if now>=next_frame:
                            next_frame=now+.05
                            conn.sendall(b'\x89\x00')
                            send_server_text(conn,{'type':'heartbeat'})
                            if kind=='stt' and now<.65:
                                send_server_text(conn,{'type':'stt','text':'测试语音'})
                            if kind=='long-task' and not sent and now>=.85:
                                send_server_text(conn,{'type':'stt','text':'工具任务已完成'})
                                send_server_text(conn,{'type':'goodbye'})
                                sent=1
                            if kind=='tts' and now<.65:
                                if not sent: send_server_text(conn,{'type':'tts','state':'start'})
                                send_server_binary(conn,packet); sent+=1
            except (BrokenPipeError, ConnectionResetError):
                # The client may finish its idle close between select() and
                # our next keepalive write. Client exit/event/time assertions
                # below distinguish that expected close from a real failure.
                observed['closed_during_keepalive'] = True
            except Exception as error:
                errors.append(repr(error))
            finally: listener.close()
        writer=threading.Thread(target=microphone,daemon=True); writer.start()
        peer=threading.Thread(target=server,daemon=True); peer.start()
        env={**os.environ,'XZ_WS_URL':f'ws://127.0.0.1:{listener.getsockname()[1]}/ws','XZ_DEVICE_ID':'02:00:00:00:00:01',
             'XZ_CLIENT_ID':'00000000-0000-4000-8000-000000000001','XZ_ACCESS_TOKEN':'test-only-token',
             'XZ_MIC_FIFO':str(fifo),'XZ_TIMELINE_FILE':'-','XZ_PLAYBACK':'0','XZ_IDLE_TIMEOUT_MS':'300'}
        if kind=='log':
            env.update(XZ_TIMELINE_FILE=str(Path(folder)/'trace'),XZ_TALK_LOG='0',XZ_IDLE_TIMEOUT_MS='1700')
        if kind=='long-task':
            env.update(XZ_KEEPALIVE_MINUTES='1')
        start=time.monotonic()
        try:
            result=subprocess.run([str(ROOT/'build/xz_client')],env=env,capture_output=True,text=True,timeout=4,
                preexec_fn=(lambda:resource.setrlimit(resource.RLIMIT_FSIZE,(2048,2048))) if kind=='log' else None)
        finally: stopped.set()
        elapsed=time.monotonic()-start; writer.join(1); peer.join(1)
        combined=result.stdout+result.stderr
        if kind=='log':
            trace=Path(folder)/'trace'
            assert trace.stat().st_size<=2048
            combined+=trace.read_text()
            assert observed['mic']>40,observed
        assert result.returncode==0,combined
        if kind=='long-task':
            assert 'event=STT_RECOGNIZED' in combined and 'event=IDLE_TIMEOUT' not in combined,combined
        else:
            assert 'event=IDLE_TIMEOUT' in combined,combined
        assert not errors,errors
        assert observed['mic']>2 and observed['pong']>2,observed
        assert (.25 if kind=='quiet' else .8)<elapsed<(2.4 if kind=='log' else 1.8),(kind,elapsed)
        print('PASS idle',kind,round(elapsed,3),observed)

if __name__=='__main__':
    for case in ['quiet','stt','tts','log','long-task']: run_case(case)

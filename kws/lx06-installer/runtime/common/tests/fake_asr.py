"""Local seqpacket fixture implementing the speaker SDK bridge contract."""
import json
import socket
import tempfile
import threading
import time
from pathlib import Path

class SpeakerASR:
    def __enter__(self):
        self.tmp=tempfile.TemporaryDirectory();self.path=str(Path(self.tmp.name)/'asr.sock')
        self.server=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);self.server.bind(self.path);self.server.listen(1)
        self.conn=None;self.dialog='';self.last_sent='';self.audio_packets=0;self.errors=[];self.stopping=False
        self.end_packets=0;self.cancel_packets=0;self.start_packets=[];self.audio_by_dialog={}
        self.read_gate=threading.Event();self.read_gate.set()
        self.lock=threading.Lock();self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start();return self
    def run(self):
        try:
            self.conn,_=self.server.accept();self.send({'type':'hello'})
            while True:
                self.read_gate.wait()
                data=self.conn.recv(8192)
                if not data:return
                if data[:1] in [b'V',b'S']:
                    epoch=int.from_bytes(data[1:],'little');self.dialog=f'fixture-{epoch}'
                    self.start_packets.append((data[:1],self.dialog));self.audio_by_dialog[self.dialog]=[]
                    self.send({'type':'ready','dialog':self.dialog,'epoch':epoch})
                elif data[:1]==b'A':
                    assert int.from_bytes(data[1:3],'big')==len(data)-3
                    self.audio_packets+=1
                    self.audio_by_dialog.setdefault(self.dialog,[]).append(data[3:])
                elif data==b'C':self.cancel_packets+=1;self.dialog=''
                elif data==b'P':self.send({'type':'pong'})
                elif data==b'E':self.end_packets+=1
                else:raise AssertionError('unexpected bridge packet')
        except (OSError,ConnectionError):
            if not self.stopping:self.errors.append('unexpected socket failure')
        except BaseException as exc:self.errors.append(exc)
    def send(self,obj):
        with self.lock:self.conn.send(json.dumps(obj,ensure_ascii=False).encode())
    def emit(self,text,final=True,dialog=None):
        if dialog is None:
            end=time.monotonic()+3
            while not self.dialog or self.dialog==self.last_sent:
                if time.monotonic()>end:raise AssertionError('missing fresh auxiliary turn')
                time.sleep(.005)
            dialog=self.dialog
        self.send({'header':{'namespace':'SpeechRecognizer','name':'RecognizeResult','dialog_id':dialog},
                   'payload':{'is_final':final,'results':[{'text':text}]}})
        if final:self.last_sent=dialog
        return dialog
    def __exit__(self,*args):
        self.stopping=True
        self.read_gate.set()
        if self.conn:self.conn.close()
        self.server.close();self.thread.join(2);self.tmp.cleanup()
        if not args[0] and self.errors:raise AssertionError(self.errors)

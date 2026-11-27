"""Native behavioral tests. Run under Linux; no device or cloud required."""
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parent

class PassiveAudioTest(unittest.TestCase):
    def test_broadcast_preserves_official_output_and_slow_consumer_cannot_block(self):
        with tempfile.TemporaryDirectory(prefix='lx06-tap-') as tmp:
            tmp=Path(tmp)
            (tmp/'stock.c').write_text('''
            #include <errno.h>
            #include <string.h>
            int xaudio_wrapper_dnn(void *ctx,const void *in,int n,void *out,int *len) {
                memcpy(out,in,n); *len=n; errno=EAGAIN; return ctx ? 7 : 0;
            }
            ''')
            (tmp/'driver.c').write_text('''
            #include <stdint.h>
            #include <errno.h>
            #include <string.h>
            #include <unistd.h>
            extern int xaudio_wrapper_dnn(void *,const void *,int,void *,int *);
            int main(void) {
                uint16_t in[320],out[320]; int n;
                for(int i=0;i<500;i++) {
                    for(int j=0;j<320;j++) in[j]=j<160 ? i : 0xbeef;
                    int r=xaudio_wrapper_dnn(0,in,640,out,&n);
                    if(r || n!=640 || errno!=EAGAIN || memcmp(in,out,640)) return 2;
                    usleep(1000);
                }
                if(xaudio_wrapper_dnn((void*)1,in,640,out,&n)!=7 || errno!=EAGAIN) return 3;
                return 0;
            }
            ''')
            def cc(*args): subprocess.run(['gcc',*map(str,args)],check=True,capture_output=True)
            cc('-shared','-fPIC',tmp/'stock.c','-o',tmp/'stock.so')
            cc('-shared','-fPIC','-O2','-DTAPDIR="'+str(tmp)+'"', ROOT/'processed-mic-tap.c',ROOT/'native-wake-bridge.c','-ldl','-o',tmp/'tap.so')
            cc(tmp/'driver.c',tmp/'stock.so','-o',tmp/'driver')
            sockets=[]
            try:
                for name in ['a','b','slow']:
                    sock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
                    sock.bind(str(tmp/(name+'.sock'))); sockets.append(sock)
                start=time.monotonic()
                proc=subprocess.Popen([str(tmp/'driver')],env={**os.environ,'LD_PRELOAD':str(tmp/'tap.so')})
                streams=[[],[]]
                while proc.poll() is None or select.select(sockets[:2],[],[],0)[0]:
                    ready,_,_=select.select(sockets[:2],[],[],0.1)
                    for sock in ready:
                        packet=sock.recv(4096); magic,seq,rate,size=struct.unpack('<IIII',packet[:16])
                        self.assertEqual((magic,rate,size),(0x3141584c,16000,320))
                        self.assertEqual(packet[16:],struct.pack('<H',seq)*160)
                        streams[sockets.index(sock)].append(seq)
                    self.assertLess(time.monotonic()-start,4,'producer stalled on slow subscriber')
                self.assertEqual(proc.wait(),0)
                self.assertEqual(streams[0],list(range(500)))
                self.assertEqual(streams[1],streams[0])
            finally:
                for sock in sockets: sock.close()

if __name__=='__main__': unittest.main()

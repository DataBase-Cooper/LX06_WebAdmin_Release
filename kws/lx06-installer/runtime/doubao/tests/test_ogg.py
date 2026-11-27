import ctypes
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
class OggTest(unittest.TestCase):
    def test_spool_under_keeper_limits_wrap_reclaim_and_cancel(self):
        harness=r'''
#include "ogg_audio.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv){
    assert(argc==2);
    struct rlimit files={32,32},size={262144,262144};
    assert(!setrlimit(RLIMIT_NOFILE,&files));assert(!setrlimit(RLIMIT_FSIZE,&size));
    /* Mirror standard streams, inherited resident lock, TLS, mic, player. */
    int baseline[9];for(int i=0;i<9;i++){baseline[i]=open("/dev/null",O_RDONLY);assert(baseline[i]>=0);}
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
    unsigned char *data=malloc(n);assert(data);assert(fread(data,1,n,f)==(size_t)n);fclose(f);
    db_ogg *o=calloc(1,sizeof(*o));int16_t pcm[2880];unsigned long frames=0;
    /* Three rounds cumulatively cross the 4 MiB ring while each burst is
       >1 MiB. Decode real chained Opus streams and reclaim every shard. */
    size_t count=(2*1024*1024)/(size_t)n;assert(count>0);
    for(int turn=0;turn<3;turn++){
        for(size_t i=0;i<count;i++)assert(!db_ogg_feed(o,data,n));
        assert(o->used>1024*1024);assert(db_ogg_storage(o)>0);
        for(size_t i=0;i<DB_OGG_FILES;i++)if(o->spool[i]){
            struct stat st;assert(!fstat(o->spool[i]-1,&st));assert(st.st_size<=262144);
            assert(fcntl(o->spool[i]-1,F_GETFD)&FD_CLOEXEC);
        }
        int samples;while((samples=db_ogg_next(o,pcm,2880))>0)frames+=(unsigned)samples;
        assert(samples==0);assert(db_ogg_empty(o));assert(db_ogg_storage(o)==0);
        for(size_t i=0;i<DB_OGG_FILES;i++)assert(!o->spool[i]);
    }
    /* Exercise the full descriptor/space ceiling and reject one extra byte.
       Reset is also used by cancel and session teardown. */
    unsigned char *full=calloc(1,DB_OGG_CAP);assert(full);
    assert(!db_ogg_feed(o,full,DB_OGG_CAP));assert(db_ogg_feed(o,full,1)<0);
    assert(db_ogg_storage(o)<=DB_OGG_CAP);
    db_ogg_reset(o);assert(!db_ogg_storage(o));
    for(size_t i=0;i<DB_OGG_FILES;i++)assert(!o->spool[i]);
    assert(!db_ogg_feed(o,data,n));db_ogg_reset(o);
    printf("decoded_frames=%lu burst_bytes=%zu cap=%u\n",frames,count*(size_t)n,DB_OGG_CAP);
    free(full);free(o);free(data);return 0;
}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'h.c').write_text(harness)
            subprocess.run(['gcc','-O2','-I'+str(ROOT/'src'),str(p/'h.c'),str(ROOT/'src/ogg_audio.c'),'-lopus','-o',str(p/'test')],check=True)
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','anoisesrc=sample_rate=48000:duration=1','-ac','1','-c:a','libopus','-b:a','96k',str(p/'sample.ogg')],check=True)
            result=subprocess.run([str(p/'test'),str(p/'sample.ogg')],capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_real_opus_fragmentation_crc_duration_and_chained_streams(self):
        harness=r'''
#include "ogg_audio.h"
#include <stdlib.h>
void *make(void){return calloc(1,sizeof(db_ogg));}
void drop(db_ogg *p){db_ogg_reset(p);free(p);}
'''
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'h.c').write_text(harness)
            # A smaller ring forces real wraparound during the chained streams.
            subprocess.run(['gcc','-shared','-fPIC','-O2','-DDB_OGG_CAP=32768','-I'+str(ROOT/'src'),str(p/'h.c'),str(ROOT/'src/ogg_audio.c'),'-lopus','-o',str(p/'ogg.so')],check=True)
            subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=2.13','-ac','1','-c:a','libopus',str(p/'sample.ogg')],check=True)
            subprocess.run(['ffmpeg','-v','error','-c:a','libopus','-i',str(p/'sample.ogg'),'-ar','24000','-f','s16le',str(p/'expected.pcm')],check=True)
            lib=ctypes.CDLL(str(p/'ogg.so'));lib.make.restype=ctypes.c_void_p
            lib.db_ogg_feed.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t]
            lib.db_ogg_next.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
            lib.db_ogg_empty.argtypes=[ctypes.c_void_p];lib.drop.argtypes=[ctypes.c_void_p]
            data=(p/'sample.ogg').read_bytes();expected=(p/'expected.pcm').stat().st_size
            for chunk in (1,37,4096,len(data)):
                with self.subTest(chunk=chunk):
                    decoder=lib.make();pcm=(ctypes.c_int16*2880)();out=bytearray()
                    try:
                        for offset in range(0,len(data)*2,chunk):
                            piece=(data*2)[offset:offset+chunk]
                            self.assertEqual(lib.db_ogg_feed(decoder,piece,len(piece)),0)
                            while True:
                                n=lib.db_ogg_next(decoder,pcm,2880);self.assertGreaterEqual(n,0)
                                if not n:break
                                out.extend(bytes(pcm)[:n*2])
                        self.assertEqual(len(out),expected*2)
                        self.assertTrue(lib.db_ogg_empty(decoder))
                        samples=(ctypes.c_int16*(len(out)//2)).from_buffer_copy(out)
                        self.assertGreater(max(samples),3000)
                    finally:lib.drop(decoder)
            decoder=lib.make()
            try:
                bad=bytearray(data);bad[23]^=1
                self.assertEqual(lib.db_ogg_feed(decoder,bytes(bad),len(bad)),0)
                self.assertEqual(lib.db_ogg_next(decoder,(ctypes.c_int16*2880)(),2880),-1)
                self.assertEqual(lib.db_ogg_feed(decoder,b'x',32768+1),-1)
            finally:lib.drop(decoder)

if __name__=='__main__':unittest.main()

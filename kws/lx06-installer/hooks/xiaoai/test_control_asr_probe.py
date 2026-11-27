"""SDK bridge isolation contract; mock SDK, no device or cloud calls."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

class BridgeTest(unittest.TestCase):
    def test_official_passthrough_and_owned_dialog_isolation(self):
        harness=r'''
#include <dlfcn.h>
#include <cassert>
#include <vector>
static void *fake_dlsym(void*,const char*);
#define dlsym fake_dlsym
#include "control-asr-probe.cc"
#undef dlsym
static int original_calls, posted, bytes_sent;
static std::string original_input,built_input;
static opaque test_engine;
static void *base[26], *derived[24];
struct Mock {void **vptr=derived;};
static opaque make_engine(opaque&,opaque&,int){return test_engine;}
static bool build_event(const std::string &s,opaque &out){built_input=s;out=std::make_shared<int>(1);return true;}
static bool original_instruction(const std::string &s,opaque &out){++original_calls;original_input=s;out=test_engine;return true;}
static bool post(void*,opaque&){++posted;return true;}
static bool raw(void*,const unsigned char*,unsigned n){bytes_sent+=n;return true;}
static void *fake_dlsym(void*,const char *name){
 if(!strcmp(name,CREATE))return (void*)make_engine;
 if(!strcmp(name,BUILD_EVENT))return (void*)build_event;
 if(!strcmp(name,BUILD_INSTR))return (void*)original_instruction;
 if(!strcmp(name,POST_EVENT))return (void*)post;
 if(!strcmp(name,POST_RAW))return (void*)raw;
 if(!strcmp(name,BASE_VTABLE))return base;
 return nullptr;
}
int main(){
 test_engine=std::make_shared<Mock>();opaque config,info,out;
 base[3]=derived[1]=(void*)post;base[5]=derived[3]=(void*)raw;
 unsetenv("LX06_CONTROL_ASR_PROBE");
 assert(capture_create(config,info,1)==test_engine && current_engine.expired());
 std::string official="{\"header\":{\"dialog_id\":\"official-1\"},\"payload\":{\"text\":\"等一下\"}}";
 assert(capture_instruction(official,out));assert(original_input==official && out==test_engine);
 setenv("LX06_CONTROL_ASR_PROBE","1",1);launched=true;
 assert(capture_create(config,info,1)==test_engine);assert(current_engine.lock()==test_engine);
 assert(resolve_abi() && event_slot==1 && raw_slot==3);
 strcpy(prefix,"lx06-ctrl-testnonce-");
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK,0,pair));client=pair[0];
 assert(begin());assert(posted==1);
 char buf[2048];assert(recv(pair[1],buf,sizeof(buf),0)>0);
 assert(built_input.find("RequestControl")!=std::string::npos);
 assert(built_input.find("\"disabled\":[\"NLP\",\"TTS\"]")!=std::string::npos);
 assert(built_input.find("GlobalConfig")==std::string::npos);
 unsigned char pkt[]={0,1,2};assert(audio(pkt,sizeof(pkt)) && bytes_sent==3);
 int before=original_calls;
 assert(capture_instruction(official,out) && original_calls==before+1 && original_input==official);
 std::string unrelated="{\"header\":{\"dialog_id\":\"official-2\"},\"payload\":{\"text\":\"lx06-ctrl-testnonce-\"}}";
 assert(capture_instruction(unrelated,out) && original_input==unrelated);
 std::string final="{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"RecognizeResult\",\"dialog_id\":\""+std::string(active)+"\"},\"payload\":{\"is_final\":true,\"results\":[{\"text\":\"等一下\"}]}}";
 before=original_calls;assert(!capture_instruction(final,out));assert(!out && original_calls==before);
 int n=recv(pair[1],buf,sizeof(buf),0);assert(n==(int)final.size() && !memcmp(buf,final.data(),n));
 assert(dispatch_instruction(final,out,true));assert(out==test_engine);
 assert(original_input.find("等一下")==std::string::npos && original_input.find("\"is_final\":true")!=std::string::npos);
 n=recv(pair[1],buf,sizeof(buf),0);assert(n==(int)final.size());
 std::string stop="{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"StopCapture\",\"dialog_id\":\""+std::string(active)+"\"},\"payload\":{}}";
 assert(dispatch_instruction(stop,out,true));assert(original_input==stop);
 assert(recv(pair[1],buf,sizeof(buf),0)>0);
 cancel_active();assert(!*active && posted==1); // Completed turn needs no late Cancel.
 assert(!capture_instruction(final,out));assert(recv(pair[1],buf,sizeof(buf),0)<0);
 assert(capture_instruction(official,out) && original_input==official);
 assert(begin());assert(posted==2);cancel_active();assert(posted==3 && built_input.find("Cancel")!=std::string::npos);
 // Disabled bridge is an exact pass-through, even for prior probe dialogs.
 unsetenv("LX06_CONTROL_ASR_PROBE");assert(capture_instruction(final,out));assert(original_input==final);
 client=-1;close(pair[0]);close(pair[1]);
}
'''
        with tempfile.TemporaryDirectory() as d:
            folder=Path(d);src=folder/'test.cc';src.write_text(harness)
            subprocess.run(['gcc','-fPIC','-c',str(ROOT/'runtime/doubao/src/json.c'),'-o',str(folder/'json.o')],check=True)
            subprocess.run(['g++','-std=c++14','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'hooks/xiaoai'),str(src),str(folder/'json.o'),'-lpthread','-ldl','-o',str(folder/'test')],check=True)
            subprocess.run([str(folder/'test')],check=True,timeout=10)

if __name__=='__main__':unittest.main()

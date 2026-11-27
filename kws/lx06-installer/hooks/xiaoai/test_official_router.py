"""Linux process/wire-format regressions for the shared official command router."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class RouterTest(unittest.TestCase):
    def test_registry_final_only_dedup_rotation_and_restart(self):
        with tempfile.TemporaryDirectory(prefix='lx06-router-') as folder:
            p = Path(folder)
            (p/'apps').mkdir()
            source = r'''
#define OR_TEST 1
#include "official-router.c"
#include <assert.h>
static void write_file(const char *p,const char *s) {
 FILE *f=fopen(p,"w");assert(f);fputs(s,f);fclose(f);
}
static void event(const char *id,const char *text,int final) {
 char s[2048];snprintf(s,sizeof(s),"{\"header\":{\"namespace\":\"SpeechRecognizer\",\"name\":\"RecognizeResult\",\"dialog_id\":\"%s\"},\"payload\":{\"is_final\":%s,\"results\":[{\"text\":\"%s\"}]}}",id,final?"true":"false",text);
 instruction(s,strlen(s));
}
int main(void) {
 char pid[96];snprintf(pid,sizeof(pid),"%u %llu",(unsigned)getpid(),(unsigned long long)lx06_process_ticks(getpid()));write_file(OR_GATE_PID,pid);
 write_file(OR_APPS "/xz.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# 小爱通道唤醒词=小智出来,召唤小智\n");
 registry();assert(match("召唤小智"));assert(!match("豆包出来"));
 mkdir(LX06_SCRIPT_POLICY_DIR,0755);
 write_file(LX06_SCRIPT_POLICY_DIR "/autostart","off\n");
 event("disabled","召唤小智",1);assert(!dialog_count);
 write_file(LX06_SCRIPT_POLICY_DIR "/autostart","on\n");
 event("a","召唤小智",0);assert(!dialog_count);
 event("a","不要召唤小智",1);assert(!dialog_count);
 event("b","召唤 小智！",1);assert(dialog_count==1&&dialogs[0].canceled);
 event("b","召唤小智",1);assert(dialog_count==1);
 event("c","\\u53ec\\u5524\\u5c0f\\u667a",1);assert(dialog_count==2);
 write_file(OR_APPS "/db.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# 小爱通道唤醒词=豆包出来，召唤豆包\n");
 registry();event("d","召唤豆包",1);assert(dialog_count==3);
 write_file(OR_APPS "/door.sh","#!/bin/sh\n# 小爱通道指令=打开玻璃门\n");
 registry();assert(match("打开玻璃门")&&!match("打开玻璃门")->chat);
 event("lx06-ctrl-testnonce-1","打开玻璃门",1);assert(dialog_count==3);
 event("lx06-ctrl-testnonce-2","召唤小智",1);assert(dialog_count==3);
 event("e","打开玻璃门",1);assert(dialog_count==4);
 event("negated","不要召唤豆包",1);event("negated","召唤豆包",1);assert(dialog_count==4);
 event("old-partial","召唤豆包",0);write_file(OR_NATIVE,"official 2 1\n");
 event("old-partial","召唤豆包",1);assert(dialog_count==4);
 write_file(OR_APPS "/conflict.sh","#!/bin/sh\n# 小爱通道指令=打开玻璃门\n");
 registry();assert(!match("打开玻璃门"));event("f","打开玻璃门",1);assert(dialog_count==4);
 mkdir(LX06_SETTINGS_DIR,0700);
 unlink(OR_APPS "/conflict.sh");
 write_file(OR_APPS "/door.sh","#!/bin/sh\n# LX06-SETTINGS: official_command\n# 小爱通道指令=打开玻璃门,打开小区门\n");
 registry();assert(match("打开玻璃门")&&!match("打开玻璃门")->chat);assert(match("打开小区门")&&!match("打开小区门")->chat);
 write_file(LX06_SETTINGS_DIR "/door.sh.json","{\"official_command\":\"开大门\"}");chmod(LX06_SETTINGS_DIR "/door.sh.json",0600);
 registry();assert(!match("打开小区门"));assert(match("开大门")&&!match("开大门")->chat);
 write_file(LX06_SETTINGS_DIR "/door.sh.json","{\"official_command\":\"\"}");
 registry();assert(!match("打开小区门")&&!match("打开玻璃门")&&!match("开大门"));
 write_file(LX06_SETTINGS_DIR "/door.sh.json","invalid");
 registry();assert(!match("打开玻璃门")&&match("召唤小智"));
 write_file(OR_APPS "/xz.sh","#!/bin/sh\n# LX06-LOCALWAKEUP: on\n# LX06-SETTINGS: official_wake\n# LX06-SESSION-MANAGED: on\n# 小爱通道唤醒词=召唤小智\n");
 registry();assert(match("召唤小智")->managed);assert(!match("豆包出来")->managed);
 write_file(LX06_SETTINGS_DIR "/xz.sh.json","{\"official_wake\":\"你好小智,小智助手\"}");chmod(LX06_SETTINGS_DIR "/xz.sh.json",0600);
 registry();assert(!match("召唤小智"));assert(match("你好小智")&&match("小智助手"));assert(match("豆包出来"));
 write_file(LX06_SETTINGS_DIR "/xz.sh.json","{\"official_wake\":\"豆包出来\"}");
 registry();assert(!match("豆包出来"));assert(match("召唤豆包"));
 write_file(LX06_SETTINGS_DIR "/xz.sh.json","{\"official_wake\":\"\"}");
 registry();assert(!match("召唤小智"));assert(match("豆包出来"));
 write_file(LX06_SETTINGS_DIR "/xz.sh.json","invalid");registry();assert(!match("召唤小智"));
 write_file(LX06_SETTINGS_DIR "/xz.sh.json","{\"official_wake\":\"你好小智\"}");chmod(LX06_SETTINGS_DIR "/xz.sh.json",0644);
 registry();assert(!match("你好小智")&&!match("召唤小智"));
 unlink(LX06_SETTINGS_DIR "/xz.sh.json");registry();assert(match("召唤小智"));
 unlink(OR_APPS "/xz.sh");registry();event("g","召唤小智",1);assert(dialog_count==4);
 unlink(OR_GATE_PID);event("h","豆包出来",1);assert(dialog_count==4);
 assert(!find_dialog("brand-new-official",0));assert(find_dialog("b",0)->canceled);
 memset(dialogs,0,sizeof(dialogs));dialog_count=0;load_canceled();assert(dialog_count==4&&find_dialog("b",0)->canceled);
 write_file(OR_GATE_PID,pid);write_file(OR_LOG,"old log ignored\n");readlog();assert(started);
 write_file(OR_LOG,"{}\n");readlog();assert(used==0);
 char normal[128];assert(normalize("召唤小智。",normal)&&!strcmp(normal,"召唤小智"));
 assert(normalize("不要召唤小智",normal)&&!match(normal));
 assert(!name_ok("../evil.sh"));assert(!name_ok("$(id).sh"));
 puts("registry / exact match / partial correction / duplicate / independent install / conflict / stale gate / canceled dialog persistence passed");
}
'''
            (p/'test.c').write_text(source)
            flags = [f'-D{k}="{p/v}"' for k,v in {
                'OR_APPS':'apps','OR_LOG':'instruction.log','OR_NATIVE':'native','LX06_SCRIPT_POLICY_DIR':'policy',
                'OR_GATE_PID':'gate.pid','OR_CANCELED':'canceled','LX06_SETTINGS_DIR':'settings'}.items()]
            subprocess.run(['gcc','-O1','-g','-fsanitize=address,undefined','-I',str(ROOT),*flags,
                            str(p/'test.c'),str(ROOT/'../../runtime/doubao/src/json.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_playback_blob_fields_and_invalid_lengths(self):
        with tempfile.TemporaryDirectory(prefix='lx06-gate-') as folder:
            p=Path(folder)
            (p/'test.c').write_text(r'''
#include "official-play-gate.c"
#include <assert.h>
static size_t add(unsigned char *b,const char *name,const char *value) {
 size_t n=strlen(name),h=(n+3+3)&~(size_t)3,len=4+h+strlen(value)+1;
 uint32_t raw=htonl(0x83000000|len);memcpy(b,&raw,4);
 uint16_t nl=htons(n);memcpy(b+4,&nl,2);memcpy(b+6,name,n+1);strcpy((char*)b+4+h,value);return (len+3)&~(size_t)3;
}
int main(void) {
 unsigned char b[1024]={0};size_t n=4;n+=add(b+n,"url","http://localhost:8888//tmp/tts/tts_test.mp3");n+=add(b+n,"dialog_id","abcd");
 uint32_t raw=htonl(n);memcpy(b,&raw,4);char out[512];
 assert(field(b,"dialog_id",out,sizeof(out))&&!strcmp(out,"abcd"));
 assert(field(b,"url",out,sizeof(out))&&!strcmp(out,"http://localhost:8888//tmp/tts/tts_test.mp3"));
 assert(!field(b,"missing",out,sizeof(out)));assert(!field(b,"url",out,2));
 raw=htonl(3);memcpy(b,&raw,4);assert(!field(b,"url",out,sizeof(out)));
 puts("stock blobmsg wire format / bounds passed");
}
''')
            subprocess.run(['gcc','-O1','-g','-fsanitize=address,undefined','-I',str(ROOT),str(p/'test.c'),'-ldl','-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)


if __name__=='__main__': unittest.main()

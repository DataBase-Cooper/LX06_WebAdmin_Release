/* Opt-in feasibility bridge. Not included in firmware or enabled by default.
 * Reuse this speaker's existing SDK Engine and authentication. Never create a
 * second cloud connection. The IPC accepts only bounded audio/control frames.
 * Ordinary official traffic is passed through byte-for-byte. Bridge-owned
 * text goes only to IPC; sanitized ASR lifecycle still reaches SDK timeout
 * bookkeeping, while auxiliary NLP/TTS instructions are suppressed.
 */
#define _GNU_SOURCE 1
#ifdef __arm__
// GCC 12 headers optimize shared_ptr via glibc 2.32's flag. On glibc 2.25
// use a private always-multithreaded flag, never interpose a libc symbol.
#define __libc_single_threaded lx06_probe_single_threaded
#endif
#include <memory>
#ifdef __arm__
#undef __libc_single_threaded
extern "C" { __attribute__((visibility("hidden"))) char lx06_probe_single_threaded=0; }
#endif
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#ifdef __arm__
extern "C" int __lxstat64(int,const char*,struct stat64*);
#define ASR_STAT struct stat64
#define ASR_LSTAT(path,st) __lxstat64(3,path,st)
#else
#define ASR_STAT struct stat
#define ASR_LSTAT(path,st) lstat(path,st)
#endif
extern "C" {
#include "../../runtime/doubao/src/json.h"
}

#define CREATE "_ZN4aivs6Engine6createERSt10shared_ptrINS_10AivsConfigEERS1_INS_8Settings10ClientInfoEEi"
#define BUILD_EVENT "_ZN4aivs5Event5buildERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERSt10shared_ptrIS0_E"
#define BUILD_INSTR "_ZN4aivs11Instruction5buildERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERSt10shared_ptrIS0_E"
#define POST_EVENT "_ZN4aivs6Engine9postEventERSt10shared_ptrINS_5EventEE"
#define POST_RAW "_ZN4aivs6Engine11postRawDataEPKhj"
#define BASE_VTABLE "_ZTVN4aivs6EngineE"
#define SOCK_PATH "/tmp/lx06-control-asr-probe.sock"

using opaque = std::shared_ptr<void>;
using creator = opaque (*)(opaque&,opaque&,int);
using builder = bool (*)(const std::string&,opaque&);
using poster = bool (*)(void*,opaque&);
using rawposter = bool (*)(void*,const unsigned char*,unsigned);
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static std::weak_ptr<void> current_engine;
static int client=-1, event_slot=-1, raw_slot=-1;
static unsigned serial;
static char active[96], prefix[64];
static bool launched;
static bool completed;
static unsigned char lease[96];
static bool lease_bound;
static bool same_lease() {
 if(!lease_bound)return true; /* Explicit idle-device probe. */
 unsigned char record[sizeof(lease)];
 int f=open("/tmp/lx06-session.owner",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
 if(f<0)return false;
 ssize_t n=read(f,record,sizeof(record));close(f);
 return n==sizeof(record) && !memcmp(record,lease,sizeof(record));
}

static bool enabled() {
 const char *v=getenv("LX06_CONTROL_ASR_PROBE");
 return v && !strcmp(v,"1");
}
static bool owned(const char *id) {
 return *prefix && !strncmp(id,prefix,strlen(prefix));
}
static bool resolve_abi() {
 void **base=(void**)dlsym(RTLD_NEXT,BASE_VTABLE);
 void *ev=dlsym(RTLD_NEXT,POST_EVENT), *raw=dlsym(RTLD_NEXT,POST_RAW);
 if(!base||!ev||!raw)return false;
 // Verified exported Engine base vtable: 2 metadata + 24 method slots.
 // Resolve methods by their symbols, never by a firmware code address.
 for(int i=0;i<24;i++) {
  if(base[i+2]==ev)event_slot=i;
  if(base[i+2]==raw)raw_slot=i;
 }
 return event_slot>=0 && raw_slot>=0;
}
static bool event(const char *name,const char *ns,const char *id,const char *payload,const char *context="") {
 opaque engine;
 pthread_mutex_lock(&lock);engine=current_engine.lock();pthread_mutex_unlock(&lock);
 auto build=(builder)dlsym(RTLD_NEXT,BUILD_EVENT);
 if(!engine||!build||event_slot<0)return false;
 std::string s="{\"header\":{\"id\":\""+std::string(id)+"\",\"dialog_id\":\""+id+
  "\",\"namespace\":\""+ns+"\",\"name\":\""+name+"\"},\"payload\":"+payload+context+"}";
 opaque ev;if(!build(s,ev)||!ev)return false;
 void **table=*(void***)engine.get();
 return ((poster)table[event_slot])(engine.get(),ev);
}
static bool audio(const unsigned char *data,unsigned n) {
 opaque engine;
 pthread_mutex_lock(&lock);engine=current_engine.lock();pthread_mutex_unlock(&lock);
 if(!engine||raw_slot<0)return false;
 return ((rawposter)(*(void***)engine.get())[raw_slot])(engine.get(),data,n);
}
static void reply(const char *s) {
 pthread_mutex_lock(&lock);
 if(client>=0)(void)send(client,s,strlen(s),MSG_DONTWAIT|MSG_NOSIGNAL);
 pthread_mutex_unlock(&lock);
}
static void cancel_active() {
 char id[96];pthread_mutex_lock(&lock);strcpy(id,active);bool done=completed;active[0]=0;completed=false;pthread_mutex_unlock(&lock);
 // A released/replaced owner means native wake may already be in progress.
 // Never send a delayed auxiliary Cancel into that new official turn.
 if(*id && !done && same_lease())(void)event("Cancel","SpeechRecognizer",id,"{}");
}
static bool begin(bool vad=false,unsigned epoch=0) {
 cancel_active();
 char id[96];snprintf(id,sizeof(id),"%s%u",prefix,++serial);
 pthread_mutex_lock(&lock);strcpy(active,id);completed=false;pthread_mutex_unlock(&lock);
 // ASR-only context is per request. Do not alter official GlobalConfig.
 const char *context=",\"context\":[{\"header\":{\"namespace\":\"Execution\",\"name\":\"RequestControl\"},\"payload\":{\"disabled\":[\"NLP\",\"TTS\"]}}]";
 std::string payload="{\"asr\":{\"vad\":"+std::string(vad?"true":"false")+
  ",\"partial_result\":true,\"lang\":\"zh-CN\",\"format\":{\"codec\":\"OPUS\",\"rate\":16000,\"bits\":16,\"channel\":1}},\"is_using_local_vad\":"+(vad?"false":"true")+"}";
 bool ok=event("Recognize","SpeechRecognizer",id,payload.c_str(),context);
 if(ok) {char msg[180];snprintf(msg,sizeof(msg),"{\"type\":\"ready\",\"dialog\":\"%s\",\"epoch\":%u}",id,epoch);reply(msg);}
 return ok;
}
static void *serve(void*) {
 int s=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(s<0)return nullptr;
 sockaddr_un addr={};addr.sun_family=AF_UNIX;strcpy(addr.sun_path,SOCK_PATH);
 // Remove only a refused, unchanged root-owned stale socket after a crash.
 ASR_STAT before={},after={};
 if(!ASR_LSTAT(SOCK_PATH,&before) && S_ISSOCK(before.st_mode) && before.st_uid==0) {
  int check=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
  if(check>=0) {
   int result=connect(check,(sockaddr*)&addr,sizeof(addr)),saved=errno;close(check);
   if(result<0 && saved==ECONNREFUSED && !ASR_LSTAT(SOCK_PATH,&after) &&
      before.st_dev==after.st_dev && before.st_ino==after.st_ino)unlink(SOCK_PATH);
  }
 }
 mode_t old=umask(0077);int bound=bind(s,(sockaddr*)&addr,sizeof(addr));umask(old);
 if(bound || listen(s,1)){close(s);return nullptr;}
 syslog(LOG_INFO,"lx06-control-asr-probe: SDK bridge ready");
 for(;;) {
  int fd=accept4(s,nullptr,nullptr,SOCK_CLOEXEC);if(fd<0)continue;
  ucred cred={};socklen_t len=sizeof(cred);
  if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&len)||cred.uid!=0){close(fd);continue;}
  int owner=open("/tmp/lx06-session.owner",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  lease_bound=owner>=0 && read(owner,lease,sizeof(lease))==sizeof(lease);
  if(owner>=0)close(owner);
  pthread_mutex_lock(&lock);client=fd;pthread_mutex_unlock(&lock);
  reply("{\"type\":\"hello\",\"asr_only\":true}");
  for(;;) {
   pollfd p={fd,POLLIN,0};if(poll(&p,1,15000)<=0)break;
   if(!same_lease())break;
   unsigned char data[4097];ssize_t n=recv(fd,data,sizeof(data),MSG_TRUNC);
   if(n<=0||n>(ssize_t)sizeof(data))break;
   char id[96];pthread_mutex_lock(&lock);strcpy(id,active);pthread_mutex_unlock(&lock);
   if((data[0]=='S'||data[0]=='V') && (n==1||n==5)) {
    unsigned epoch=0;if(n==5)for(int i=0;i<4;i++)epoch|=(unsigned)data[i+1]<<(i*8);
    if(!begin(data[0]=='V',epoch)){reply("{\"type\":\"error\",\"reason\":\"start\"}");break;}
   }
   else if(data[0]=='A' && n>1 && *id) {
    if(!audio(data+1,(unsigned)n-1)){reply("{\"type\":\"error\",\"reason\":\"audio\"}");break;}
   } else if(data[0]=='E' && n==1 && *id) {
    if(!event("RecognizeStreamFinished","SpeechRecognizer",id,"{}"))break;
   } else if(data[0]=='C' && n==1)cancel_active();
   else if(data[0]=='P' && n==1)reply("{\"type\":\"pong\"}");
   else break;
  }
  pthread_mutex_lock(&lock);client=-1;pthread_mutex_unlock(&lock);
  close(fd);cancel_active();
 }
 return nullptr;
}

extern "C" opaque capture_create(opaque&,opaque&,int) asm(CREATE);
extern "C" opaque capture_create(opaque &config,opaque &info,int type) {
 auto original=(creator)dlsym(RTLD_NEXT,CREATE);
 if(!original)return {};
 opaque engine=original(config,info,type);
 if(enabled() && engine) {
  pthread_mutex_lock(&lock);current_engine=engine;bool start=!launched;launched=true;pthread_mutex_unlock(&lock);
  if(start && resolve_abi()) {
   unsigned char nonce[8];int f=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
   ssize_t n=f<0?-1:read(f,nonce,sizeof(nonce));if(f>=0)close(f);
   if(n==sizeof(nonce)) {
    char hex[17];for(int i=0;i<8;i++)snprintf(hex+i*2,3,"%02x",nonce[i]);
    snprintf(prefix,sizeof(prefix),"lx06-ctrl-%s-",hex);
    pthread_t tid;if(!pthread_create(&tid,nullptr,serve,nullptr))pthread_detach(tid);
   }
  }
 }
 return engine;
}
extern "C" bool capture_instruction(const std::string&,opaque&) asm(BUILD_INSTR);
static bool dispatch_instruction(const std::string &text,opaque &instruction,bool sdk_internal) {
 auto original=(builder)dlsym(RTLD_NEXT,BUILD_INSTR);
 if(!original)return false;
 if(enabled() && *prefix && text.find(prefix)!=std::string::npos && text.size()<65536) {
  auto doc=std::make_unique<xz_json_doc>();
  if(!xz_json_parse(doc.get(),text.data(),text.size())) {
   int h=xz_json_object_get(doc.get(),doc->root,"header");char id[96];
   if(!xz_json_get_string(doc.get(),h,"dialog_id",id,sizeof(id)) && owned(id)) {
    // Only our own dialog is diverted, including late results after cancel.
    // Deliver bounded original JSON to the test client; never execute it.
    char ns[64]={},name[64]={};
    (void)xz_json_get_string(doc.get(),h,"namespace",ns,sizeof(ns));
    (void)xz_json_get_string(doc.get(),h,"name",name,sizeof(name));
    int body=xz_json_object_get(doc.get(),doc->root,"payload");
    int fin=xz_json_object_get(doc.get(),body,"is_final");
    bool final=fin>=0 && doc->tokens[fin].end-doc->tokens[fin].start==4 && !text.compare(doc->tokens[fin].start,4,"true");
    pthread_mutex_lock(&lock);
    if(client>=0 && !strcmp(id,active)) {
     if(!strcmp(ns,"SpeechRecognizer") && !strcmp(name,"RecognizeResult") && final)completed=true;
     (void)send(client,text.data(),text.size(),MSG_DONTWAIT|MSG_NOSIGNAL);
    }
    pthread_mutex_unlock(&lock);
    if(sdk_internal) {
     // Keep SDK timeout bookkeeping, but never expose auxiliary speech to
     // official command handlers/log consumers. No NLP/TTS from our dialog
     // may execute, even if a cloud service disregards RequestControl.
     if(!strcmp(ns,"SpeechRecognizer") && !strcmp(name,"RecognizeResult")) {
      std::string sanitized="{\"header\":"+text.substr(doc->tokens[h].start,doc->tokens[h].end-doc->tokens[h].start)+
       ",\"payload\":{\"is_final\":"+(final?"true":"false")+",\"results\":[{\"text\":\"\",\"confidence\":0.0,\"is_nlp_request\":false}]}}";
      return original(sanitized,instruction);
     }
     if((!strcmp(ns,"SpeechRecognizer") && !strcmp(name,"StopCapture")) ||
        (!strcmp(ns,"Dialog") && !strcmp(name,"Finish")))return original(text,instruction);
    }
    instruction.reset();return false;
   }
  }
 }
 return original(text,instruction);
}
extern "C" bool capture_instruction(const std::string &text,opaque &instruction) {
 auto original=(builder)dlsym(RTLD_NEXT,BUILD_INSTR);
 if(!original)return false;
 if(!enabled() || !*prefix || text.find(prefix)==std::string::npos)return original(text,instruction);
 Dl_info origin={},sdk={};
 bool sdk_internal=dladdr(__builtin_return_address(0),&origin) && dladdr((void*)original,&sdk) &&
                   origin.dli_fbase==sdk.dli_fbase && sdk.dli_fname && strstr(sdk.dli_fname,"libaivs_sdk");
 return dispatch_instruction(text,instruction,sdk_internal);
}

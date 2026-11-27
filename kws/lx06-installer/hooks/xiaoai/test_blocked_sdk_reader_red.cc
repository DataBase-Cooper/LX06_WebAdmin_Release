/* Offline expected RED against the real bridge reader. Run only in isolated
 * mount/network namespaces with a private /tmp; SDK and audio are synthetic. */
#include <atomic>
#include <chrono>
#include <thread>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/vfs.h>
#include <linux/magic.h>
static void *fake_dlsym(void *, const char *);
static ssize_t observed_recv(int, void *, size_t, int);
#define dlsym fake_dlsym
#define recv observed_recv
#include "control-asr-probe.cc"
#undef dlsym
#undef recv
static std::atomic<int> blocked{0}, release_post{0}, audio_read{0}, cancel_read{0};
static opaque engine;
static void *base[26], *derived[24];
struct Mock { void **vptr=derived; };
static bool build(const std::string&, opaque &out) { out=std::make_shared<int>(1); return true; }
static bool post(void *, opaque &) {
 blocked.store(1);
 while (!release_post.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
 return true;
}
static bool raw(void *, const unsigned char *, unsigned) { return true; }
static void *fake_dlsym(void *, const char *name) {
 if (!strcmp(name,BUILD_EVENT)) return (void *)build;
 if (!strcmp(name,POST_EVENT)) return (void *)post;
 if (!strcmp(name,POST_RAW)) return (void *)raw;
 if (!strcmp(name,BASE_VTABLE)) return base;
 return nullptr;
}
static ssize_t observed_recv(int fd, void *buf, size_t size, int flags) {
 ssize_t n=syscall(SYS_recvfrom,fd,buf,size,flags,nullptr,nullptr);
 if(n>0) { unsigned char c=*(unsigned char *)buf; if(c=='A') ++audio_read; if(c=='C') ++cancel_read; }
 return n;
}
static bool wait_for(const std::atomic<int> &value,int target,int ms) {
 for(int i=0;i<ms;i++) { if(value.load()>=target)return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
 return value.load()>=target;
}
int main() {
 if(geteuid()!=0 || !getenv("DC_ISOLATED_BLOCKED_ASR_TEST")) return 2;
 struct stat self_net={}, init_net={}, self_mnt={}, init_mnt={}; struct statfs tmp={};
 if(stat("/proc/self/ns/net",&self_net) || stat("/proc/1/ns/net",&init_net) ||
    stat("/proc/self/ns/mnt",&self_mnt) || stat("/proc/1/ns/mnt",&init_mnt) ||
    (self_net.st_ino==init_net.st_ino && self_net.st_dev==init_net.st_dev) ||
    (self_mnt.st_ino==init_mnt.st_ino && self_mnt.st_dev==init_mnt.st_dev) ||
    statfs("/tmp",&tmp) || (unsigned long)tmp.f_type!=TMPFS_MAGIC) return 2;
 engine=std::make_shared<Mock>(); current_engine=engine;
 base[3]=derived[1]=(void *)post; base[5]=derived[3]=(void *)raw;
 if(!resolve_abi()) return 3;
 strcpy(prefix,"lx06-ctrl-offline-");
 pthread_t tid; if(pthread_create(&tid,nullptr,serve,nullptr))return 4;
 int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0); if(fd<0)return 5;
 sockaddr_un addr={};addr.sun_family=AF_UNIX;strcpy(addr.sun_path,SOCK_PATH);
 bool connected=false;
 for(int i=0;i<2000;i++) { if(!connect(fd,(sockaddr*)&addr,sizeof(addr))) {connected=true;break;} std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
 if(!connected)return 6;
 char hello[256]; if(::recv(fd,hello,sizeof(hello),0)<=0)return 7;
 if(send(fd,"S",1,0)!=1 || !wait_for(blocked,1,2000))return 8;
 const unsigned char frame[]={'A',1,2,3};
 for(int i=0;i<8;i++) if(send(fd,frame,sizeof(frame),0)!=(ssize_t)sizeof(frame))return 9;
 if(send(fd,"C",1,0)!=1)return 10;
 bool consumed=wait_for(audio_read,8,500) && wait_for(cancel_read,1,500);
 printf("while SDK postEvent blocked: audio packets read=%d/8 cancel commands read=%d/1\n",audio_read.load(),cancel_read.load()); fflush(stdout);
 release_post.store(1);
 bool recovered=wait_for(audio_read,8,2000) && wait_for(cancel_read,1,2000);
 printf("after SDK release: audio packets read=%d/8 cancel commands read=%d/1\n",audio_read.load(),cancel_read.load()); fflush(stdout);
 /* _exit avoids destroying globals while the production listener remains alive. */
 _exit(consumed && recovered ? 0 : 1);
}

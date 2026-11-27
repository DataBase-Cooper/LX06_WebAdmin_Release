#define _GNU_SOURCE
#include "../bootstrap.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned cases;static uint64_t epoch=300;
static char base[48],sdk[108];static const char *mock;
static uint64_t millis(void){struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;}
static void file(const char *path,const char *text){int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(fd>=0);assert(write(fd,text,strlen(text))==(ssize_t)strlen(text));assert(!close(fd));}
static dc_bootstrap_config config(void){return (dc_bootstrap_config){.private_base=base,.broker_path=mock,.worker_path=mock,.snapshot_client_path=mock,.sdk_path=sdk,.mico_pid=getpid()};}
static dc_bootstrap *start(const char *mode){assert(!setenv("BOOT_FIXTURE_MODE",mode,1));dc_bootstrap_config cfg=config();dc_bootstrap *b=dc_bootstrap_create(++epoch,&cfg,millis());if(!b)fprintf(stderr,"bootstrap fixture create errno=%d\n",errno);assert(b);return b;}
static void ready(dc_bootstrap *b){
 uint64_t end=millis()+3000;
 while(millis()<end){int status=dc_bootstrap_tick(b,millis());assert(status>=0);if(status==1){assert(dc_bootstrap_state(b)==DC_BOOT_READY);return;}poll(NULL,0,2);}
 assert(0);
}
static void drain(dc_bootstrap *b){
 uint64_t end=millis()+3000;
 while(millis()<end){(void)dc_bootstrap_tick(b,millis());if(dc_bootstrap_quiescent(b))return;poll(NULL,0,2);}
 fprintf(stderr,"drain case=%u state=%d error=%d broker=%ld\n",cases,dc_bootstrap_state(b),dc_bootstrap_error(b),(long)dc_bootstrap_broker_pid(b));
 assert(0);
}
static void finish(dc_bootstrap *b){dc_bootstrap_stop(b,millis());drain(b);assert(!dc_bootstrap_destroy(b));}
static void normal(void){
 dc_bootstrap *b=start("normal");assert(dc_bootstrap_destroy(b)==-1&&errno==EBUSY);ready(b);
 char path[108];strcpy(path,dc_bootstrap_socket(b));pid_t pid=dc_bootstrap_broker_pid(b);assert(pid>1);finish(b);
 assert(access(path,F_OK)&&kill(pid,0)<0&&errno==ESRCH);cases++;
}
static void ignores_term(void){
 dc_bootstrap *b=start("ignore");ready(b);pid_t pid=dc_bootstrap_broker_pid(b);uint64_t now=millis();dc_bootstrap_stop(b,now);
 (void)dc_bootstrap_tick(b,now+79);assert(!kill(pid,0)&&!dc_bootstrap_quiescent(b));
 (void)dc_bootstrap_tick(b,now+80);drain(b);assert(!dc_bootstrap_destroy(b));assert(kill(pid,0)<0&&errno==ESRCH);cases++;
}
static void startup_deadline(void){
 dc_bootstrap *b=start("hang");assert(dc_bootstrap_tick(b,millis()+10001)==-1);assert(dc_bootstrap_error(b)==DC_BOOT_ERR_DEADLINE);
 dc_bootstrap_stop(b,millis());drain(b);assert(!dc_bootstrap_destroy(b));cases++;
}
static void descendant(int unexpected){
 dc_bootstrap *b=start("fork");ready(b);
 if(unexpected){assert(!kill(dc_bootstrap_broker_pid(b),SIGKILL));for(unsigned i=0;i<200&&dc_bootstrap_tick(b,millis())>=0;i++)poll(NULL,0,2);assert(dc_bootstrap_error(b)==DC_BOOT_ERR_PROCESS);}
 finish(b);int status;assert(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD);cases++;
}
static void replacement(void){
 dc_bootstrap *b=start("normal");ready(b);char path[108];strcpy(path,dc_bootstrap_socket(b));
 assert(!unlink(path));file(path,"foreign-fixture-node");dc_bootstrap_stop(b,millis());
 for(unsigned i=0;i<200;i++){(void)dc_bootstrap_tick(b,millis());poll(NULL,0,2);}
 assert(!dc_bootstrap_quiescent(b)&&dc_bootstrap_error(b)==DC_BOOT_ERR_CLEANUP);
 int fd=open(path,O_RDONLY);char raw[32]={0};assert(fd>=0&&read(fd,raw,sizeof raw)>0&&!strcmp(raw,"foreign-fixture-node"));close(fd);
 assert(!unlink(path));drain(b);assert(!dc_bootstrap_destroy(b));cases++;
}
static void existing_directory(void){
 char dir[108],path[128];snprintf(dir,sizeof dir,"%s/lx06-dialogue.%llu",base,(unsigned long long)++epoch);assert(!mkdir(dir,0700));snprintf(path,sizeof path,"%s/keep",dir);file(path,"keep");
 dc_bootstrap_config cfg=config();assert(!dc_bootstrap_create(epoch,&cfg,millis())&&errno==EEXIST);assert(!access(path,F_OK));assert(!unlink(path)&&!rmdir(dir));cases++;
}
static pid_t external(const char *path){
 assert(!setenv("BOOT_FIXTURE_MODE","normal",1));pid_t child=fork();assert(child>=0);
 if(!child){execl(mock,mock,"--serve","--socket",path,"--worker",mock,"--snapshot-client",mock,"--sdk",sdk,"--mico-pid","2",(char *)NULL);_exit(127);}
 return child;
}
static void external_mode(int wrong_peer){
 char dir[108],path[108];snprintf(dir,sizeof dir,"%s/external-%u",base,cases);assert(!mkdir(dir,0700));assert(snprintf(path,sizeof path,"%s/tts.sock",dir)<(int)sizeof path);
 pid_t child=external(path);dc_bootstrap_config cfg={.external_socket=path,.external_peer_pid=wrong_peer?getpid():child};
 dc_bootstrap *b=dc_bootstrap_create(++epoch,&cfg,millis());assert(b);
 if(wrong_peer){for(unsigned i=0;i<1000&&dc_bootstrap_tick(b,millis())>=0;i++)poll(NULL,0,2);assert(dc_bootstrap_error(b)==DC_BOOT_ERR_PEER);}
 else {ready(b);assert(dc_bootstrap_broker_pid(b)==child);}
 finish(b);assert(!kill(child,0)&&!access(path,F_OK));assert(!kill(child,SIGTERM));int status;assert(waitpid(child,&status,0)==child);assert(!unlink(path)&&!rmdir(dir));cases++;
}
static void stale_external(void){
 char dir[108],path[108];snprintf(dir,sizeof dir,"%s/stale",base);assert(!mkdir(dir,0700));assert(snprintf(path,sizeof path,"%s/tts.sock",dir)<(int)sizeof path);
 int fd=socket(AF_UNIX,SOCK_SEQPACKET,0);assert(fd>=0);struct sockaddr_un a={0};a.sun_family=AF_UNIX;strcpy(a.sun_path,path);assert(!bind(fd,(struct sockaddr *)&a,sizeof a)&&!chmod(path,0600));close(fd);
 dc_bootstrap_config cfg={.external_socket=path};dc_bootstrap *b=dc_bootstrap_create(++epoch,&cfg,millis());assert(b);
 assert(!dc_bootstrap_tick(b,millis()));assert(dc_bootstrap_tick(b,millis()+10001)==-1&&dc_bootstrap_error(b)==DC_BOOT_ERR_DEADLINE);
 finish(b);assert(!access(path,F_OK)&&!unlink(path)&&!rmdir(dir));cases++;
}
static void config_refusals(void){
 dc_bootstrap_config cfg=config();cfg.sdk_path=NULL;assert(!unsetenv("LX06_NATIVE_TTS_SDK"));assert(!dc_bootstrap_create(++epoch,&cfg,millis()));
 cfg=config();assert(!prctl(PR_SET_CHILD_SUBREAPER,0));assert(!dc_bootstrap_create(++epoch,&cfg,millis())&&errno==EPERM);assert(!prctl(PR_SET_CHILD_SUBREAPER,1));cases++;
}
static void scanner(void){
 char proc[108],dir[128],path[144];snprintf(proc,sizeof proc,"%s/proc",base);assert(!mkdir(proc,0700));snprintf(dir,sizeof dir,"%s/%ld",proc,(long)getpid());assert(!mkdir(dir,0700));snprintf(path,sizeof path,"%s/comm",dir);file(path,"fixture-mico\n");
 pid_t pid;int found=dc_bootstrap_find_mico(proc,"fixture-mico",&pid);if(found)fprintf(stderr,"scanner fixture errno=%d\n",errno);assert(!found&&pid==getpid());
 pid_t child=fork();assert(child>=0);if(!child){for(;;)pause();}
 char other[128],other_file[144];snprintf(other,sizeof other,"%s/%ld",proc,(long)child);assert(!mkdir(other,0700));snprintf(other_file,sizeof other_file,"%s/comm",other);file(other_file,"fixture-mico\n");
 assert(dc_bootstrap_find_mico(proc,"fixture-mico",&pid)==-1&&errno==EEXIST);assert(!kill(child,SIGTERM));int status;assert(waitpid(child,&status,0)==child);
 assert(!unlink(other_file)&&!rmdir(other)&&!unlink(path)&&!rmdir(dir)&&!rmdir(proc));cases++;
}
static atomic_int running;
static void *busy(void *ignored){(void)ignored;while(atomic_load(&running)){char *p=malloc(2048);assert(p);snprintf(p,2048,"synthetic");free(p);}return NULL;}
static void threaded_fork(void){
 int source=open("/dev/null",O_RDONLY);assert(source>=0);int leaked=fcntl(source,F_DUPFD,40);close(source);assert(leaked==40);assert(!setenv("BOOT_FORBID_FD","40",1));
 pthread_t thread;atomic_store(&running,1);assert(!pthread_create(&thread,NULL,busy,NULL));dc_bootstrap *b=start("normal");ready(b);finish(b);atomic_store(&running,0);assert(!pthread_join(thread,NULL));
 assert(fcntl(leaked,F_GETFD)>=0);close(leaked);assert(!unsetenv("BOOT_FORBID_FD"));cases++;
}
int main(int argc,char **argv){
 assert((argc==2||(argc==3&&!strcmp(argv[2],"--external-only")))&&geteuid()==0);mock=argv[1];char temporary[]="/tmp/dc-boot-XXXXXX";assert(mkdtemp(temporary));strcpy(base,temporary);assert(!chmod(base,0700));snprintf(sdk,sizeof sdk,"%s/sdk.fixture",base);file(sdk,"synthetic-no-sdk");
 assert(!unsetenv("LX06_NATIVE_TTS_SOCKET"));assert(!prctl(PR_SET_CHILD_SUBREAPER,1));struct rlimit limit={128,128};assert(!setrlimit(RLIMIT_NOFILE,&limit));
 if(argc==3){external_mode(0);external_mode(1);stale_external();}
 else {config_refusals();normal();ignores_term();startup_deadline();descendant(0);descendant(1);replacement();existing_directory();external_mode(0);external_mode(1);stale_external();scanner();threaded_fork();}
 assert(!unlink(sdk)&&!rmdir(base));printf("bootstrap cases=%u passed\n",cases);return 0;
}

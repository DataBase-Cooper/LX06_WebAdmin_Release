#define _GNU_SOURCE
#include "bootstrap.h"
#include "io/compat.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILDREN 8u
#define SCAN_LIMIT 2048u
struct process {pid_t pid,parent,group;uint64_t ticks;char state;};
struct dc_bootstrap {
 enum dc_bootstrap_state state;enum dc_bootstrap_error error;
 int external,base_fd,dir_fd,probe_fd,removed,killed,scan_failed;
 uint64_t deadline,kill_at;pid_t owner,external_pin;
 struct process broker,mico,children[CHILDREN];unsigned child_count;
 dc_io_stat directory,socket_node,helper_node[2];int socket_pinned;
 char base[PATH_MAX],name[64],socket_path[108];
};
static const char *value(const char *given,const char *key,const char *fallback){
 if(given&&*given)return given;
 const char *environment=key?getenv(key):NULL;return environment&&*environment?environment:fallback;
}
static int path_valid(const char *path,size_t limit){
 return path&&path[0]=='/'&&strnlen(path,limit)<limit&&!strchr(path,'\n')&&!strchr(path,'\r');
}
static int decimal(const char *text,pid_t *pid){
 if(!text||!*text)return -1;
 unsigned long value=0;
 for(const unsigned char *p=(const unsigned char *)text;*p;p++){
  if(*p<'0'||*p>'9'||value>((unsigned long)INT_MAX-(*p-'0'))/10)return -1;
  value=value*10+(*p-'0');
 }
 if(value<2)return -1;
 *pid=(pid_t)value;return 0;
}
static int process_read(pid_t pid,struct process *out){
 char path[64],data[2048];snprintf(path,sizeof path,"/proc/%ld/stat",(long)pid);
 int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
 ssize_t n=read(fd,data,sizeof(data)-1);int saved=errno;close(fd);errno=saved;
 if(n<=0||n==(ssize_t)sizeof(data)-1)return -1;
 data[n]=0;char *at=strrchr(data,')');if(!at||at[1]!=' ')return -1;at+=2;
 memset(out,0,sizeof(*out));out->pid=pid;out->state=*at;
 for(unsigned field=3;field<=22;field++){
  char *end=strchr(at,' ');if(field!=22&&!end)return -1;
  if(field==4||field==5||field==22){
   char save=end?*end:0;if(end)*end=0;uint64_t v=0;int bad=!*at;
   for(const unsigned char *digit=(const unsigned char *)at;*digit;digit++){
    if(*digit<'0'||*digit>'9'||v>(UINT64_MAX-(*digit-'0'))/10){bad=1;break;}
    v=v*10+(*digit-'0');
   }
   if(field!=22&&v>INT_MAX)bad=1;
   if(end)*end=save;
   if(bad)return -1;
   if(field==4)out->parent=(pid_t)v;else if(field==5)out->group=(pid_t)v;else out->ticks=v;
  }
  if(end)at=end+1;
 }
 return out->ticks?0:-1;
}
static int same_process(const struct process *pin,struct process *current){
 return pin->pid>1&&!process_read(pin->pid,current)&&pin->ticks==current->ticks&&pin->group==current->group;
}
static int root_process(pid_t pid){
 char path[64];snprintf(path,sizeof path,"/proc/%ld",(long)pid);int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
 dc_io_stat st;int okay=fd>=0&&!dc_io_fstat(fd,&st)&&st.st_uid==0;if(fd>=0)close(fd);return okay;
}
int dc_bootstrap_find_mico(const char *proc_root,const char *comm,pid_t *pid){
 if(!pid||!path_valid(proc_root,PATH_MAX)||!comm||!*comm||strnlen(comm,16)>=16||strchr(comm,'/')||strchr(comm,'\n')){errno=EINVAL;return -1;}
 DIR *dir=opendir(proc_root);if(!dir)return -1;struct dirent *entry;pid_t found=0;int bad=0;
 while((entry=readdir(dir))){
  pid_t candidate;if(decimal(entry->d_name,&candidate))continue;
  char relative[64];int n=snprintf(relative,sizeof relative,"%ld/comm",(long)candidate);if(n<0||(size_t)n>=sizeof relative){bad=1;break;}
  int fd=openat(dirfd(dir),relative,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
  char name[18];ssize_t bytes=read(fd,name,sizeof(name)-1);close(fd);if(bytes<=0||bytes==(ssize_t)sizeof(name)-1)continue;
  name[bytes]=0;if(bytes&&name[bytes-1]=='\n')name[--bytes]=0;
  if(strcmp(name,comm))continue;
  struct process identity;if(process_read(candidate,&identity)||identity.state=='Z'||!root_process(candidate))continue;
  if(found){bad=1;break;}found=candidate;
 }
 closedir(dir);if(bad||!found){errno=bad?EEXIST:ESRCH;return -1;}*pid=found;return 0;
}
static int trusted_file(const char *path,int executable){
 if(!path_valid(path,PATH_MAX)){errno=EINVAL;return -1;}
 int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);dc_io_stat st;
 if(fd<0)return -1;
 if(dc_io_fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid||st.st_mode&0022||(executable&&!(st.st_mode&0111))){close(fd);errno=EACCES;return -1;}
 return fd;
}
static int directory_valid(dc_bootstrap *b){
 dc_io_stat st;
 return b->dir_fd>=0&&!dc_io_fstat(b->dir_fd,&st)&&S_ISDIR(st.st_mode)&&!st.st_uid&&(st.st_mode&0777)==0700&&
  st.st_dev==b->directory.st_dev&&st.st_ino==b->directory.st_ino;
}
static void fail(dc_bootstrap *b,enum dc_bootstrap_error error){if(!b->error)b->error=error;}
static int known(const dc_bootstrap *b,pid_t pid,uint64_t ticks){
 if(b->broker.pid==pid&&b->broker.ticks==ticks)return 1;
 for(unsigned i=0;i<b->child_count;i++)if(b->children[i].pid==pid&&b->children[i].ticks==ticks)return 1;
 return 0;
}
static int orphan_helper(const dc_bootstrap *b,const struct process *p){
 if(p->parent!=b->owner||p->group!=p->pid||p->ticks<b->broker.ticks)return 0;
 char path[64];snprintf(path,sizeof path,"/proc/%ld/exe",(long)p->pid);
 int fd=open(path,O_RDONLY|O_CLOEXEC);dc_io_stat st;
 int okay=fd>=0&&!dc_io_fstat(fd,&st)&&!st.st_uid&&S_ISREG(st.st_mode);
 if(fd>=0)close(fd);
 if(!okay)return 0;
 for(unsigned i=0;i<2;i++)if(st.st_dev==b->helper_node[i].st_dev&&st.st_ino==b->helper_node[i].st_ino)return 1;
 return 0;
}
/* Called only on shutdown, never every 5ms in a healthy conversation. Parent
 * links are numeric and pinned to exact births; unrelated core jobs are ignored. */
static int descendants(dc_bootstrap *b){
 struct process *all=calloc(SCAN_LIMIT,sizeof(*all));if(!all)return -1;
 DIR *dir=opendir("/proc");if(!dir){free(all);return -1;}unsigned count=0;struct dirent *entry;int bad=0;
 while((entry=readdir(dir))){
  pid_t pid;if(decimal(entry->d_name,&pid))continue;
  if(count==SCAN_LIMIT){bad=1;break;}
  if(!process_read(pid,all+count))count++;
 }
 closedir(dir);
 for(unsigned i=0;i<count&&!bad;i++){
  if(known(b,all[i].pid,all[i].ticks)||all[i].pid==b->owner)continue;
  struct process p=all[i];int owned=orphan_helper(b,&p);
  for(unsigned depth=0;depth<32;depth++){
   if(known(b,p.pid,p.ticks)){owned=1;break;}
   if(p.parent<2||p.parent==p.pid||p.parent==b->owner)break;
   unsigned j=0;while(j<count&&all[j].pid!=p.parent)j++;if(j==count||all[j].ticks>p.ticks)break;p=all[j];
  }
  if(!owned)continue;
  if(b->child_count==CHILDREN){bad=1;break;}
  b->children[b->child_count++]=all[i];
 }
 free(all);if(bad){errno=E2BIG;return -1;}return 0;
}
static void signal_pin(const struct process *pin,int signal_number){
 struct process current;if(same_process(pin,&current)&&current.state!='Z')kill(pin->pid,signal_number);
}
static void signal_all(dc_bootstrap *b,int signal_number){
 /* Each SDK/snapshot child has its own PGID. Signal only attested individual
  * PIDs, never a reused/broad session group or a process selected by name. */
 for(unsigned i=0;i<b->child_count;i++)signal_pin(b->children+i,signal_number);
 signal_pin(&b->broker,signal_number);
}
static void reap(struct process *pin){
 if(pin->pid<2)return;
 int status;pid_t result=waitpid(pin->pid,&status,WNOHANG);
 if(result==pin->pid){pin->pid=0;return;}
 if(result<0&&errno==ECHILD){struct process current;if(process_read(pin->pid,&current)||current.ticks!=pin->ticks)pin->pid=0;}
}
static int live(const dc_bootstrap *b){
 if(b->broker.pid>1)return 1;
 for(unsigned i=0;i<b->child_count;i++)if(b->children[i].pid>1)return 1;
 return 0;
}
static void cleanup(dc_bootstrap *b){
 if(b->external||b->removed||live(b))return;
 if(!directory_valid(b)){fail(b,DC_BOOT_ERR_CLEANUP);return;}
 dc_io_stat st;
 if(!dc_io_fstatat(b->dir_fd,"tts.sock",&st,AT_SYMLINK_NOFOLLOW)){
  if(!b->socket_pinned||!S_ISSOCK(st.st_mode)||st.st_uid||st.st_dev!=b->socket_node.st_dev||st.st_ino!=b->socket_node.st_ino||unlinkat(b->dir_fd,"tts.sock",0)){fail(b,DC_BOOT_ERR_CLEANUP);return;}
 }else if(errno!=ENOENT){fail(b,DC_BOOT_ERR_CLEANUP);return;}
 if(dc_io_fstatat(b->base_fd,b->name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(st.st_mode)||st.st_ino!=b->directory.st_ino||st.st_dev!=b->directory.st_dev||unlinkat(b->base_fd,b->name,AT_REMOVEDIR)){
  fail(b,DC_BOOT_ERR_CLEANUP);return;
 }
 close(b->dir_fd);b->dir_fd=-1;close(b->base_fd);b->base_fd=-1;b->removed=1;b->state=DC_BOOT_STOPPED;
}
static int socket_check(dc_bootstrap *b,dc_io_stat *node){
 if(b->external){
  char parent[108];strcpy(parent,b->socket_path);char *slash=strrchr(parent,'/');if(!slash||slash==parent)return -1;*slash=0;
  int fd=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);dc_io_stat st;
  int okay=fd>=0&&!dc_io_fstat(fd,&st)&&S_ISDIR(st.st_mode)&&!st.st_uid&&(st.st_mode&0777)==0700;
  if(fd>=0)close(fd);
  if(!okay){errno=EACCES;return -1;}
  int base=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(base<0)return -1;
  int result=dc_io_fstatat(base,strrchr(b->socket_path,'/')+1,node,AT_SYMLINK_NOFOLLOW);int saved=errno;close(base);errno=saved;if(result)return -1;
 }else if(!directory_valid(b)||dc_io_fstatat(b->dir_fd,"tts.sock",node,AT_SYMLINK_NOFOLLOW))return -1;
 if(!S_ISSOCK(node->st_mode)||node->st_uid||(node->st_mode&0777)!=0600){errno=EACCES;return -1;}
 return 0;
}
static int ready_probe(dc_bootstrap *b){
 dc_io_stat before,after;if(socket_check(b,&before))return errno==ENOENT?0:-1;
 int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return -1;
 struct sockaddr_un address={0};address.sun_family=AF_UNIX;strcpy(address.sun_path,b->socket_path);
 if(connect(fd,(struct sockaddr *)&address,sizeof address)){int saved=errno;close(fd);return saved==ECONNREFUSED||saved==ENOENT||saved==EAGAIN||saved==EINPROGRESS?0:-1;}
 struct ucred peer;socklen_t bytes=sizeof peer;
 int okay=!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&bytes)&&bytes==sizeof peer&&peer.uid==0&&peer.pid>1;
 if(okay&&!b->external)okay=peer.pid==b->broker.pid;
 if(okay&&b->external&&b->external_pin>1)okay=peer.pid==b->external_pin;
 struct process current;if(okay)okay=!process_read(peer.pid,&current)&&current.state!='Z';
 if(okay&&!b->external)okay=current.ticks==b->broker.ticks&&current.parent==b->owner&&current.group==b->broker.pid;
 if(okay)okay=!socket_check(b,&after)&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino;
 close(fd);if(!okay){errno=EACCES;return -1;}
 if(b->external)b->broker=current;
 b->socket_node=before;b->socket_pinned=1;return 1;
}
dc_bootstrap *dc_bootstrap_create(uint64_t epoch,const dc_bootstrap_config *config,uint64_t now){
 if(!epoch||!config||geteuid()!=0){errno=EINVAL;return NULL;}
 dc_bootstrap *b=calloc(1,sizeof(*b));if(!b)return NULL;
 b->owner=getpid();b->base_fd=b->dir_fd=b->probe_fd=-1;b->state=DC_BOOT_STARTING;b->deadline=now+10000;
 /* Normal callers borrow the boot service, whose broker alone owns the
  * lifecycle snapshot and supervised worker. An explicit private base keeps
  * the original managed harness/legacy opt-in; absence never launches a second
  * broker or falls back from a missing supervised service. */
 const char *external=value(config->external_socket,"LX06_NATIVE_TTS_SOCKET",
     config->private_base&&*config->private_base?NULL:"/tmp/run/lx06-native-audio-state/tts.sock");
 if(external){
  if(!path_valid(external,sizeof b->socket_path))goto invalid;
  strcpy(b->socket_path,external);b->external=1;b->external_pin=config->external_peer_pid;return b;
 }
 int subreaper=0;if(prctl(PR_GET_CHILD_SUBREAPER,&subreaper)||!subreaper){errno=EPERM;goto invalid;}
 const char *sdk=value(config->sdk_path,"LX06_NATIVE_TTS_SDK",NULL);
 const char *broker=value(config->broker_path,NULL,"/usr/lib/lx06/native-tts-broker");
 const char *worker=value(config->worker_path,NULL,"/usr/lib/lx06/native-tts-session-worker");
 const char *snapshot=value(config->snapshot_client_path,NULL,"/usr/lib/lx06/native-snapshot-client");
 int executable=trusted_file(broker,1);if(executable<0)goto invalid;
 const char *inputs[]={worker,snapshot,sdk};
 for(unsigned i=0;i<3;i++){
  int fd=trusted_file(inputs[i],i<2);if(fd<0){close(executable);goto invalid;}
  if(i<2&&dc_io_fstat(fd,b->helper_node+i)){close(fd);close(executable);goto invalid;}
  close(fd);
 }
 pid_t mico=config->mico_pid;
 if(mico<2&&dc_bootstrap_find_mico(value(config->proc_root,NULL,"/proc"),value(config->mico_comm,"LX06_NATIVE_TTS_MICO_COMM",NULL),&mico)){close(executable);goto invalid;}
 if(process_read(mico,&b->mico)||b->mico.state=='Z'||!root_process(mico)){close(executable);errno=ESRCH;goto invalid;}
 const char *base=value(config->private_base,NULL,"/tmp");
 if(!path_valid(base,sizeof b->base)){close(executable);goto invalid;}
 strcpy(b->base,base);b->base_fd=open(base,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);dc_io_stat parent;
 if(b->base_fd<0||dc_io_fstat(b->base_fd,&parent)||!S_ISDIR(parent.st_mode)||parent.st_uid||((parent.st_mode&0022)&&!(parent.st_mode&S_ISVTX))){close(executable);errno=EACCES;goto invalid;}
 snprintf(b->name,sizeof b->name,"lx06-dialogue.%llu",(unsigned long long)epoch);
 int length=snprintf(b->socket_path,sizeof b->socket_path,"%s/%s/tts.sock",base,b->name);
 if(length<0||(size_t)length>=sizeof b->socket_path){close(executable);errno=ENAMETOOLONG;goto invalid;}
 if(mkdirat(b->base_fd,b->name,0700)){close(executable);goto invalid;}
 b->dir_fd=openat(b->base_fd,b->name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
 if(b->dir_fd<0||dc_io_fstat(b->dir_fd,&b->directory)||b->directory.st_uid||fchmod(b->dir_fd,0700)||dc_io_fstat(b->dir_fd,&b->directory)){
  close(executable);goto created_failure;
 }
 /* All formatting, configuration and libc directory work stays before fork:
  * fixed player/light threads may already exist. The child only uses bounded
  * descriptor/syscall operations and the async-safe POSIX fexecve operation. */
 char mico_text[32];snprintf(mico_text,sizeof mico_text,"%ld",(long)mico);
 char *const argv[]={(char *)broker,"--serve","--socket",b->socket_path,"--worker",(char *)worker,"--snapshot-client",(char *)snapshot,"--sdk",(char *)sdk,"--mico-pid",mico_text,NULL};
 struct sigaction reset;memset(&reset,0,sizeof reset);reset.sa_handler=SIG_DFL;sigemptyset(&reset.sa_mask);
 struct rlimit no_core={0,0},descriptors;
 if(getrlimit(RLIMIT_NOFILE,&descriptors)||descriptors.rlim_cur>1048576){close(executable);errno=E2BIG;goto created_failure;}
 int close_limit=(int)descriptors.rlim_cur,sink=open("/dev/null",O_RDWR|O_CLOEXEC);
 if(sink<0){close(executable);goto created_failure;}
 extern char **environ;char **child_environment=environ;
 pid_t child=fork();if(child<0){close(sink);close(executable);goto created_failure;}
 if(!child){
  if(sigaction(SIGTERM,&reset,NULL)||sigaction(SIGINT,&reset,NULL)||sigaction(SIGHUP,&reset,NULL)||sigaction(SIGPIPE,&reset,NULL))_exit(126);
  if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=b->owner||setrlimit(RLIMIT_CORE,&no_core))_exit(126);
  for(int fd=0;fd<3;fd++)if(dup2(sink,fd)<0)_exit(126);
  for(int fd=3;fd<close_limit;fd++)if(fd!=executable)close(fd);
  fexecve(executable,argv,child_environment);_exit(127);
 }
 close(sink);close(executable);(void)setpgid(child,child);
 if(process_read(child,&b->broker)||b->broker.parent!=b->owner||b->broker.group!=child){
  /* It remains our unreaped direct child, so this literal child PID cannot be
   * reused. No process-name or broad group termination is used. */
  kill(child,SIGKILL);b->broker.pid=child;fail(b,DC_BOOT_ERR_SPAWN);b->state=DC_BOOT_STOPPING;b->killed=1;return b;
 }
 return b;
created_failure:
 (void)unlinkat(b->base_fd,b->name,AT_REMOVEDIR);
invalid:{int saved=errno?errno:EINVAL;if(b->dir_fd>=0)close(b->dir_fd);if(b->base_fd>=0)close(b->base_fd);free(b);errno=saved;return NULL;}
}
void dc_bootstrap_stop(dc_bootstrap *b,uint64_t now){
 if(!b||b->state==DC_BOOT_STOPPED||b->state==DC_BOOT_STOPPING)return;
 if(b->external){b->state=DC_BOOT_STOPPED;b->removed=1;return;}
 b->state=DC_BOOT_STOPPING;b->kill_at=now+80;
 if(descendants(b)){fail(b,DC_BOOT_ERR_LIMIT);b->scan_failed=1;}
 signal_all(b,SIGTERM);
}
int dc_bootstrap_tick(dc_bootstrap *b,uint64_t now){
 if(!b){errno=EINVAL;return -1;}
 if(b->state==DC_BOOT_STOPPED)return b->error?-1:0;
 if(b->external){
  if(b->state==DC_BOOT_READY){struct process current;if(!same_process(&b->broker,&current)||current.state=='Z'){fail(b,DC_BOOT_ERR_PROCESS);b->state=DC_BOOT_STOPPED;}return b->error?-1:1;}
  int ready=ready_probe(b);if(ready>0)b->state=DC_BOOT_READY;
  else if(ready<0||now>=b->deadline){fail(b,ready<0?DC_BOOT_ERR_PEER:DC_BOOT_ERR_DEADLINE);b->state=DC_BOOT_STOPPED;}
  return b->error?-1:ready;
 }
 if(b->state!=DC_BOOT_STOPPING){
  struct process current;
  if(!same_process(&b->broker,&current)||current.state=='Z'||!same_process(&b->mico,&current)||current.state=='Z'){
   fail(b,DC_BOOT_ERR_PROCESS);dc_bootstrap_stop(b,now);
  }else if(b->state==DC_BOOT_STARTING){
   int ready=ready_probe(b);
   if(ready>0)b->state=DC_BOOT_READY;
   else if(ready<0||now>=b->deadline){fail(b,ready<0?DC_BOOT_ERR_PEER:DC_BOOT_ERR_DEADLINE);dc_bootstrap_stop(b,now);}
  }
 }
 if(b->state==DC_BOOT_STOPPING){
  if(!b->killed&&now>=b->kill_at){
   if(descendants(b)){fail(b,DC_BOOT_ERR_LIMIT);b->scan_failed=1;}
   signal_all(b,SIGKILL);b->killed=1;
  }
  reap(&b->broker);for(unsigned i=0;i<b->child_count;i++)reap(b->children+i);
  if(!live(b)&&!b->scan_failed)cleanup(b);
 }
 return b->error?-1:b->state==DC_BOOT_READY?1:0;
}
int dc_bootstrap_quiescent(const dc_bootstrap *b){return b&&b->state==DC_BOOT_STOPPED&&(b->external||(!live(b)&&b->removed));}
int dc_bootstrap_destroy(dc_bootstrap *b){
 if(!b)return 0;
 if(!dc_bootstrap_quiescent(b)){errno=EBUSY;return -1;}
 if(b->dir_fd>=0)close(b->dir_fd);
 if(b->base_fd>=0)close(b->base_fd);
 memset(b,0,sizeof(*b));free(b);return 0;
}
const char *dc_bootstrap_socket(const dc_bootstrap *b){return b?b->socket_path:NULL;}
pid_t dc_bootstrap_broker_pid(const dc_bootstrap *b){return b?b->broker.pid:0;}
enum dc_bootstrap_state dc_bootstrap_state(const dc_bootstrap *b){return b?b->state:DC_BOOT_STOPPED;}
enum dc_bootstrap_error dc_bootstrap_error(const dc_bootstrap *b){return b?b->error:DC_BOOT_ERR_PROCESS;}

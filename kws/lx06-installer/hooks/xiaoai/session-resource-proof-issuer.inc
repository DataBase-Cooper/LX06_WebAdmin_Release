/* Included only by original session-run; no public caller-issued proof. */
#include "session-resource-proof-internal.h"
struct lx06_resource_issuer {int fd,started,invalid;lx06_resource_message start;};
static int resource_issuer_open(struct lx06_resource_issuer *p) {
    memset(p,0,sizeof(*p));p->fd=-1;const char *v=getenv("LX06_SESSION_PROOF_FD");
    if(!v)return 0;
    unsigned fd=0;if(!*v||*v=='0')return -1;
    for(const char *s=v;*s;s++){if(*s<'0'||*s>'9'||fd>1024)return -1;fd=fd*10+(unsigned)(*s-'0');}
    if(fd<3||fd>1024)return -1;
    LX06_SESSION_STATBUF st;struct ucred peer;socklen_t n=sizeof(peer);
    if(!lx06_resource_socket((int)fd,&st)||getsockopt((int)fd,SOL_SOCKET,SO_PEERCRED,&peer,&n)||
       n!=sizeof(peer)||peer.pid!=getppid()||peer.uid!=getuid()||peer.gid!=getgid())return -1;
    int flags=fcntl((int)fd,F_GETFL);
    if(flags<0||fcntl((int)fd,F_SETFD,FD_CLOEXEC)||fcntl((int)fd,F_SETFL,flags|O_NONBLOCK)||
       unsetenv("LX06_SESSION_PROOF_FD"))return -1;
    p->fd=(int)fd;return 0;
}
static int resource_issuer_send(struct lx06_resource_issuer *p,const lx06_resource_message *m) {
    ssize_t n;do{n=send(p->fd,m,sizeof(*m),MSG_DONTWAIT|MSG_NOSIGNAL);}while(n<0&&errno==EINTR);
    return n==(ssize_t)sizeof(*m);
}
/* The original OFD's kernel lock record, not an unrelated free-flock/PID scan.
 * Missing/partial/unsupported fdinfo refuses only the explicit proof mode. */
static int resource_issuer_lock(int lock) {
    char path[64],bytes[4096];snprintf(path,sizeof path,"/proc/self/fdinfo/%d",lock);
    int f=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(f<0)return 0;
    ssize_t n=read(f,bytes,sizeof(bytes)-1);char extra;ssize_t tail=n>0?read(f,&extra,1):-1;close(f);
    if(n<=0||n==(ssize_t)sizeof(bytes)-1||tail!=0||memchr(bytes,0,(size_t)n))return 0;
    bytes[n]=0;unsigned found=0;char *line=bytes;
    while(*line){char *end=strchr(line,'\n');if(!end)return 0;*end=0;
        if(!strncmp(line,"lock:",5)){
            unsigned long long index=0,pid=0;char kind[16],scope[16],type[16],device[64],first[32],last[16];int used=0;
            if(sscanf(line,"lock: %llu: %15s %15s %15s %llu %63s %31s %15s %n",&index,kind,scope,type,&pid,device,first,last,&used)!=8||
               line[used]||strcmp(kind,"FLOCK")||strcmp(scope,"ADVISORY")||strcmp(type,"WRITE")||
               pid!=(unsigned long long)getpid()||strcmp(first,"0")||strcmp(last,"EOF")||++found!=1)return 0;
        }
        line=end+1;
    }
    return found==1;
}
static int resource_issuer_current(const struct lx06_resource_issuer *p,int lock) {
    LX06_SESSION_STATBUF held,named;unsigned char raw[sizeof(struct lx06_session_record)+LX06_SESSION_NAME_SIZE];
    const lx06_resource_message *m=&p->start;
    if(LX06_RESOURCE_FSTAT(lock,&held)||LX06_SESSION_STAT(LX06_SESSION_FILE,&named)||
       !S_ISREG(held.st_mode)||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||
       (uint64_t)held.st_dev!=m->device||(uint64_t)held.st_ino!=m->inode||
       held.st_uid!=m->uid||held.st_gid!=m->gid||held.st_mode!=m->mode||held.st_nlink!=m->links||
       held.st_size!=(off_t)sizeof(raw)||pread(lock,raw,sizeof(raw),0)!=(ssize_t)sizeof(raw)||
       memcmp(raw,&m->record,sizeof(m->record))||memcmp(raw+sizeof(m->record),m->name,sizeof(m->name)))return 0;
    return resource_issuer_lock(lock);
}
static int resource_issuer_start(struct lx06_resource_issuer *p,int lock,const struct lx06_session_record *record,const char *name) {
    if(p->fd<0)return 1;
    LX06_SESSION_STATBUF st;int subreaper=0;
    if(LX06_RESOURCE_FSTAT(lock,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||
       st.st_nlink!=1||(st.st_mode&07777)!=0600||!lx06_session_name_valid(name)||
       record->supervisor!=(uint32_t)getpid()||record->supervisor_ticks!=lx06_process_ticks((unsigned)getpid())||
       record->group<2||!record->group_ticks||record->group_ticks!=lx06_process_ticks(record->group)||
       record->reserved!=(uint32_t)lock||prctl(PR_GET_CHILD_SUBREAPER,&subreaper)||subreaper!=1)return 0;
    lx06_resource_message *m=&p->start;memset(m,0,sizeof(*m));m->magic=LX06_RESOURCE_MAGIC;m->version=1;m->kind=LX06_RESOURCE_START;
    m->record=*record;m->device=(uint64_t)st.st_dev;m->inode=(uint64_t)st.st_ino;
    m->uid=st.st_uid;m->gid=st.st_gid;m->mode=st.st_mode;m->links=(uint32_t)st.st_nlink;strcpy(m->name,name);
    if(!resource_issuer_current(p,lock)||!resource_issuer_send(p,m))return 0;
    p->started=1;return 1;
}
static void resource_issuer_tick(struct lx06_resource_issuer *p,int lock) {
    if(p->fd>=0&&p->started&&!p->invalid&&!resource_issuer_current(p,lock))p->invalid=1;
}
/* Cleanup candidates come only from this subreaper's own direct-child
 * list. waitid WNOWAIT verifies each PID is STILL our unreaped child before
 * signalling; it cannot be recycled while this sole wait owner retains it.
 * These are cleanup requests, never a reap/completion certificate. */
static int resource_issuer_signal_children(int sig) {
    char path[96],bytes[16384];snprintf(path,sizeof path,"/proc/self/task/%ld/children",(long)getpid());
    int f=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(f<0)return 0;
    ssize_t n=read(f,bytes,sizeof(bytes)-1);char extra;ssize_t tail=n>=0?read(f,&extra,1):-1;close(f);
    if(n<0||n==(ssize_t)sizeof(bytes)-1||tail!=0||memchr(bytes,0,(size_t)n))return 0;
    bytes[n]=0;char *at=bytes;
    while(*at){
        while(*at==' '||*at=='\n'||*at=='\t')at++;
        if(!*at)break;
        unsigned long pid=0;int digits=0;
        while(*at>='0'&&*at<='9'){if(pid>214748364u)return 0;pid=pid*10+(unsigned)(*at++-'0');digits=1;}
        if(!digits||pid<2||pid>2147483647u||(*at&&*at!=' '&&*at!='\n'&&*at!='\t'))return 0;
        siginfo_t information;memset(&information,0,sizeof information);
        if(waitid(P_PID,(id_t)pid,&information,WEXITED|WNOHANG|WNOWAIT)){
            if(errno==ECHILD)continue;
            return 0;
        }
        if(kill((pid_t)pid,sig)&&errno!=ESRCH)return 0;
    }
    return 1;
}
static int resource_issuer_reaped(struct lx06_resource_issuer *p,int lock) {
    int subreaper=0,status=0;
    if(p->fd<0||!p->started||p->invalid||prctl(PR_GET_CHILD_SUBREAPER,&subreaper)||subreaper!=1)return 0;
    errno=0;pid_t result=waitpid(-1,&status,WNOHANG);int saved=errno;
    return result<0&&saved==ECHILD&&resource_issuer_current(p,lock);
}
/* Distinct, non-inherited OFD on the SAME retained lease inode. After the
 * real owner has reaped its children and closed its own original lease FD,
 * acquiring this observer detects a still-held conflicting lock at this local
 * same-inode observation. It does not prove all raw OFD references vanished
 * or prevent an external unlocked reference from later acquiring a lock.
 * Competition, record/path replacement or close uncertainty yields no cert.
 * It is closed before emission; no assertion of global idle at reader time. */
static int resource_issuer_observer(const struct lx06_resource_issuer *p,int fd,int empty) {
    LX06_SESSION_STATBUF held,named;const lx06_resource_message *m=&p->start;
    if(LX06_RESOURCE_FSTAT(fd,&held)||LX06_SESSION_STAT(LX06_SESSION_FILE,&named)||
       held.st_dev!=named.st_dev||held.st_ino!=named.st_ino||
       (uint64_t)held.st_dev!=m->device||(uint64_t)held.st_ino!=m->inode||
       held.st_uid!=m->uid||held.st_gid!=m->gid||held.st_mode!=m->mode||held.st_nlink!=m->links)return 0;
    return empty?held.st_size==0:held.st_size==(off_t)(sizeof(m->record)+sizeof(m->name));
}
static void resource_issuer_closed(struct lx06_resource_issuer *p,int clean) {
    if(p->fd<0)return;
    if(clean){lx06_resource_message m=p->start;m.kind=LX06_RESOURCE_CLOSED;(void)resource_issuer_send(p,&m);}
    close(p->fd);p->fd=-1;
}

static void resource_issuer_finish(struct lx06_resource_issuer *p,int lock) {
    int proved=resource_issuer_reaped(p,lock),observer=-1;
    if(proved){observer=open(LX06_SESSION_FILE,O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(observer<0||!resource_issuer_observer(p,observer,0))proved=0;}
    int truncated=proved&&ftruncate(lock,0)==0;
    int closed=close(lock)==0,complete=0;
    if(proved&&truncated&&closed&&observer>=0&&flock(observer,LOCK_EX|LOCK_NB)==0)
        complete=resource_issuer_observer(p,observer,1);
    if(observer>=0&&close(observer))complete=0;
    resource_issuer_closed(p,complete);
}

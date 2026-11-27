#define _GNU_SOURCE
#include "session-resource-proof-internal.h"
struct lx06_session_resource_proof {
    int fd,started,complete,failed;pid_t expected;uid_t uid;gid_t gid;
    uint64_t device,inode;char name[LX06_SESSION_NAME_SIZE];lx06_resource_message start;
};
_Static_assert(sizeof(struct lx06_session_resource_proof)<=512,"bounded observer");
int lx06_session_resource_channel(int pair[2]) {
    if(!pair){errno=EINVAL;return -1;}
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair))return -1;
    int on=1;if(setsockopt(pair[0],SOL_SOCKET,SO_PASSCRED,&on,sizeof(on))){int saved=errno;close(pair[0]);close(pair[1]);pair[0]=pair[1]=-1;errno=saved;return -1;}
    return 0;
}
lx06_session_resource_proof *lx06_session_resource_open(int fd,pid_t expected,const char *name) {
    LX06_SESSION_STATBUF st;int on=0;socklen_t n=sizeof(on);
    if(expected<2||!name||!lx06_session_name_valid(name)||!lx06_resource_socket(fd,&st)||
       getsockopt(fd,SOL_SOCKET,SO_PASSCRED,&on,&n)||n!=sizeof(on)||on!=1){errno=EINVAL;return NULL;}
    int flags=fcntl(fd,F_GETFL);if(flags<0||fcntl(fd,F_SETFD,FD_CLOEXEC)||fcntl(fd,F_SETFL,flags|O_NONBLOCK))return NULL;
    lx06_session_resource_proof *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->fd=fd;p->expected=expected;p->uid=getuid();p->gid=getgid();p->device=(uint64_t)st.st_dev;p->inode=(uint64_t)st.st_ino;strcpy(p->name,name);return p;
}
static int resource_message_valid(const lx06_resource_message *m) {
    if(m->magic!=LX06_RESOURCE_MAGIC||m->version!=1||(m->kind!=LX06_RESOURCE_START&&m->kind!=LX06_RESOURCE_CLOSED)||
       m->record.magic!=LX06_SESSION_MAGIC||m->record.supervisor<2||m->record.group<2||m->record.supervisor==m->record.group||
       !m->record.supervisor_ticks||!m->record.group_ticks||m->record.reserved<3||m->record.reserved>1024||
       !m->inode||!S_ISREG(m->mode)||(m->mode&07777)!=0600||m->links!=1||
       !memchr(m->name,0,sizeof(m->name))||!lx06_session_name_valid(m->name))return 0;
    for(unsigned i=0;i<4;i++)if(m->reserved[i])return 0;
    for(size_t i=strlen(m->name)+1;i<sizeof(m->name);i++)if(m->name[i])return 0;
    return 1;
}
int lx06_session_resource_poll(lx06_session_resource_proof *p) {
    if(!p||p->failed)return -1;if(p->complete)return 1;
    LX06_SESSION_STATBUF st;
    if(!lx06_resource_socket(p->fd,&st)||(uint64_t)st.st_dev!=p->device||(uint64_t)st.st_ino!=p->inode){p->failed=1;return -1;}
    for(unsigned batch=0;batch<2;batch++){
        lx06_resource_message m;
        union {struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(4*sizeof(int))];} control;
        memset(&m,0,sizeof(m));memset(&control,0,sizeof(control));struct iovec vector={&m,sizeof(m)};
        struct msghdr msg={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
        ssize_t n=recvmsg(p->fd,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
        if(n<0&&errno==EINTR)return 0;
        int valid=n==(ssize_t)sizeof(m)&&!(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC)),creds=0;struct ucred sender={0};
        for(struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c;c=CMSG_NXTHDR(&msg,c)){
            if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len>=CMSG_LEN(0)){
                size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);int *fds=(int *)CMSG_DATA(c);
                for(size_t i=0;i<count;i++)close(fds[i]);valid=0;
            }else if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_CREDENTIALS&&c->cmsg_len==CMSG_LEN(sizeof(sender))&&!creds){memcpy(&sender,CMSG_DATA(c),sizeof(sender));creds=1;}
            else valid=0;
        }
        if(!valid||!creds||sender.pid!=p->expected||sender.uid!=p->uid||sender.gid!=p->gid||
           !resource_message_valid(&m)||m.record.supervisor!=(uint32_t)p->expected||m.uid!=p->uid||m.gid!=p->gid||strcmp(m.name,p->name)){
            p->failed=1;return -1;
        }
        if(!p->started){
            if(m.kind!=LX06_RESOURCE_START||lx06_process_ticks((unsigned)p->expected)!=m.record.supervisor_ticks){p->failed=1;return -1;}
            p->start=m;p->started=1;
        }else{
            lx06_resource_message expected=p->start;expected.kind=LX06_RESOURCE_CLOSED;
            if(memcmp(&expected,&m,sizeof(m))){p->failed=1;return -1;}
            p->complete=1;return 1;
        }
    }
    return 0;
}
int lx06_session_resource_identity(const lx06_session_resource_proof *p,struct lx06_session_record *out) {
    if(!p||!out||p->failed)return -1;if(!p->started)return 0;*out=p->start.record;return 1;
}
void lx06_session_resource_free(lx06_session_resource_proof *p) {
    if(!p)return;close(p->fd);memset(p,0,sizeof(*p));free(p);
}

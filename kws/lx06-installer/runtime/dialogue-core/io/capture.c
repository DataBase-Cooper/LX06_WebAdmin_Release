/* Original passive processed-mic LXA1 wire protocol; no official audio control.
 * One nonblocking subscription replaces a reader child in the core poll loop. */
#define _GNU_SOURCE
#include "io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "compat.h"
struct dc_io_capture {
    int fd,lock_fd,dir_fd,seen;
    enum dc_io_capture_mode mode;
    uint64_t previous,previous_birth;
    uint32_t previous_pid;
    uint64_t last_ns;
    char socket_name[40];
    dc_io_stat node;
};
static uint64_t now_ns(void){struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return 0;return (uint64_t)ts.tv_sec*1000000000u+(unsigned long)ts.tv_nsec;}
static int trusted_file(int fd,int regular){dc_io_stat st;return !dc_io_fstat(fd,&st)&&st.st_uid==0&&!(st.st_mode&0022)&&(!regular||(S_ISREG(st.st_mode)&&st.st_nlink==1));}
dc_io_capture *dc_io_capture_open(const char *directory,const char *name){return dc_io_capture_open_mode(directory,name,DC_IO_CAPTURE_LXA1);}
dc_io_capture *dc_io_capture_open_mode(const char *directory,const char *name,enum dc_io_capture_mode mode) {
    if(!directory||!name||!*name||strlen(name)>32||strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=strlen(name)){errno=EINVAL;return NULL;}
    if(mode!=DC_IO_CAPTURE_LXA1&&mode!=DC_IO_CAPTURE_LXA2){errno=EINVAL;return NULL;}
    struct sockaddr_un address={.sun_family=AF_UNIX};
    int written=mode==DC_IO_CAPTURE_LXA2?snprintf(address.sun_path,sizeof(address.sun_path),"%s/account.a2",directory):snprintf(address.sun_path,sizeof(address.sun_path),"%s/%s.sock",directory,name);
    if(written<0||(size_t)written>=sizeof(address.sun_path)){errno=ENAMETOOLONG;return NULL;}
    dc_io_capture *c=calloc(1,sizeof(*c));if(!c)return NULL;
    c->fd=c->lock_fd=c->dir_fd=-1;c->last_ns=now_ns();c->mode=mode;
    c->dir_fd=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(c->dir_fd<0||!trusted_file(c->dir_fd,0)){errno=EPERM;goto fail;}
    if(mode==DC_IO_CAPTURE_LXA2)strcpy(c->socket_name,"account.a2");
    else snprintf(c->socket_name,sizeof(c->socket_name),"%s.sock",name);
    char lock_name[40];
    if(mode==DC_IO_CAPTURE_LXA2)strcpy(lock_name,"account.a2.lock");
    else snprintf(lock_name,sizeof(lock_name),"%s.lock",name);
    c->lock_fd=openat(c->dir_fd,lock_name,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if(c->lock_fd<0||!trusted_file(c->lock_fd,1)){errno=EPERM;goto fail;}
    if(flock(c->lock_fd,LOCK_EX|LOCK_NB))goto fail;
    dc_io_stat old;
    if(!dc_io_fstatat(c->dir_fd,c->socket_name,&old,AT_SYMLINK_NOFOLLOW)) {
        if(!S_ISSOCK(old.st_mode)||old.st_uid!=0){errno=EPERM;goto fail;}
        int probe=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
        if(probe<0)goto fail;
        int r=connect(probe,(struct sockaddr *)&address,sizeof(address)),saved=errno;close(probe);
        if(!r){errno=EBUSY;goto fail;}
        if(saved!=ENOENT&&saved!=ECONNREFUSED){errno=saved;goto fail;}
        dc_io_stat again;
        if(!dc_io_fstatat(c->dir_fd,c->socket_name,&again,AT_SYMLINK_NOFOLLOW)&&
           (old.st_dev!=again.st_dev||old.st_ino!=again.st_ino)){errno=ESTALE;goto fail;}
        if(unlinkat(c->dir_fd,c->socket_name,0)&&errno!=ENOENT)goto fail;
    } else if(errno!=ENOENT)goto fail;
    c->fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(c->fd<0)goto fail;
    int on=1,buffer=65536;
    if(setsockopt(c->fd,SOL_SOCKET,SO_PASSCRED,&on,sizeof(on)))goto fail;
    (void)setsockopt(c->fd,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer));
    if(bind(c->fd,(struct sockaddr *)&address,sizeof(address)))goto fail;
    if(dc_io_fstatat(c->dir_fd,c->socket_name,&c->node,AT_SYMLINK_NOFOLLOW))goto fail;
    if(fchmodat(c->dir_fd,c->socket_name,0600,0))goto fail;
    return c;
fail:
    {int saved=errno;dc_io_capture_close(c);errno=saved;return NULL;}
}
int dc_io_capture_fd(const dc_io_capture *c){return c?c->fd:-1;}
static uint16_t u16(const unsigned char*p){return (uint16_t)((unsigned)p[0]|((unsigned)p[1]<<8));}
static uint32_t u32(const unsigned char*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t u64(const unsigned char*p){return (uint64_t)u32(p)|((uint64_t)u32(p+4)<<32);}
int dc_io_capture_next(dc_io_capture *c,dc_io_capture_frame *frame) {
    if(!c||!frame)return -EINVAL;
    unsigned char packet[16+DC_IO_CAPTURE_MAX];
    union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(struct ucred))];} control;
    struct iovec iov={&packet,sizeof(packet)};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
    ssize_t n=recvmsg(c->fd,&msg,MSG_DONTWAIT|MSG_TRUNC);
    if(n<0)return errno==EAGAIN||errno==EWOULDBLOCK?0:-errno;
    if(msg.msg_flags&(MSG_TRUNC|MSG_CTRUNC))return -EMSGSIZE;
    struct ucred *peer=NULL;
    for(struct cmsghdr *h=CMSG_FIRSTHDR(&msg);h;h=CMSG_NXTHDR(&msg,h))
        if(h->cmsg_level==SOL_SOCKET&&h->cmsg_type==SCM_CREDENTIALS&&h->cmsg_len==CMSG_LEN(sizeof(struct ucred)))peer=(struct ucred *)CMSG_DATA(h);
    if(!peer||peer->uid!=0||peer->pid<1)return -EPERM;
    size_t header=16,bytes=0;uint64_t sequence=0,birth=0,available=0;uint32_t pid=0,origin=0,flags=0,lost=0;
    if(c->mode==DC_IO_CAPTURE_LXA1) {
        if(n<16||u32(packet)!=0x3141584cU||u32(packet+8)!=16000)return -EPROTO;
        bytes=u32(packet+12);sequence=u32(packet+4);
        if(!bytes||(bytes&1)||bytes>DC_IO_CAPTURE_MAX||n!=(ssize_t)(header+bytes))return -EPROTO;
    } else {
        header=56;
        if(n<56||u32(packet)!=0x3241584cU||u16(packet+4)!=2||u16(packet+6)!=56||u32(packet+12)!=16000||u32(packet+28))return -EPROTO;
        uint32_t samples=u32(packet+16);if(samples!=160&&samples!=320&&samples!=480)return -EPROTO;
        bytes=(size_t)samples*2;if(u32(packet+8)!=56+bytes||n!=(ssize_t)(56+bytes))return -EPROTO;
        origin=u32(packet+20);pid=u32(packet+24);birth=u64(packet+32);sequence=u64(packet+40);available=u64(packet+48);
        /* Preserve invalid stamp metadata and PCM for the endpoint's explicit
         * fallback; never turn receiver time into a producer capture time. */
        if(!pid||pid!=(uint32_t)peer->pid||!birth||!available||origin<1||origin>3)flags|=DC_IO_CAPTURE_METADATA_INVALID;
    }
    if(!c->seen)flags|=DC_IO_CAPTURE_READY;
    else {uint64_t delta=c->mode==DC_IO_CAPTURE_LXA1?(uint32_t)(sequence-c->previous):sequence-c->previous;
        /* Continuity diagnostics must not discard well-framed speech. The
         * endpoint sees duplicate/restarted/untrusted stamps and falls back;
         * it never receives fabricated continuity or receiver-time stamps. */
        if(!delta || (c->mode==DC_IO_CAPTURE_LXA2&&(pid!=c->previous_pid||birth!=c->previous_birth)) ||
           delta>=(c->mode==DC_IO_CAPTURE_LXA1?UINT64_C(0x80000000):UINT64_C(0x8000000000000000)))flags|=DC_IO_CAPTURE_SEQUENCE_RESET;
        else if(delta!=1){flags|=DC_IO_CAPTURE_GAP;lost=delta-1>UINT32_MAX?UINT32_MAX:(uint32_t)(delta-1);}}
    c->seen=1;c->previous=sequence;c->previous_pid=pid;c->previous_birth=birth;c->last_ns=now_ns();
    *frame=(dc_io_capture_frame){.sequence=sequence,.producer_birth_ticks=birth,.available_mono_ms=available,
        .producer_pid=pid,.origin=origin,.rate=16000,.bytes=(uint32_t)bytes,.flags=flags,.lost=lost,.mode=c->mode,.monotonic_ns=c->last_ns};
    memcpy(frame->pcm,packet+header,bytes);return 1;
}
int dc_io_capture_stalled(const dc_io_capture *c,uint64_t stamp){return c&&stamp>=c->last_ns&&stamp-c->last_ns>=3000000000u;}
void dc_io_capture_close(dc_io_capture *c) {
    if(!c)return;
    if(c->dir_fd>=0&&c->node.st_ino){dc_io_stat current;
        if(!dc_io_fstatat(c->dir_fd,c->socket_name,&current,AT_SYMLINK_NOFOLLOW)&&
           current.st_dev==c->node.st_dev&&current.st_ino==c->node.st_ino)(void)unlinkat(c->dir_fd,c->socket_name,0);}
    if(c->fd>=0)close(c->fd);
    if(c->lock_fd>=0)close(c->lock_fd);
    if(c->dir_fd>=0)close(c->dir_fd);
    free(c);
}

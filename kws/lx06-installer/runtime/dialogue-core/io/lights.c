#define _GNU_SOURCE
#include "io.h"
#include "../../../hooks/xiaoai/dialog-light.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "compat.h"
struct dc_io_lights {
    pthread_t worker;pthread_mutex_t lock;
    int fd,wake_fd,quit,pending,closed;
    dc_tag tag;uint64_t sequence;
    enum dc_state state;
    atomic_uint_fast64_t sent,failed;
};
static int light_code(enum dc_state state) {
    switch(state) {
    case DC_CLOSED:return LX06_LIGHT_END;
    case DC_CONNECTING:return LX06_LIGHT_WAKE;
    case DC_LISTENING:return LX06_LIGHT_LISTENING;
    case DC_THINKING:case DC_CLOSING:return LX06_LIGHT_THINKING;
    case DC_SPEAKING:return LX06_LIGHT_SPEAKING;
    case DC_INTERRUPTED:return LX06_LIGHT_INTERRUPTED;
    default:return -1;
    }
}
static int trusted_socket(int fd,const char *identity) {
    if(fd<3||!identity)return 0;
    char path[64],actual[96];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(path,actual,sizeof(actual)-1);if(n<=0)return 0;actual[n]=0;
    if(strncmp(actual,"socket:[",8)||strcmp(actual,identity))return 0;
    int type=0;dc_io_stat st;struct ucred peer;socklen_t length=sizeof(type),pl=sizeof(peer);
    return !dc_io_fstat(fd,&st)&&S_ISSOCK(st.st_mode)&&
        !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length)&&type==SOCK_DGRAM&&
        !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&pl)&&peer.uid==0&&peer.pid>0;
}
static void *light_worker(void *arg) {
    dc_io_lights *l=arg;
    /* Linux nice is per thread. This does not reduce the microphone/player or
     * change a process-wide scheduling policy. Failure leaves default priority. */
    (void)setpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid),5);
    for(;;) {
        struct pollfd wait={l->wake_fd,POLLIN,0};(void)poll(&wait,1,30);
        uint64_t count;for(unsigned budget=0;budget<8&&read(l->wake_fd,&count,sizeof(count))>0;budget++){}
        pthread_mutex_lock(&l->lock);
        if(l->pending) {
            /* The trusted transport is a nonblocking one-byte datagram, never
             * an asynchronous RPC or a shell. Hold this tiny critical section
             * through send: a newer epoch cannot be accepted before this send
             * is ordered. No old END is emitted after a new epoch is accepted. */
            unsigned char code=(unsigned char)light_code(l->state);
            ssize_t n=send(l->fd,&code,1,MSG_DONTWAIT|MSG_NOSIGNAL);
            if(n==1){l->pending=0;atomic_fetch_add(&l->sent,1);}
            else {atomic_fetch_add(&l->failed,1);if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)l->pending=0;}
        }
        /* destroy does not manufacture END. It does give an already accepted
         * latest state one final nonblocking send before the worker exits, so
         * an explicit CLOSED is not silently discarded by immediate cleanup. */
        int quit=l->quit;
        pthread_mutex_unlock(&l->lock);
        if(quit)break;
    }
    return NULL;
}
dc_io_lights *dc_io_lights_create(int fd,const char *identity) {
    if(!trusted_socket(fd,identity)){errno=EPERM;return NULL;}
    dc_io_lights *l=calloc(1,sizeof(*l));if(!l)return NULL;l->fd=l->wake_fd=-1;
    if(pthread_mutex_init(&l->lock,NULL)){free(l);errno=ENOMEM;return NULL;}
    l->fd=fcntl(fd,F_DUPFD_CLOEXEC,3);l->wake_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
    if(l->fd<0||l->wake_fd<0)goto fail;
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,128u*1024u);
    int r=pthread_create(&l->worker,&attr,light_worker,l);pthread_attr_destroy(&attr);
    if(r){errno=r;goto fail;}return l;
fail:
    if(l->fd>=0)close(l->fd);
    if(l->wake_fd>=0)close(l->wake_fd);
    pthread_mutex_destroy(&l->lock);free(l);return NULL;
}
int dc_io_lights_set(dc_io_lights *l,dc_tag tag,uint64_t seq,enum dc_state state) {
    if(!l||!tag.session_epoch||!seq||light_code(state)<0)return -EINVAL;
    pthread_mutex_lock(&l->lock);int r=0;
    if(l->quit)r=-ESHUTDOWN;
    else if(tag.session_epoch<l->tag.session_epoch)r=-ESTALE;
    else if(tag.session_epoch==l->tag.session_epoch&&(seq<=l->sequence||l->closed))r=-ESTALE;
    else if(tag.session_epoch>l->tag.session_epoch&&state==DC_CLOSED)r=-ESTALE;
    else {l->tag=tag;l->sequence=seq;l->state=state;l->closed=state==DC_CLOSED;l->pending=1;}
    pthread_mutex_unlock(&l->lock);
    if(!r){uint64_t one=1;(void)write(l->wake_fd,&one,sizeof(one));}return r;
}
void dc_io_lights_destroy(dc_io_lights *l) {
    if(!l)return;
    pthread_mutex_lock(&l->lock);l->quit=1;pthread_mutex_unlock(&l->lock);
    uint64_t one=1;(void)write(l->wake_fd,&one,sizeof(one));pthread_join(l->worker,NULL);
    close(l->fd);close(l->wake_fd);pthread_mutex_destroy(&l->lock);free(l);
}
uint64_t dc_io_lights_sent(const dc_io_lights *l){return l?atomic_load(&l->sent):0;}
uint64_t dc_io_lights_failed(const dc_io_lights *l){return l?atomic_load(&l->failed):0;}

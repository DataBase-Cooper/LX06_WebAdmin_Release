#define _GNU_SOURCE
#include "io.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define EVENTS 64u
struct dc_io_player {
    dc_io_playback_backend backend;
    dc_io_reference reference;
    pthread_t worker;
    pthread_mutex_t lock;
    atomic_uint revision;
    int wake_fd,stop_fd,event_fd,quit,active,sealed,stop_pending,drop_error;
    dc_tag tag,stop_tag;
    unsigned rate;
    unsigned char pcm[DC_IO_PCM_CAPACITY];
    size_t head,tail,used;
    dc_io_event events[EVENTS];
    unsigned event_head,event_used;
};
static uint64_t clock_ns(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts))return 0;
    return (uint64_t)ts.tv_sec*1000000000u+(unsigned long)ts.tv_nsec;
}
static void signal_fd(int fd) {uint64_t one=1;(void)write(fd,&one,sizeof(one));}
static void drain_fd(int fd) {uint64_t value;for(unsigned i=0;i<8&&read(fd,&value,sizeof(value))>0;i++){}}
static void emit(dc_io_player *p,enum dc_io_event_type type,dc_tag tag,int error) {
    pthread_mutex_lock(&p->lock);
    if(p->event_used==EVENTS) {
        /* Preserve an observable overflow rather than silently losing stop or
         * device failure. The core must treat this ERROR as a failed session. */
        unsigned last=(p->event_head+EVENTS-1)%EVENTS;
        p->events[last]=(dc_io_event){DC_IO_ERROR,tag,clock_ns(),ENOBUFS};
    } else {
        unsigned slot=(p->event_head+p->event_used++)%EVENTS;
        p->events[slot]=(dc_io_event){type,tag,clock_ns(),error};
        signal_fd(p->event_fd);
    }
    pthread_mutex_unlock(&p->lock);
}
static void backend_close(dc_io_player *p,int *opened,int drop) {
    if(*opened) {
        int r=drop?p->backend.drop(p->backend.context):0;
        if(r<0) {pthread_mutex_lock(&p->lock);p->drop_error=-r;pthread_mutex_unlock(&p->lock);}
        p->backend.close(p->backend.context);*opened=0;
    }
}
static void fail_tag(dc_io_player *p,dc_tag tag,int error) {
    pthread_mutex_lock(&p->lock);
    if(p->active&&dc_tag_equal(p->tag,tag)) {p->active=0;p->used=p->head=p->tail=0;}
    pthread_mutex_unlock(&p->lock);
    emit(p,DC_IO_ERROR,tag,error>0?error:EIO);
}
static void *play_worker(void *arg) {
    dc_io_player *p=arg;
    int opened=0,first=0,retries=0;
    dc_tag tag={0};
    uint64_t accepted_ns=0;
    uint64_t operation_counter=0,operation=0;
    unsigned rate=0,revision=0;
    int16_t chunk[480]; /* <=10ms at the largest permitted 48kHz rate */
    size_t local_frames=0,local_at=0;
    for(;;) {
        struct pollfd fds[2+DC_IO_BACKEND_POLL_MAX]={{p->stop_fd,POLLIN,0},{p->wake_fd,POLLIN,0}};
        pthread_mutex_lock(&p->lock);
        int has_pcm=local_frames||p->used,finishing=p->sealed;
        pthread_mutex_unlock(&p->lock);
        /* ALSA is usually writable while no cloud PCM is available. Including
         * POLLOUT then would spin at 100% CPU. Data/stop eventfds wake us; a
         * sealed hardware tail is checked with a bounded timer instead. */
        int n=opened&&has_pcm?p->backend.pollfds(p->backend.context,fds+2,DC_IO_BACKEND_POLL_MAX):0;
        if(n<0 || n>(int)DC_IO_BACKEND_POLL_MAX) {
            backend_close(p,&opened,1);local_frames=local_at=0;fail_tag(p,tag,n<0?-n:EOVERFLOW);n=0;
        }
        int polled=poll(fds,(nfds_t)(2+n),has_pcm?10:finishing?5:50);
        if(polled<0&&errno!=EINTR) {int error=errno;backend_close(p,&opened,1);fail_tag(p,tag,error);}
        /* Stop always precedes ordinary wake/data readiness. */
        drain_fd(p->stop_fd);drain_fd(p->wake_fd);
        pthread_mutex_lock(&p->lock);
        int quit=p->quit,active=p->active,pending=p->stop_pending;
        dc_tag desired=p->tag,stop_tag=p->stop_tag;
        unsigned desired_rate=p->rate,observed=atomic_load_explicit(&p->revision,memory_order_acquire);
        pthread_mutex_unlock(&p->lock);
        if(quit) {backend_close(p,&opened,1);break;}
        if(pending || (opened&&(!active || observed!=revision || !dc_tag_equal(tag,desired)))) {
            backend_close(p,&opened,1);local_frames=local_at=0;first=0;
            if(pending) {
                pthread_mutex_lock(&p->lock);
                /* begin refuses while this acknowledgement is pending. */
                p->stop_pending=0;
                int drop_error=p->drop_error;
                pthread_mutex_unlock(&p->lock);
                emit(p,drop_error?DC_IO_STOP_FAILED:DC_IO_STOPPED,stop_tag,drop_error);
            }
            continue;
        }
        if(!active)continue;
        if(!opened) {
            tag=desired;rate=desired_rate;revision=observed;first=0;retries=0;
            local_frames=local_at=0;accepted_ns=0;
            int r=p->backend.open(p->backend.context,rate);
            if(r<0) {fail_tag(p,tag,-r);continue;}
            opened=1;
            if(atomic_load_explicit(&p->revision,memory_order_acquire)!=revision)continue;
        }
        if(!local_frames) {
            pthread_mutex_lock(&p->lock);
            int current=p->active&&dc_tag_equal(p->tag,tag)&&atomic_load_explicit(&p->revision,memory_order_acquire)==revision;
            size_t bytes=current?p->used:0,limit=(rate/100u)*sizeof(int16_t);
            if(bytes>limit)bytes=limit;
            if(bytes) {
                size_t first_bytes=DC_IO_PCM_CAPACITY-p->tail;
                if(first_bytes>bytes)first_bytes=bytes;
                memcpy(chunk,p->pcm+p->tail,first_bytes);
                memcpy((unsigned char *)chunk+first_bytes,p->pcm,bytes-first_bytes);
                p->tail=(p->tail+bytes)%DC_IO_PCM_CAPACITY;p->used-=bytes;
                local_frames=bytes/2;local_at=0;
                /* Exhaustion disables span joining rather than reusing an ID. */
                operation=operation_counter==UINT64_MAX?0:++operation_counter;
            }
            int sealed=current&&p->sealed;
            pthread_mutex_unlock(&p->lock);
            if(!current)continue;
            if(!local_frames) {
                if(sealed) {
                    long delay=0;int r=p->backend.delay(p->backend.context,&delay);
                    if(r==-EPIPE || (r>=0&&delay<=0)) {
                        backend_close(p,&opened,0);
                        pthread_mutex_lock(&p->lock);
                        int done=p->active&&dc_tag_equal(p->tag,tag)&&atomic_load_explicit(&p->revision,memory_order_acquire)==revision;
                        if(done){p->active=0;p->sealed=0;}
                        pthread_mutex_unlock(&p->lock);
                        if(done)emit(p,DC_IO_DRAINED,tag,0);
                    } else if(r<0&&r!=-EAGAIN&&r!=-EINTR) {backend_close(p,&opened,1);fail_tag(p,tag,-r);}
                }
                continue;
            }
        }
        if(atomic_load_explicit(&p->revision,memory_order_acquire)!=revision) {local_frames=local_at=0;continue;}
        ssize_t written=p->backend.write(p->backend.context,chunk+local_at,local_frames-local_at);
        pthread_mutex_lock(&p->lock);
        int valid=!p->quit && p->active && dc_tag_equal(p->tag,tag) &&
            atomic_load_explicit(&p->revision,memory_order_acquire)==revision;
        if(valid && written>0 && (size_t)written<=local_frames-local_at && p->reference.accepted)
            p->reference.accepted(p->reference.context,tag,chunk+local_at,(size_t)written,rate,operation,local_at);
        pthread_mutex_unlock(&p->lock);
        /* A stop racing with one nonblocking <=10ms write is immediately
         * followed by drop; no local remainder may survive invalidation. */
        if(!valid || atomic_load_explicit(&p->revision,memory_order_acquire)!=revision) {local_frames=local_at=0;continue;}
        if(written>0&&(size_t)written<=local_frames-local_at) {
            uint64_t accepted=clock_ns();
            if(!first){first=1;emit(p,DC_IO_FIRST_PCM,tag,0);}
            else if(accepted>=accepted_ns&&accepted-accepted_ns>=UINT64_C(200000000))emit(p,DC_IO_PCM_RESUMED,tag,0);
            accepted_ns=accepted;
            local_at+=(size_t)written;retries=0;
            if(local_at==local_frames)local_frames=local_at=0;
        } else if(written==-EAGAIN||written==-EINTR||written==0)continue;
        else if((written==-EPIPE||written==-ESTRPIPE)&&retries++<4) {
            /* Recovery is an observable discontinuity even within a chunk. */
            operation=operation_counter==UINT64_MAX?0:++operation_counter;
            int r=p->backend.prepare(p->backend.context);
            if(r<0){backend_close(p,&opened,1);fail_tag(p,tag,-r);}
        } else {backend_close(p,&opened,1);local_frames=local_at=0;fail_tag(p,tag,written<0?(int)-written:EIO);}
    }
    return NULL;
}
dc_io_player *dc_io_player_create_with_reference(const dc_io_playback_backend *backend,const dc_io_reference *reference) {
    if(!backend||!backend->open||!backend->pollfds||!backend->write||!backend->delay||!backend->prepare||!backend->drop||!backend->close){errno=EINVAL;return NULL;}
    dc_io_player *p=calloc(1,sizeof(*p));if(!p)return NULL;
    p->wake_fd=p->stop_fd=p->event_fd=-1;p->backend=*backend;if(reference)p->reference=*reference;
    if(pthread_mutex_init(&p->lock,NULL)){free(p);errno=ENOMEM;return NULL;}
    p->wake_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    p->stop_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    p->event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC|EFD_SEMAPHORE);
    if(p->wake_fd<0||p->stop_fd<0||p->event_fd<0)goto fail;
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256u*1024u);
    int r=pthread_create(&p->worker,&attr,play_worker,p);pthread_attr_destroy(&attr);
    if(r){errno=r;goto fail;}return p;
fail:
    if(p->wake_fd>=0)close(p->wake_fd);
    if(p->stop_fd>=0)close(p->stop_fd);
    if(p->event_fd>=0)close(p->event_fd);
    pthread_mutex_destroy(&p->lock);free(p);return NULL;
}
dc_io_player *dc_io_player_create(const dc_io_playback_backend *backend) {
    return dc_io_player_create_with_reference(backend,NULL);
}
int dc_io_player_begin(dc_io_player *p,dc_tag tag,unsigned rate) {
    if(!p||!tag.session_epoch||!tag.reply_generation||(rate!=8000&&rate!=16000&&rate!=24000&&rate!=48000))return -EINVAL;
    pthread_mutex_lock(&p->lock);
    int r=0;
    if(p->quit)r=-ESHUTDOWN;
    else if(p->drop_error)r=-p->drop_error;
    else if(p->stop_pending)r=-EAGAIN;
    else if(dc_tag_equal(tag,p->stop_tag))r=-ESTALE;
    else if(p->active)r=dc_tag_equal(tag,p->tag)&&rate==p->rate?0:-EBUSY;
    else {if(p->reference.reset)p->reference.reset(p->reference.context);p->tag=tag;p->rate=rate;p->active=1;p->sealed=0;p->used=p->head=p->tail=0;atomic_fetch_add_explicit(&p->revision,1,memory_order_release);}
    pthread_mutex_unlock(&p->lock);if(!r)signal_fd(p->wake_fd);return r;
}
int dc_io_player_write(dc_io_player *p,dc_tag tag,const void *data,size_t bytes) {
    if(!p||(!data&&bytes)||(bytes&1)||bytes>DC_IO_PCM_WRITE_MAX)return -EINVAL;
    pthread_mutex_lock(&p->lock);int r=0;
    if(!p->active||!dc_tag_equal(p->tag,tag))r=-ESTALE;
    else if(p->sealed)r=-EPIPE;
    else if(bytes>DC_IO_PCM_CAPACITY-p->used)r=-EAGAIN;
    else {size_t first=DC_IO_PCM_CAPACITY-p->head;if(first>bytes)first=bytes;
        if(first)memcpy(p->pcm+p->head,data,first);
        if(bytes>first)memcpy(p->pcm,(const unsigned char *)data+first,bytes-first);
        p->head=(p->head+bytes)%DC_IO_PCM_CAPACITY;p->used+=bytes;}
    pthread_mutex_unlock(&p->lock);if(!r)signal_fd(p->wake_fd);return r;
}
int dc_io_player_finish(dc_io_player *p,dc_tag tag) {
    if(!p)return -EINVAL;
    pthread_mutex_lock(&p->lock);
    int r=p->active&&dc_tag_equal(p->tag,tag)?0:-ESTALE;if(!r)p->sealed=1;
    pthread_mutex_unlock(&p->lock);if(!r)signal_fd(p->wake_fd);return r;
}
int dc_io_player_stop(dc_io_player *p,dc_tag tag) {
    if(!p)return -EINVAL;
    pthread_mutex_lock(&p->lock);int r=0;
    if(p->active&&!dc_tag_equal(p->tag,tag))r=-ESTALE;
    else if(dc_tag_equal(p->stop_tag,tag))r=0;
    else if(!p->active||!dc_tag_equal(p->tag,tag))r=-ESTALE;
    else {
        atomic_fetch_add_explicit(&p->revision,1,memory_order_release);
        if(p->reference.reset)p->reference.reset(p->reference.context);
        p->stop_tag=tag;p->stop_pending=1;p->active=0;p->sealed=0;p->used=p->head=p->tail=0;
    }
    pthread_mutex_unlock(&p->lock);if(!r)signal_fd(p->stop_fd);return r;
}
size_t dc_io_player_queued(dc_io_player *p) {if(!p)return 0;pthread_mutex_lock(&p->lock);size_t n=p->used;pthread_mutex_unlock(&p->lock);return n;}
int dc_io_player_event_fd(const dc_io_player *p){return p?p->event_fd:-1;}
int dc_io_player_next_event(dc_io_player *p,dc_io_event *event) {
    if(!p||!event)return -EINVAL;
    pthread_mutex_lock(&p->lock);
    if(!p->event_used){pthread_mutex_unlock(&p->lock);return 0;}
    *event=p->events[p->event_head];p->event_head=(p->event_head+1)%EVENTS;--p->event_used;
    uint64_t one;(void)read(p->event_fd,&one,sizeof(one));pthread_mutex_unlock(&p->lock);return 1;
}
void dc_io_player_destroy(dc_io_player *p) {
    if(!p)return;
    pthread_mutex_lock(&p->lock);p->quit=1;atomic_fetch_add_explicit(&p->revision,1,memory_order_release);pthread_mutex_unlock(&p->lock);
    signal_fd(p->stop_fd);pthread_join(p->worker,NULL);
    close(p->wake_fd);close(p->stop_fd);close(p->event_fd);pthread_mutex_destroy(&p->lock);free(p);
}

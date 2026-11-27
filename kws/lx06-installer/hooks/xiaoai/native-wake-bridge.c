/* LX06 stock ARM wake bridge. The xaudio confirmed keyword callback is an
 * arbitration candidate. Script routes consume the stock local-winner result.
 * Local aliases retain their audio-epoch admission and explicit stock handoff.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "session-owner.h"
#include "session-epoch.h"
#include "official-bind.h"
#ifndef LX06_OFFICIAL_FILE
#define LX06_OFFICIAL_FILE "/data/wakeword/official_xiaoai"
#endif
#ifndef LX06_NATIVE_REQUEST
#define LX06_NATIVE_REQUEST "/tmp/lx06-native-request"
#endif
#ifndef LX06_NATIVE_EVENT
#define LX06_NATIVE_EVENT "/tmp/lx06-native-wake"
#endif
#ifdef __arm__
__asm__(".symver dlsym,dlsym@GLIBC_2.4");
#endif
typedef void (*wake_callback)(void *, unsigned, float);
typedef void (*register_fn)(void *, wake_callback, void *);
static wake_callback callback;
static void *callback_context;
static void *callback_engine;
static int callback_ambiguous;
static pthread_mutex_t callback_registration_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned wake_serial, poll_frames;
/* WUW ABI: flags, PCM, bytes, offset, extra; confirmed by do_suspect_callback
 * and the registered stock mipns callback. The latter consumes the first three. */
typedef void (*wuw_callback)(unsigned,const void *,int,unsigned,void *);
typedef void (*asr_callback)(void *,const void *,int);
#if defined(__arm__) || defined(LX06_NATIVE_BIND_SELFTEST)
static int mdspeech_hold_wuw(unsigned flags,const void *pcm,int bytes,unsigned offset);
static void mdspeech_clear_wuw(void);
#endif
static wuw_callback upload_wuw;
static asr_callback upload_asr;
static int stream_source; /* 0=disabled/no wake, 1=official, 2=local */
static int stream_allowed = 1; /* Preserve non-KWS stock ASR/multi-round starts. */
static int official_pending;
static int script_active;
static pthread_mutex_t deferred_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char deferred_pcm[96000];
static int deferred_bytes;
static unsigned deferred_flags, deferred_offset;
static float deferred_angle;
static unsigned pending_polls;
static void discard_official_pending(void) {
    deferred_bytes=0;
    __atomic_store_n(&official_pending,0,__ATOMIC_RELEASE);
}
/* Stock wake and WUW callbacks can overlap in this process. Join its
 * short-lived reservation rather than treating its flock as a script owner.
 * A negative user count means the descriptor is being opened or closed. */
static int stock_lease_users;
static int stock_lease_fd;
static int stock_lease_take(void) {
    int count=__atomic_load_n(&stock_lease_users,__ATOMIC_ACQUIRE);
    for(;;) {
        if(count<0 || count==INT_MAX) return 0;
        int next=count ? count+1 : -1;
        if(!__atomic_compare_exchange_n(&stock_lease_users,&count,next,0,
                                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) continue;
        if(count) return 1;
        int fd=lx06_session_take_idle();
        stock_lease_fd=fd;
        __atomic_store_n(&stock_lease_users,fd>=0 ? 1 : 0,__ATOMIC_RELEASE);
        return fd>=0;
    }
}
static void stock_lease_release(void) {
    int saved=errno, count=__atomic_load_n(&stock_lease_users,__ATOMIC_ACQUIRE);
    for(;;) {
        int next=count==1 ? -1 : count-1;
        if(!__atomic_compare_exchange_n(&stock_lease_users,&count,next,0,
                                        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) continue;
        if(count==1) {
            while(flock(stock_lease_fd,LOCK_UN)<0 && errno==EINTR) {}
            close(stock_lease_fd);
            __atomic_store_n(&stock_lease_users,0,__ATOMIC_RELEASE);
        }
        errno=saved;
        return;
    }
}
static void stock_epoch_advance(void) {
    if(lx06_idle_epoch_changed(stock_lease_fd)==0) return;
    /* The installed assistant can still start when the local epoch store
     * fails. Withdraw the old epoch while the stock reservation excludes it. */
    (void)unlink(LX06_IDLE_EPOCH_FILE);
    syslog(LOG_WARNING,"lx06-native: stock wake invalidated local epoch");
}
static int official_enabled(void) {
    char value='1';
    int fd=open(LX06_OFFICIAL_FILE,O_RDONLY|O_CLOEXEC);
    if(fd>=0) { (void)read(fd,&value,1); close(fd); }
    return value!='0';
}
static void forward_asr(void *context,const void *pcm,int bytes) {
    asr_callback fn=__atomic_load_n(&upload_asr,__ATOMIC_ACQUIRE);
    if(fn && !__atomic_load_n(&official_pending,__ATOMIC_ACQUIRE) &&
       __atomic_load_n(&stream_allowed,__ATOMIC_ACQUIRE) &&
       !__atomic_load_n(&script_active,__ATOMIC_ACQUIRE))
        fn(context,pcm,bytes);
}
static unsigned char history[96000], wake_pcm[96000];
static unsigned history_pos, history_used;
#define NATIVE_EPOCH_SAMPLES 320u
#define NATIVE_EPOCH_BYTES (NATIVE_EPOCH_SAMPLES * 2u)
#define NATIVE_EPOCH_MIN_BLOCKS 30u /* 600 ms at 16 kHz */
#define NATIVE_EPOCH_MAX_AGE_MS 3000u
#define NATIVE_EPOCH_MAX_WORD_SAMPLES 40000u
/* A valid request can arrive up to 3 s after the *end* of its word.
 * Retain its beginning as well, including a partial boundary frame. */
#define NATIVE_EPOCH_BLOCKS ((NATIVE_EPOCH_MAX_WORD_SAMPLES + 16u * NATIVE_EPOCH_MAX_AGE_MS) / NATIVE_EPOCH_SAMPLES + 1u)
static pthread_mutex_t epoch_history_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    unsigned char source[LX06_IDLE_EPOCH_BYTES], epoch[LX06_IDLE_EPOCH_BYTES];
    uint64_t available[NATIVE_EPOCH_BLOCKS], first_sample;
    unsigned oldest, count;
} epoch_history;

static int nonzero_identity(const unsigned char token[LX06_IDLE_EPOCH_BYTES]) {
    static const unsigned char zero[LX06_IDLE_EPOCH_BYTES];
    return token && memcmp(token,zero,sizeof(zero));
}

static int diagnostic_enabled(void) {
    const char *mode=getenv("LX06_NATIVE_DIAGNOSTIC");
    return mode && !strcmp(mode,"1");
}

/* The capture issuer calls this only for a complete, qualified KWE2 block.
 * Retain the published positions and timestamps only while they are contiguous. */
void lx06_native_audio_epoch(const void *pcm,const unsigned char source[LX06_IDLE_EPOCH_BYTES],
                             const unsigned char epoch[LX06_IDLE_EPOCH_BYTES],
                             uint64_t first_sample,uint64_t available_ms) {
    if(pthread_mutex_trylock(&epoch_history_lock)) return;
    if(!pcm || !nonzero_identity(source) || !nonzero_identity(epoch) || !available_ms ||
       first_sample>UINT64_MAX-NATIVE_EPOCH_SAMPLES) {
        epoch_history.count=0;
        pthread_mutex_unlock(&epoch_history_lock);
        return;
    }
    unsigned pos=(epoch_history.oldest+epoch_history.count)%NATIVE_EPOCH_BLOCKS;
    unsigned last=(pos+NATIVE_EPOCH_BLOCKS-1)%NATIVE_EPOCH_BLOCKS;
    if(!epoch_history.count || memcmp(source,epoch_history.source,LX06_IDLE_EPOCH_BYTES) ||
       memcmp(epoch,epoch_history.epoch,LX06_IDLE_EPOCH_BYTES) ||
       first_sample!=epoch_history.first_sample+(uint64_t)epoch_history.count*NATIVE_EPOCH_SAMPLES ||
       available_ms<epoch_history.available[last]) {
        memcpy(epoch_history.source,source,LX06_IDLE_EPOCH_BYTES);
        memcpy(epoch_history.epoch,epoch,LX06_IDLE_EPOCH_BYTES);
        epoch_history.first_sample=first_sample;
        epoch_history.oldest=0;
        epoch_history.count=0;
        pos=0;
    }
    epoch_history.available[pos]=available_ms;
    if(epoch_history.count==NATIVE_EPOCH_BLOCKS) {
        epoch_history.oldest=(epoch_history.oldest+1)%NATIVE_EPOCH_BLOCKS;
        epoch_history.first_sample+=NATIVE_EPOCH_SAMPLES;
    } else ++epoch_history.count;
    pthread_mutex_unlock(&epoch_history_lock);
}

void lx06_native_audio(const void *pcm,unsigned bytes) {
    if(bytes!=320 || !diagnostic_enabled()) return;
    memcpy(history+history_pos,pcm,bytes);
    history_pos=(history_pos+bytes)%sizeof(history);
    if(history_used<sizeof(history)) history_used+=bytes;
}
static void forward_wuw_guarded(unsigned flags,const void *pcm,int bytes,unsigned offset,void *extra) {
    int saved=errno;
    wuw_callback fn=__atomic_load_n(&upload_wuw,__ATOMIC_ACQUIRE);
    if(!fn || !official_enabled()) { errno=saved; return; }
    if(__atomic_load_n(&official_pending,__ATOMIC_ACQUIRE) || !stock_lease_take()) {
        /* Native WUW can arrive while the old cloud/player are shutting down.
         * Preserve its real audio boundary until the owner releases its lock. */
        pthread_mutex_lock(&deferred_lock);
        if(!(flags & 0xff00) && __atomic_load_n(&official_pending,__ATOMIC_ACQUIRE)!=2 &&
           pcm && bytes>0 && bytes<=(int)sizeof(deferred_pcm)) {
            memcpy(deferred_pcm,pcm,(size_t)bytes); deferred_bytes=bytes;
            deferred_flags=flags; deferred_offset=offset;
        }
        pthread_mutex_unlock(&deferred_lock);
        syslog(LOG_WARNING,"lx06-native: deferred wuw flags=%u bytes=%d",flags,bytes);
        errno=saved; return;
    }
    if(!(flags & 0xff00)) {
        stock_epoch_advance();
        __atomic_store_n(&stream_source,1,__ATOMIC_RELEASE);
    }
    syslog(LOG_WARNING,"lx06-native: wuw flags=%u bytes=%d offset=%u",flags,bytes,offset);
    errno=saved;
    fn(flags,pcm,bytes,offset,extra);
    __atomic_store_n(&stream_allowed,1,__ATOMIC_RELEASE);
    stock_lease_release();
}
static void forward_wuw(unsigned flags,const void *pcm,int bytes,unsigned offset,void *extra) {
    int cancel_state;
    if(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel_state)) return;
#if defined(__arm__) || defined(LX06_NATIVE_BIND_SELFTEST)
    if(!mdspeech_hold_wuw(flags,pcm,bytes,offset))
#endif
    forward_wuw_guarded(flags,pcm,bytes,offset,extra);
    (void)pthread_setcancelstate(cancel_state,NULL);
}
void register_data_upload_callback(void *a,void *b,void *c,void *d) {
    void (*original)(void *,void *,void *,void *)=dlsym(RTLD_NEXT,"register_data_upload_callback");
    Dl_info info;
    void *args[]={a,b,c,d};
    for(int i=0;i<4;i++) if(dladdr(args[i],&info))
        syslog(LOG_WARNING,"lx06-native: data arg%d %s +%lx",i,info.dli_fname,(unsigned long)((char *)args[i]-(char *)info.dli_fbase));
    if(original) {
        pthread_mutex_lock(&callback_registration_lock);
        __atomic_store_n(&upload_asr,(asr_callback)a,__ATOMIC_RELEASE);
        __atomic_store_n(&upload_wuw,(wuw_callback)b,__ATOMIC_RELEASE);
        original(a ? (void *)forward_asr : NULL,b ? (void *)forward_wuw : NULL,c,d);
        pthread_mutex_unlock(&callback_registration_lock);
    }
}
void unregister_data_upload_callback(void) {
    void (*original)(void)=dlsym(RTLD_NEXT,"unregister_data_upload_callback");
    if(!original) return;
    pthread_mutex_lock(&callback_registration_lock);
    __atomic_store_n(&upload_asr,NULL,__ATOMIC_RELEASE);
    __atomic_store_n(&upload_wuw,NULL,__ATOMIC_RELEASE);
#if defined(__arm__) || defined(LX06_NATIVE_BIND_SELFTEST)
    mdspeech_clear_wuw();
#endif
    pthread_mutex_lock(&deferred_lock);
    discard_official_pending();
    pthread_mutex_unlock(&deferred_lock);
    original();
    pthread_mutex_unlock(&callback_registration_lock);
}
static void record_wake(const char *kind, unsigned event, float angle) {
    int fd=open(LX06_NATIVE_EVENT,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
    if(fd>=0) {
        char line[80];
        int n=snprintf(line,sizeof(line),"%s %u %u\n",kind,++wake_serial,event);
        (void)write(fd,line,(size_t)n); close(fd);
    }
    if (event == 1 && !strcmp(kind, "official"))
        syslog(LOG_WARNING, "lx06-native: official event=1 唤醒词：小爱同学");
    else
        syslog(LOG_WARNING,"lx06-native: %s event=%u angle=%.1f",kind,event,(double)angle);
}
/* Script routing consumes a local win from Xiaomi's distributed wake service.
 * A DSP hit alone is only a candidate. The stock loss path stays registered. */
#if defined(__arm__) || defined(LX06_NATIVE_BIND_SELFTEST)
typedef int (*mdspeech_winner_fn)(void *, unsigned, float);
static mdspeech_winner_fn stock_mdspeech_winner;
static pthread_mutex_t mdspeech_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t mdspeech_origin_once = PTHREAD_ONCE_INIT;
static pthread_key_t mdspeech_origin;
static int mdspeech_origin_ready;
static uint64_t mdspeech_deadline;
static unsigned mdspeech_generation;
static char mdspeech_script[65];
struct mdspeech_wuw {
    unsigned flags, offset;
    int bytes;
    unsigned char pcm[];
};
static struct mdspeech_wuw *mdspeech_held_wuw;
static uint64_t mdspeech_drop_late_wuw;

static void mdspeech_clear_wuw(void) {
    pthread_mutex_lock(&mdspeech_lock);
    free(mdspeech_held_wuw);
    mdspeech_held_wuw = NULL;
    mdspeech_drop_late_wuw = mdspeech_deadline = 0;
    mdspeech_script[0] = 0;
    pthread_mutex_unlock(&mdspeech_lock);
}

static void mdspeech_replay_wuw(struct mdspeech_wuw *held) {
    if (!held) return;
    /* The stock callback consumes flags, PCM and size. Its producer's
     * opaque extra pointer is no longer live when arbitration completes. */
    forward_wuw_guarded(held->flags,held->pcm,held->bytes,held->offset,NULL);
    free(held);
}

static int mdspeech_hold_wuw(unsigned flags,const void *pcm,int bytes,unsigned offset) {
    if (flags & 0xff00) return 0;
    uint64_t now = lx06_idle_epoch_now_ms();
    int handled = 0;
    pthread_mutex_lock(&mdspeech_lock);
    if (now && now < mdspeech_drop_late_wuw) {
        mdspeech_drop_late_wuw = 0;
        handled = 1;
    } else if (now && now < mdspeech_deadline && mdspeech_script[0]) {
        /* Keep the confirmed keyword data with this arbitration. Sending it
         * early leaves stale cloud wake audio when the winner goes to a script. */
        struct mdspeech_wuw *held = NULL;
        if (pcm && bytes > 0 && bytes <= 96000)
            held = malloc(sizeof(*held) + (size_t)bytes);
        if (held) {
            held->flags = flags;
            held->offset = offset;
            held->bytes = bytes;
            memcpy(held->pcm,pcm,(size_t)bytes);
            free(mdspeech_held_wuw);
            mdspeech_held_wuw = held;
            handled = 1;
        } else {
            /* Fall back to stock if we cannot preserve its wake packet. */
            mdspeech_script[0] = 0;
        }
    }
    pthread_mutex_unlock(&mdspeech_lock);
    return handled;
}

static void mdspeech_origin_init(void) {
    mdspeech_origin_ready = pthread_key_create(&mdspeech_origin, NULL) == 0;
}

static unsigned mdspeech_submission(void) {
    uint64_t now = lx06_idle_epoch_now_ms();
    pthread_once(&mdspeech_origin_once, mdspeech_origin_init);
    pthread_mutex_lock(&mdspeech_lock);
    unsigned generation = ++mdspeech_generation;
    int overlapping = mdspeech_deadline && now < mdspeech_deadline;
    struct mdspeech_wuw *previous = mdspeech_held_wuw;
    mdspeech_held_wuw = NULL;
    mdspeech_drop_late_wuw = 0;
    mdspeech_deadline = now ? now + 1500 : 0;
    mdspeech_script[0] = 0;
    /* The winner callback has no request ID. Two outstanding submissions
     * make the result ambiguous, so preserve stock handling in that case. */
    if (now && !overlapping && mdspeech_origin_ready && pthread_getspecific(mdspeech_origin))
        (void)lx06_official_bind_read(mdspeech_script, sizeof(mdspeech_script));
    pthread_mutex_unlock(&mdspeech_lock);
    if (overlapping) mdspeech_replay_wuw(previous);
    else free(previous);
    return generation;
}

static void mdspeech_failed(unsigned generation) {
    struct mdspeech_wuw *held = NULL;
    pthread_mutex_lock(&mdspeech_lock);
    if (generation == mdspeech_generation) {
        mdspeech_script[0] = 0;
        held = mdspeech_held_wuw;
        mdspeech_held_wuw = NULL;
    }
    pthread_mutex_unlock(&mdspeech_lock);
    mdspeech_replay_wuw(held);
}

static int dispatch_mdspeech_winner(const char *submitted, float angle) {
    char id[65];
    if (!submitted[0] || !official_enabled() ||
        lx06_official_bind_read(id, sizeof(id)) != 1 || strcmp(id, submitted))
        return 0;
    if (lx06_official_bind_decide_at(LX06_OFFICIAL_BIND_PATH, LX06_BIND_APPS, id, sizeof(id))
        != LX06_BIND_ARMED || strcmp(id, submitted))
        return 0;
    if (lx06_official_bind_signal_at(LX06_OFFICIAL_BIND_EVENT, id) != 0) {
        syslog(LOG_ERR, "lx06-native: official bind signal failed");
        return 0;
    }
    if (lx06_session_busy()) (void)lx06_session_preempt();
    __atomic_store_n(&stream_allowed, 0, __ATOMIC_RELEASE);
    syslog(LOG_WARNING, "lx06-native: official nearby winner angle=%.1f script=%s", (double)angle, id);
    return 1;
}

static int mdspeech_winner(void *context, unsigned event, float angle) {
    int saved = errno, cancel_state;
    if (!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state)) {
        uint64_t now = lx06_idle_epoch_now_ms();
        pthread_mutex_lock(&mdspeech_lock);
        int dispatched = event == 1 && now && now < mdspeech_deadline &&
            dispatch_mdspeech_winner(mdspeech_script, angle);
        struct mdspeech_wuw *held = mdspeech_held_wuw;
        mdspeech_held_wuw = NULL;
        mdspeech_drop_late_wuw = dispatched && !held ? mdspeech_deadline : 0;
        mdspeech_deadline = 0;
        mdspeech_script[0] = 0;
        pthread_mutex_unlock(&mdspeech_lock);
        if (dispatched) free(held);
        else mdspeech_replay_wuw(held);
        (void)pthread_setcancelstate(cancel_state, NULL);
        errno = saved;
        if (dispatched) return 0;
    }
    mdspeech_winner_fn original = __atomic_load_n(&stock_mdspeech_winner, __ATOMIC_ACQUIRE);
    return original(context, event, angle);
}
#endif

#if defined(__arm__) && !defined(LX06_NATIVE_BIND_SELFTEST)
/* Verified against LX06 stock mipns-xiaomi and libmdspeech.so. Check code at
 * both sides of the private config ABI before replacing just the winner slot.
 * Unrecognized firmware continues through the original stock wake path. */
void *mdspeech_create(void *config, int value) {
    void *(*original)(void *, int) = dlsym(RTLD_NEXT, "mdspeech_create");
    if (!original) return NULL;
    const uint32_t *caller = __builtin_return_address(0);
    const uint32_t *entry = (const uint32_t *)original;
    Dl_info lib, layout, invoke;
    const uint32_t *load_winner = entry + (0xa864 - 0x8a78) / 4;
    const uint32_t *call_winner = entry + (0x1b62c - 0x8a78) / 4;
    if (!config || caller != (const uint32_t *)0x26d84 ||
        caller[-2] != 0xe1a00006 || caller[-1] != 0xebffb131 ||
        !dladdr(entry, &lib) || (uintptr_t)entry - (uintptr_t)lib.dli_fbase != 0x8a78 ||
        !dladdr(load_winner, &layout) || layout.dli_fbase != lib.dli_fbase ||
        !dladdr(call_winner, &invoke) || invoke.dli_fbase != lib.dli_fbase ||
        load_winner[0] != 0xe595a3b0 || load_winner[1] != 0xe59593b4 ||
        call_winner[0] != 0xe5943048 || call_winner[4] != 0xe5940044 ||
        call_winner[6] != 0xe12fff33)
        return original(config, value);
    mdspeech_winner_fn stock;
    unsigned char *slot = (unsigned char *)config + 944;
    memcpy(&stock, slot, sizeof(stock));
    if (stock != (mdspeech_winner_fn)0x25f18)
        return original(config, value);
    __atomic_store_n(&stock_mdspeech_winner, stock, __ATOMIC_RELEASE);
    mdspeech_winner_fn wrapped = mdspeech_winner;
    memcpy(slot, &wrapped, sizeof(wrapped));
    void *handle = original(config, value);
    /* The OEM constructor copies the callback and context into its worker. */
    memcpy(slot, &stock, sizeof(stock));
    if (handle) syslog(LOG_WARNING, "lx06-native: official nearby arbitration attached");
    return handle;
}

int mdspeech_wakeup_info_send(void *handle, unsigned event, const void *peer,
                             const void *when, float a, float b, float c) {
    int (*original)(void *, unsigned, const void *, const void *, float, float, float) =
        dlsym(RTLD_NEXT, "mdspeech_wakeup_info_send");
    if (!original) return -1;
    const void *caller = __builtin_return_address(0);
    if (caller != (const void *)0x26780 || event != 1 ||
        !__atomic_load_n(&stock_mdspeech_winner, __ATOMIC_ACQUIRE))
        return original(handle, event, peer, when, a, b, c);
    unsigned generation = mdspeech_submission();
    int result = original(handle, event, peer, when, a, b, c);
    if (result) mdspeech_failed(generation);
    return result;
}
#endif

static void submit_official_wake(wake_callback fn, void *context, unsigned event, float angle) {
#if defined(__arm__) || defined(LX06_NATIVE_BIND_SELFTEST)
    if (event == 1 && __atomic_load_n(&stock_mdspeech_winner, __ATOMIC_ACQUIRE)) {
        pthread_once(&mdspeech_origin_once, mdspeech_origin_init);
        if (mdspeech_origin_ready) {
            void *prior = pthread_getspecific(mdspeech_origin);
            if (pthread_setspecific(mdspeech_origin, &mdspeech_origin) == 0) {
                fn(context, event, angle);
                (void)pthread_setspecific(mdspeech_origin, prior);
                return;
            }
        }
    }
#endif
    fn(context, event, angle);
}

static void forward_wake_guarded(void *context,unsigned event,float angle) {
    int saved=errno;
    int keyword=(event & 0xff)==1;
    /* Keep the stock DSP and ASR machinery enabled. Gate only native keyword
     * callbacks: disabling engine KWS also stops local-to-official ASR. */
    if(keyword && !official_enabled()) {
        if(event==1 && __atomic_load_n(&stream_source,__ATOMIC_ACQUIRE)!=2)
            __atomic_store_n(&stream_allowed,0,__ATOMIC_RELEASE);
        errno=saved; return;
    }
    if(keyword && (__atomic_load_n(&official_pending,__ATOMIC_ACQUIRE) || !stock_lease_take())) {
        if(event==1) {
            pthread_mutex_lock(&deferred_lock);
            int expected=0;
            int first=!__atomic_load_n(&callback_ambiguous,__ATOMIC_ACQUIRE) &&
                __atomic_compare_exchange_n(&official_pending,&expected,1,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
            if(first) { deferred_angle=angle; pending_polls=0; }
            pthread_mutex_unlock(&deferred_lock);
            __atomic_store_n(&stream_allowed,0,__ATOMIC_RELEASE);
            if(first) record_wake("official",event,angle);
            (void)lx06_session_preempt();
        }
        errno=saved; return;
    }
    if(keyword) stock_epoch_advance();
    syslog(LOG_WARNING,"lx06-native: callback event=%u",event);
    if(event==1) {
        __atomic_store_n(&stream_source,1,__ATOMIC_RELEASE);
        __atomic_store_n(&stream_allowed,1,__ATOMIC_RELEASE);
        record_wake("official",event,angle);
    }
    errno=saved;
    submit_official_wake(callback,context,event,angle);
    if(keyword) stock_lease_release();
}
static void forward_wake(void *context,unsigned event,float angle) {
    int cancel_state;
    if(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel_state)) return;
    forward_wake_guarded(context,event,angle);
    (void)pthread_setcancelstate(cancel_state,NULL);
}
void xaudio_register_callback(void *engine,wake_callback fn,void *context) {
    register_fn original=(register_fn)dlsym(RTLD_NEXT,"xaudio_register_callback");
    if(!original) return;
    pthread_mutex_lock(&callback_registration_lock);
    if(callback && (callback != fn || callback_engine != engine)) {
        /* Vendor ownership is ambiguous after a second engine, replacement,
         * or unregister. Preserve its direct callback registration and stop
         * the bridge from invoking an old context on the audio worker. */
        __atomic_store_n(&callback_ambiguous,1,__ATOMIC_RELEASE);
        pthread_mutex_lock(&deferred_lock);
        discard_official_pending();
        pthread_mutex_unlock(&deferred_lock);
        original(engine,fn,context);
        pthread_mutex_unlock(&callback_registration_lock);
        return;
    }
    if(callback && callback_context!=context) {
        pthread_mutex_lock(&deferred_lock);
        discard_official_pending();
        pthread_mutex_unlock(&deferred_lock);
    }
    callback_engine=engine;
    callback_context=context;
    __atomic_store_n(&callback,fn,__ATOMIC_RELEASE);
    original(engine,fn ? forward_wake : NULL,context);
    pthread_mutex_unlock(&callback_registration_lock);
    Dl_info info;
    if(dladdr((void *)fn,&info))
        syslog(LOG_WARNING,"lx06-native: callback registered %s +%lx",info.dli_fname,(unsigned long)((char *)fn-(char *)info.dli_fbase));
}
enum native_request_kind { NATIVE_NONE, NATIVE_DIAGNOSTIC, NATIVE_ATTESTED };
struct native_request {
    unsigned char epoch[LX06_IDLE_EPOCH_BYTES], source[LX06_IDLE_EPOCH_BYTES];
    uint64_t first_sample, end_sample, deadline_ms;
    int legacy;
};

static int take_decimal(const char **pos,char terminator,uint64_t *value) {
    const char *p=*pos;
    uint64_t n=0;
    if(*p<'0' || *p>'9') return -1;
    while(*p>='0' && *p<='9') {
        unsigned digit=(unsigned)(*p++-'0');
        if(n>UINT64_MAX/10u || (n==UINT64_MAX/10u && digit>UINT64_MAX%10u)) return -1;
        n=n*10u+digit;
    }
    if(*p!=terminator) return -1;
    *pos=p+1;
    *value=n;
    return 0;
}

static enum native_request_kind parse_local_request(char *line,struct native_request *request) {
    if(!strcmp(line,"wake\n")) return diagnostic_enabled() ? NATIVE_DIAGNOSTIC : NATIVE_NONE;
    const size_t sep=6+LX06_IDLE_EPOCH_HEX, end=sep+1+LX06_IDLE_EPOCH_HEX;
    if(strlen(line)<end+5 || line[sep]!=' ' || line[end]!=' ')
        return NATIVE_NONE;
    request->legacy=0;
    if(memcmp(line,"wake3 ",6)) {
#ifdef LX06_PRIVATE_RAW_PROFILE
        if(memcmp(line,"wake2 ",6)) return NATIVE_NONE;
        request->legacy=1;
#else
        return NATIVE_NONE;
#endif
    }
    line[sep]=line[end]=0;
    if(lx06_idle_epoch_parse(line+6,request->epoch) || lx06_idle_epoch_parse(line+sep+1,request->source) ||
       !nonzero_identity(request->epoch) || !nonzero_identity(request->source)) return NATIVE_NONE;
    const char *p=line+end+1;
    if(!request->legacy && take_decimal(&p,' ',&request->first_sample)) return NATIVE_NONE;
    if(take_decimal(&p,' ',&request->end_sample) || take_decimal(&p,'\n',&request->deadline_ms) ||
       *p || !request->end_sample || !request->deadline_ms) return NATIVE_NONE;
    return NATIVE_ATTESTED;
}

/* Keep one verdict per consumed request. No polling writes or voice data. */
static void local_request_result(const char *result) {
    int saved=errno;
    char tmp[]=LX06_NATIVE_REQUEST ".result.XXXXXX";
    int fd=mkstemp(tmp);
    if(fd>=0) {
        char line[96];
        int n=snprintf(line,sizeof(line),"%llu %s\n",
                       (unsigned long long)lx06_idle_epoch_now_ms(),result);
        int ok=n>0 && (size_t)n<sizeof(line) && write(fd,line,(size_t)n)==n;
        if(close(fd) || !ok || rename(tmp,LX06_NATIVE_REQUEST ".result")) unlink(tmp);
    }
    errno=saved;
}

static enum native_request_kind take_local_request(struct native_request *request) {
    char claimed[640];
    int size=snprintf(claimed,sizeof(claimed),"%s.native",LX06_NATIVE_REQUEST);
    if(size<0 || (size_t)size>=sizeof(claimed) || rename(LX06_NATIVE_REQUEST,claimed)) return NATIVE_NONE;
    int fd=open(claimed,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    (void)unlink(claimed);
    if(fd<0) {local_request_result("request-open");return NATIVE_NONE;}
    LX06_SESSION_STATBUF st;
    char command[160];
    enum native_request_kind kind=NATIVE_NONE;
    if(!LX06_EPOCH_FSTAT(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
       !st.st_nlink && !(st.st_mode&0022) && st.st_size>0 && st.st_size<(off_t)sizeof(command)) {
        ssize_t n;
        do { n=read(fd,command,sizeof(command)); } while(n<0 && errno==EINTR);
        if(n==st.st_size && !memchr(command,0,(size_t)n)) {
            command[n]=0;
            kind=parse_local_request(command,request);
            if(kind==NATIVE_DIAGNOSTIC) {
                time_t now=time(NULL);
                if(now<=0 || st.st_mtime<now-1 || st.st_mtime>now) kind=NATIVE_NONE;
            }
        }
    }
    close(fd);
    if(kind==NATIVE_NONE) local_request_result("invalid-request");
    return kind;
}

/* Validate the local keyword against contiguous, same-source capture.
 * The caller holds the live epoch's audio lease during validation and dispatch. */
static unsigned attested_wuw_bytes(const struct native_request *request,const char **reason) {
    uint64_t now=lx06_idle_epoch_now_ms();
    *reason="expired";
    if(!now || now>=request->deadline_ms) return 0;
    *reason="history-busy";
    if(pthread_mutex_trylock(&epoch_history_lock)) return 0;
    unsigned bytes=0;
    *reason="history-source";
    if(!epoch_history.count || memcmp(request->source,epoch_history.source,LX06_IDLE_EPOCH_BYTES) ||
       memcmp(request->epoch,epoch_history.epoch,LX06_IDLE_EPOCH_BYTES) ||
       request->end_sample<=epoch_history.first_sample) goto done;
    uint64_t base=epoch_history.first_sample, end=request->end_sample;
    uint64_t first=request->legacy ? base : request->first_sample;
    if(request->legacy && epoch_history.count > sizeof(wake_pcm)/NATIVE_EPOCH_BYTES)
        first+=(epoch_history.count-sizeof(wake_pcm)/NATIVE_EPOCH_BYTES)*NATIVE_EPOCH_SAMPLES;
    *reason="history-range";
    if(first<base || end<=first || end-base>(uint64_t)epoch_history.count*NATIVE_EPOCH_SAMPLES ||
       end-first<NATIVE_EPOCH_MIN_BLOCKS*NATIVE_EPOCH_SAMPLES) goto done;
    if(request->legacy && (end-base)%NATIVE_EPOCH_SAMPLES) goto done;
    if(!request->legacy && end-first>NATIVE_EPOCH_MAX_WORD_SAMPLES) goto done;
    unsigned last=(epoch_history.oldest+(unsigned)((end-base-1)/NATIVE_EPOCH_SAMPLES))%NATIVE_EPOCH_BLOCKS;
    uint64_t available=epoch_history.available[last];
    *reason="history-stamp";
    if(available>now || request->deadline_ms<available ||
       request->deadline_ms-available>NATIVE_EPOCH_MAX_AGE_MS) goto done;
    if(!request->legacy && request->deadline_ms-available!=NATIVE_EPOCH_MAX_AGE_MS) goto done;
    bytes=(unsigned)(end-first)*2;
done:
    pthread_mutex_unlock(&epoch_history_lock);
    return bytes;
}

static unsigned diagnostic_wuw(void) {
    unsigned oldest=history_used==sizeof(history) ? history_pos : 0;
    unsigned first=sizeof(history)-oldest;
    if(first>history_used) first=history_used;
    memcpy(wake_pcm,history+oldest,first);
    memcpy(wake_pcm+first,history,history_used-first);
    return history_used;
}
/* Local-to-official trigger. It runs only for an explicit dispatcher request
 * on the existing audio worker; no extra thread, socket proxy or self-play.
 * The dispatcher marks the origin before stock callbacks create legacy events.
 */
static void native_poll_registered(void) {
    int own_stock=__atomic_load_n(&stock_lease_users,__ATOMIC_ACQUIRE)>0;
    __atomic_store_n(&script_active,!own_stock && lx06_session_busy(),__ATOMIC_RELEASE);
    static int (*asr_active)(void);
    if(!asr_active) asr_active=(int (*)(void))dlsym(RTLD_NEXT,"get_asr_flag");
    if(asr_active && !asr_active()) __atomic_store_n(&stream_source,0,__ATOMIC_RELEASE);
    struct native_request request;
    enum native_request_kind local_request=take_local_request(&request);
    wake_callback fn=__atomic_load_n(&callback,__ATOMIC_ACQUIRE);
    wuw_callback wuw=__atomic_load_n(&upload_wuw,__ATOMIC_ACQUIRE);
    if(!fn || __atomic_load_n(&callback_ambiguous,__ATOMIC_ACQUIRE)) {
        if(local_request!=NATIVE_NONE) local_request_result(fn ? "ambiguous-callback" : "missing-callback");
        return;
    }
    if(__atomic_load_n(&official_pending,__ATOMIC_ACQUIRE)) {
        if(local_request!=NATIVE_NONE) local_request_result("official-pending");
        if(!wuw || !official_enabled()) {
            if(pthread_mutex_trylock(&deferred_lock)!=0) return;
            discard_official_pending();
            pthread_mutex_unlock(&deferred_lock);
            return;
        }
        if(!stock_lease_take()) {
            (void)lx06_session_preempt();
            return;
        }
        /* No waiting or signal loops on the microphone worker. The supervisor
         * has already reaped its player and closed its network process. */
        if(pthread_mutex_trylock(&deferred_lock)!=0) { stock_lease_release(); return; }
        int bytes=deferred_bytes;
        /* Confirmation audio may be delivered after the wake event. Never
         * complete the handoff with an empty or suspect WUW packet. */
        if(!bytes) {
            if(++pending_polls>=100) {
                __atomic_store_n(&official_pending,0,__ATOMIC_RELEASE);
                syslog(LOG_ERR,"lx06-native: official handoff missing confirmed WUW");
            }
            pthread_mutex_unlock(&deferred_lock);
            stock_lease_release();
            return;
        }
        unsigned flags=deferred_flags, offset=deferred_offset;
        float angle=deferred_angle;
        memcpy(wake_pcm,deferred_pcm,(size_t)bytes);
        deferred_bytes=0;
        __atomic_store_n(&official_pending,2,__ATOMIC_RELEASE);
        __atomic_store_n(&stream_source,1,__ATOMIC_RELEASE);
        pthread_mutex_unlock(&deferred_lock);
        stock_epoch_advance();
        fn(callback_context,0x101,angle);
        submit_official_wake(fn,callback_context,1,angle);
        /* The verified stock callback reads flags/PCM/bytes only. The opaque
         * producer-owned extra pointer must not outlive the original callback. */
        wuw(flags,wake_pcm,bytes,offset,NULL);
        __atomic_store_n(&official_pending,0,__ATOMIC_RELEASE);
        __atomic_store_n(&stream_allowed,1,__ATOMIC_RELEASE);
        syslog(LOG_WARNING,"lx06-native: official handoff complete wuw_bytes=%d",bytes);
        stock_lease_release();
        return;
    }
    if(local_request==NATIVE_NONE) return;
    void (*start)(void)=(void (*)(void))dlsym(RTLD_NEXT,"set_wakeup_status");
    if(!start || !wuw || (local_request==NATIVE_ATTESTED && (!asr_active || asr_active()))) {
        local_request_result(!start ? "missing-start" : !wuw ? "missing-wuw" :
                             !asr_active ? "missing-asr-status" : "asr-active");
        return;
    }
    /* Keep the actual owner lock until the native wake, WUW and start have
     * crossed the stock entry points. An idle observation alone races a new
     * session-run lock before the first stock callback. */
    int lease=local_request==NATIVE_ATTESTED ? lx06_idle_epoch_take(request.epoch) : lx06_session_take_idle();
    if(lease<0) {
        local_request_result("idle-admission");
        syslog(LOG_WARNING,"lx06-native: local request missed idle admission");
        return;
    }
    const char *reason="unavailable-window";
    unsigned captured=local_request==NATIVE_ATTESTED ? attested_wuw_bytes(&request,&reason) : diagnostic_wuw();
    if(!captured || (local_request==NATIVE_ATTESTED && (asr_active() ||
       __atomic_load_n(&official_pending,__ATOMIC_ACQUIRE))) || lx06_idle_epoch_changed(lease)) {
        close(lease);
        local_request_result(captured ? "session-changed" : reason);
        return;
    }
    if(local_request==NATIVE_ATTESTED) {
        uint64_t now=lx06_idle_epoch_now_ms();
        if(!now || now>=request.deadline_ms) { close(lease); local_request_result("expired"); return; }
    }
    syslog(LOG_WARNING,"lx06-native: local request begin");
    __atomic_store_n(&stream_source,2,__ATOMIC_RELEASE);
    __atomic_store_n(&stream_allowed,1,__ATOMIC_RELEASE);
    /* Stock callbacks also emit wakeup-2/WuW markers. Identify the origin before
     * invoking them, otherwise webd mistakes a local alias for a real official
     * keyword and suppresses all other aliases for eight seconds. The stock
     * handshake still needs a WUW packet. Send silence for a validated local
     * alias so the cloud only hears the live follow-up through its ASR stream. */
    if(local_request==NATIVE_ATTESTED) memset(wake_pcm,0,captured);
    record_wake("local",1,0.0f);
    fn(callback_context,0x101,0.0f);
    fn(callback_context,1,0.0f);
    wuw(1,wake_pcm,(int)captured,0,NULL);
    start();
    close(lease);
    local_request_result("started");
    syslog(LOG_WARNING,"lx06-native: local request sent");
}
void lx06_native_poll(void) {
    if(__atomic_add_fetch(&poll_frames,1,__ATOMIC_RELAXED) % 5) return;
    /* The vendor cancels the DNN worker before unregistering. Leave both the
     * registration mutex and the session lease before honoring cancellation. */
    int cancel_state;
    if(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel_state)) return;
    if(!pthread_mutex_trylock(&callback_registration_lock)) {
        native_poll_registered();
        pthread_mutex_unlock(&callback_registration_lock);
    }
    (void)pthread_setcancelstate(cancel_state,NULL);
}

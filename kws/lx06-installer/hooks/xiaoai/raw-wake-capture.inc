/* Candidate private KWE2 source for the stock 48 kHz PCM. Include only in
 * a build that has verified a hardware history bound for this mixer route. */
#include "session-epoch.h"
#include "wake-capture-frame.h"

#define RAW_CAPTURE_FRAME_BYTES 16
#define RAW_CAPTURE_BUFFER 8192
#define RAW_CAPTURE_PERIOD 512
#define RAW_CAPTURE_TAPS 31

struct raw_alsa_status {
    uint32_t state;
    int32_t trigger_seconds, trigger_nanoseconds;
    int32_t stamp_seconds, stamp_nanoseconds;
    uint32_t application, hardware;
    int32_t delay;
    uint32_t available;
    uint32_t reserved[18];
};
_Static_assert(sizeof(struct raw_alsa_status) == 108, "OEM ALSA status ABI");
_Static_assert(LX06_RAW_WAKE_CAPTURE_HISTORY_FRAMES > 0 &&
    LX06_RAW_WAKE_CAPTURE_HISTORY_FRAMES <= INT32_MAX, "invalid hardware history bound");

/* 31-tap normalized, Hamming-windowed 7 kHz low pass, Q15. Its whole
 * support must lie after the source fence; it has no recursive state. */
static const int16_t raw_filter[RAW_CAPTURE_TAPS] = {
    51, 17, -58, -146, -134, 84, 426, 555, 114, -838, -1592, -1105, 1213, 4834, 8186,
    9554, 8186, 4834, 1213, -1105, -1592, -838, 114, 555, 426, 84, -134, -146, -58, 17, 51
};

static struct {
    void *pcm;
    struct raw_alsa_status last;
    unsigned char idle[16], source[16];
    uint64_t fence;
    uint64_t first;
    unsigned head, history, used;
    int16_t samples[RAW_CAPTURE_TAPS];
#ifdef LX06_RAW_WAKE_CAPTURE_CHANNEL_RESET
    int16_t hp_samples[65];
    int32_t hp_sum;
    unsigned hp_head, hp_count;
#endif
    unsigned char frame[LX06_WAKE_FRAME_BYTES];
} raw_wake;

static volatile uint32_t raw_observing;
static uintptr_t raw_stock_site;
typedef long (*raw_read_fn)(void *, void *, unsigned long);
static raw_read_fn raw_read_real;
static int (*raw_status)(void *, struct raw_alsa_status *);
static int (*raw_pcm_type)(const void *);
static int (*raw_pcm_stream)(const void *);
static const char *(*raw_pcm_name)(void *);
static int (*raw_pcm_params)(void *, unsigned long *, unsigned long *);
static size_t (*raw_params_size)(void);
static int (*raw_params_current)(void *, void *);
static int (*raw_params_format)(const void *, int *);
static int (*raw_params_access)(const void *, int *);
static int (*raw_params_channels)(const void *, unsigned *);
static int (*raw_params_rate)(const void *, unsigned *, int *);
static int raw_wake_fd = -1;
union raw_params_storage { uint64_t align; unsigned char bytes[1024]; };

#ifdef LX06_RAW_WAKE_CAPTURE_CHANNEL_RESET
#include "raw-wake-channel-reset.inc"
#define RAW_CAPTURE_MIC 4
#else
#define RAW_CAPTURE_MIC 0
#endif

static int raw_same_trigger(const struct raw_alsa_status *a, const struct raw_alsa_status *b) {
    return a->trigger_seconds == b->trigger_seconds && a->trigger_nanoseconds == b->trigger_nanoseconds;
}

static int raw_valid_status(const struct raw_alsa_status *s) {
    /* Reopen the source epoch on wrap/XRUN rather than guessing positions. */
    return s->state == 3 && s->hardware >= s->application &&
        s->hardware - s->application == s->available && s->available <= RAW_CAPTURE_BUFFER;
}

static int raw_idle_snapshot(unsigned char idle[16]) {
    int result = lx06_idle_epoch_snapshot(idle);
    if (result == 0 || errno != EWOULDBLOCK) return result;
    /* Concurrent status observers on older binaries briefly take exclusive
     * flock. Retry the complete admission once, before accepting any data.
     * A held session, surviving owner, or changed epoch still drops the read. */
    const struct { long sec, nsec; } pause = {0, 150000};
    if (syscall(SYS_nanosleep, &pause, NULL) != 0) return -1;
    return lx06_idle_epoch_snapshot(idle);
}

static int raw_valid_format(void *pcm) {
    unsigned long capacity = 0, period = 0;
    union raw_params_storage hw;
    unsigned channels, rate;
    int format, access, direction;
    const char *name = raw_pcm_name(pcm);
    return name && !strcmp(name, "hw:0,3") && raw_pcm_type(pcm) == 0 && raw_pcm_stream(pcm) == 1 &&
        raw_pcm_params(pcm, &capacity, &period) == 0 && capacity == RAW_CAPTURE_BUFFER && period == RAW_CAPTURE_PERIOD &&
        raw_params_size() <= sizeof(hw) && raw_params_current(pcm, &hw) == 0 &&
        raw_params_format(&hw, &format) == 0 && format == 2 && /* S16_LE */
        raw_params_access(&hw, &access) == 0 && access == 3 && /* RW_INTERLEAVED */
        raw_params_channels(&hw, &channels) == 0 && channels == 8 &&
        raw_params_rate(&hw, &rate, &direction) == 0 && rate == 48000;
}

static int raw_new_source(void *pcm, const struct raw_alsa_status *before, const unsigned char idle[16]) {
    static const unsigned char zero[16];
    raw_wake.pcm = NULL;
#ifdef SYS_getrandom
    if (!raw_valid_format(pcm) ||
        syscall(SYS_getrandom, raw_wake.source, sizeof(raw_wake.source), 1 /* GRND_NONBLOCK */) != sizeof(raw_wake.source) ||
        !memcmp(raw_wake.source, zero, sizeof(zero))) return 0;
#else
    return 0;
#endif
    raw_wake.pcm = pcm;
    memcpy(raw_wake.idle, idle, sizeof(raw_wake.idle));
    /* hw_ptr is the DDR reply position. The compile-time bound accounts
     * for physical history upstream of it; the observed backlog does not. */
    raw_wake.fence = (uint64_t)before->hardware + LX06_RAW_WAKE_CAPTURE_HISTORY_FRAMES;
    raw_wake.head = raw_wake.history = raw_wake.used = 0;
#ifdef LX06_RAW_WAKE_CAPTURE_CHANNEL_RESET
    memset(raw_wake.hp_samples, 0, sizeof(raw_wake.hp_samples));
    raw_wake.hp_sum = raw_wake.hp_head = raw_wake.hp_count = 0;
#endif
    memcpy(raw_wake.frame, LX06_WAKE_FRAME_MAGIC, 4);
    memcpy(raw_wake.frame + LX06_WAKE_FRAME_SOURCE, raw_wake.source, 16);
    memcpy(raw_wake.frame + LX06_WAKE_FRAME_IDLE, raw_wake.idle, 16);
    return 1;
}

extern void lx06_native_audio_epoch(const void *pcm, const unsigned char source[16],
                                    const unsigned char idle[16], uint64_t first, uint64_t available_ms);

static void raw_publish(const unsigned char frame[LX06_WAKE_FRAME_BYTES], uint64_t first, uint64_t available_ms) {
    static const struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = TAPDIR "/kws.ke2" };
    static const struct sockaddr_un enrollment = { .sun_family = AF_UNIX, .sun_path = TAPDIR "/enrollment.ke2" };
    lx06_native_audio_epoch(frame + LX06_WAKE_FRAME_HEADER, frame + LX06_WAKE_FRAME_SOURCE,
                            frame + LX06_WAKE_FRAME_IDLE, first, available_ms);
    if (raw_wake_fd < 0) raw_wake_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (raw_wake_fd >= 0) {
        (void)sendto(raw_wake_fd, frame, LX06_WAKE_FRAME_BYTES, MSG_DONTWAIT | MSG_NOSIGNAL,
                     (const struct sockaddr *)&address, sizeof(address));
        (void)sendto(raw_wake_fd, frame, LX06_WAKE_FRAME_BYTES, MSG_DONTWAIT | MSG_NOSIGNAL,
                     (const struct sockaddr *)&enrollment, sizeof(enrollment));
    }
}

#ifdef LX06_WAKE_PAIRED_NS
#include "wake-oem-ns-worker.inc"
#endif
static void raw_send(uint64_t available_ms) {
    put_le(raw_wake.frame + LX06_WAKE_FRAME_AVAILABLE, available_ms, 8);
#ifdef LX06_WAKE_PAIRED_NS
    /* The next position discontinuity resets NS after any dropped packet. */
    if (ns_owner == getpid() && ns_input >= 0 &&
        send(ns_input,raw_wake.frame,sizeof(raw_wake.frame),MSG_DONTWAIT|MSG_NOSIGNAL)!=sizeof(raw_wake.frame))
        __atomic_fetch_add(&ns_drop,1,__ATOMIC_RELAXED);
#else
    raw_publish(raw_wake.frame, raw_wake.first, available_ms);
#endif
}

static void raw_consume(const unsigned char *pcm, uint32_t position, unsigned count, uint64_t available_ms) {
    for (unsigned i = 0; i < count; ++i, ++position, pcm += RAW_CAPTURE_FRAME_BYTES) {
        if (position < raw_wake.fence) continue;
        /* The capture channel belongs to the physical microphone mask,
         * outside the reference slots. It needs its own calibration. */
        raw_wake.samples[raw_wake.head] = (int16_t)(pcm[2 * RAW_CAPTURE_MIC] |
            ((unsigned)pcm[2 * RAW_CAPTURE_MIC + 1] << 8));
        raw_wake.head = (raw_wake.head + 1) % RAW_CAPTURE_TAPS;
        if (raw_wake.history < RAW_CAPTURE_TAPS) ++raw_wake.history;
        if (raw_wake.history < RAW_CAPTURE_TAPS || position % 3) continue;
        int32_t sum = 0;
        for (unsigned j = 0; j < RAW_CAPTURE_TAPS; ++j)
            sum += raw_wake.samples[(raw_wake.head + j) % RAW_CAPTURE_TAPS] * (int32_t)raw_filter[j];
        int32_t value = sum / 32768;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        uint64_t sample = position / 3;
#ifdef LX06_RAW_WAKE_CAPTURE_CHANNEL_RESET
        /* Raw slot 4 has low speech levels and substantial low-frequency
         * room noise. Use the exact finite response calibrated for this
         * profile, with the frame position at the centre of its support. */
        unsigned slot = raw_wake.hp_head;
        raw_wake.hp_sum += value - raw_wake.hp_samples[slot];
        raw_wake.hp_samples[slot] = (int16_t)value;
        raw_wake.hp_head = (slot + 1) % 65;
        if (raw_wake.hp_count < 65) ++raw_wake.hp_count;
        if (raw_wake.hp_count < 65) continue;
        value = (raw_wake.hp_samples[(slot + 33) % 65] - raw_wake.hp_sum / 65) * 8;
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        sample -= 32;
#endif
        if (raw_wake.used == 0) {
            raw_wake.first = sample;
            put_le(raw_wake.frame + LX06_WAKE_FRAME_FIRST, raw_wake.first, 8);
        }
        put_le(raw_wake.frame + LX06_WAKE_FRAME_HEADER + 2 * raw_wake.used, (uint16_t)value, 2);
        if (++raw_wake.used == LX06_WAKE_FRAME_SAMPLES) {
            raw_send(available_ms);
            raw_wake.used = 0;
        }
    }
}

/* Linux ARM 32-bit and the exact OEM engine/libasound ABI only. Original
 * read arguments, output buffer contents, return value and errno are kept. */
__attribute__((constructor)) static void raw_wake_init(void) {
    int saved = errno;
    void *site = dlsym(RTLD_DEFAULT, "AudioRecord_ReadEx");
    size_t (*sizeof_status)(void) = dlsym(RTLD_NEXT, "snd_pcm_status_sizeof");
    raw_status = dlsym(RTLD_NEXT, "snd_pcm_status");
    raw_pcm_type = dlsym(RTLD_NEXT, "snd_pcm_type");
    raw_pcm_stream = dlsym(RTLD_NEXT, "snd_pcm_stream");
    raw_pcm_name = dlsym(RTLD_NEXT, "snd_pcm_name");
    raw_pcm_params = dlsym(RTLD_NEXT, "snd_pcm_get_params");
    raw_params_size = dlsym(RTLD_NEXT, "snd_pcm_hw_params_sizeof");
    raw_params_current = dlsym(RTLD_NEXT, "snd_pcm_hw_params_current");
    raw_params_format = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_format");
    raw_params_access = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_access");
    raw_params_channels = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_channels");
    raw_params_rate = dlsym(RTLD_NEXT, "snd_pcm_hw_params_get_rate");
    if (site && sizeof_status && sizeof_status() == sizeof(struct raw_alsa_status) &&
        raw_status && raw_pcm_type && raw_pcm_stream && raw_pcm_name && raw_pcm_params && raw_params_size &&
        raw_params_current && raw_params_format && raw_params_access && raw_params_channels && raw_params_rate
#ifdef LX06_RAW_WAKE_CAPTURE_CHANNEL_RESET
        && raw_reset_resolve()
#endif
        )
        raw_stock_site = (uintptr_t)site + (0x28034 - 0x27e7c);
#ifdef LX06_WAKE_PAIRED_NS
    if (raw_stock_site && !ns_worker_start()) raw_stock_site = 0;
#endif
    errno = saved;
}

long snd_pcm_readi(void *pcm, void *buffer, unsigned long requested) {
    const int initial_errno = errno;
    raw_read_fn read_real = __atomic_load_n(&raw_read_real, __ATOMIC_ACQUIRE);
    if (!read_real) {
        read_real = dlsym(RTLD_NEXT, "snd_pcm_readi");
        if (!read_real) { errno = ENOSYS; return -1; }
        __atomic_store_n(&raw_read_real, read_real, __ATOMIC_RELEASE);
    }
    if (!pcm || !buffer || !raw_stock_site || (uintptr_t)__builtin_return_address(0) != raw_stock_site ||
        __atomic_exchange_n(&raw_observing, 1, __ATOMIC_ACQUIRE)) {
        errno = initial_errno;
        return read_real(pcm, buffer, requested);
    }
    unsigned char idle_before[16], idle_after[16];
    struct raw_alsa_status before, after;
    uint64_t available_ms = lx06_idle_epoch_now_ms();
    int checked = available_ms && raw_idle_snapshot(idle_before) == 0 &&
        raw_status(pcm, &before) == 0 && raw_valid_status(&before);
#ifdef LX06_RAW_WAKE_CAPTURE_CHANNEL_RESET
    if (checked) checked = raw_reset_epoch(pcm, idle_before, &before) && raw_valid_status(&before);
#endif
    errno = initial_errno;
    long frames = read_real(pcm, buffer, requested);
    int saved = errno;
    if (!checked || frames <= 0 || (unsigned long)frames > requested || frames > RAW_CAPTURE_BUFFER ||
        raw_status(pcm, &after) != 0 || !raw_valid_status(&after) || !raw_same_trigger(&before, &after) ||
        after.application < before.application || after.hardware < before.hardware ||
        after.application - before.application != (uint32_t)frames ||
        raw_idle_snapshot(idle_after) != 0 || memcmp(idle_before, idle_after, sizeof(idle_before))) {
        raw_wake.pcm = NULL;
    } else {
        if (raw_wake.pcm != pcm || !raw_same_trigger(&raw_wake.last, &before) ||
            raw_wake.last.application != before.application || memcmp(raw_wake.idle, idle_before, sizeof(idle_before)))
            (void)raw_new_source(pcm, &before, idle_before);
        if (raw_wake.pcm == pcm) {
            raw_consume(buffer, before.application, (unsigned)frames, available_ms);
            raw_wake.last = after;
        }
    }
    __atomic_store_n(&raw_observing, 0, __ATOMIC_RELEASE);
    errno = saved;
    return frames;
}

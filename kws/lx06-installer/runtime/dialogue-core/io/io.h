/* Generic I/O for the dialogue core. No provider, account or cloud protocol.
 * All producer operations are bounded/nonblocking; only final destroy joins.
 * One player worker and one optional light worker are created for the core's
 * lifetime. Capture belongs to the caller's existing poll/epoll loop. */
#ifndef LX06_DC_IO_H
#define LX06_DC_IO_H
#include "../types.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <poll.h>

#define DC_IO_PCM_CAPACITY 65536u
#define DC_IO_PCM_WRITE_MAX 4096u
#define DC_IO_BACKEND_POLL_MAX 8u
enum dc_io_event_type { DC_IO_FIRST_PCM=1, DC_IO_DRAINED, DC_IO_STOPPED, DC_IO_ERROR,
    DC_IO_PCM_RESUMED, DC_IO_STOP_FAILED };
typedef struct dc_io_event {
    enum dc_io_event_type type;
    dc_tag tag;
    uint64_t monotonic_ns;
    int error; /* positive errno; zero on success */
} dc_io_event;

/* Backend methods run ONLY on the fixed player worker. They must not block.
 * write returns frames or negative errno; period writes are <=10 ms. A real
 * ALSA backend uses SND_PCM_NONBLOCK. No network/synthesis belongs here. */
typedef struct dc_io_playback_backend {
    void *context;
    int (*open)(void *,unsigned rate);
    int (*pollfds)(void *,struct pollfd *,unsigned capacity);
    ssize_t (*write)(void *,const int16_t *,size_t frames);
    int (*delay)(void *,long *frames);
    int (*prepare)(void *);
    int (*drop)(void *);
    void (*close)(void *);
} dc_io_playback_backend;
typedef struct dc_io_player dc_io_player;
dc_io_player *dc_io_player_create(const dc_io_playback_backend *);
/* Callbacks run under player mutex: bounded, no player reentry or I/O. */
typedef struct dc_io_reference {
    void *context;
    void (*reset)(void *);
    /* Worker chunk operation and accepted-frame offset, not playback time. */
    void (*accepted)(void *,dc_tag,const int16_t *,size_t,unsigned,uint64_t,uint64_t);
} dc_io_reference;
dc_io_player *dc_io_player_create_with_reference(const dc_io_playback_backend *,const dc_io_reference *);
int dc_io_player_begin(dc_io_player *,dc_tag,unsigned rate);
int dc_io_player_write(dc_io_player *,dc_tag,const void *,size_t bytes);
int dc_io_player_finish(dc_io_player *,dc_tag);
/* Invalidates queued/local PCM before notifying the independent stop eventfd.
 * Idempotent for the same canceled tag. STOPPED acknowledges device drop, not
 * physical speaker residual sound. It never waits for a cloud operation. */
int dc_io_player_stop(dc_io_player *,dc_tag);
size_t dc_io_player_queued(dc_io_player *);
int dc_io_player_event_fd(const dc_io_player *);
int dc_io_player_next_event(dc_io_player *,dc_io_event *); /* 1, 0 or -errno */
void dc_io_player_destroy(dc_io_player *);

/* Stable libasound public ABI is loaded dynamically. No fixed code addresses,
 * capture device, player subprocess, shell or dependency on host ALSA headers.
 * The object must outlive its player; release it after player_destroy. */
typedef struct dc_io_alsa dc_io_alsa;
dc_io_alsa *dc_io_alsa_create(const char *device,dc_io_playback_backend *out);
void dc_io_alsa_destroy(dc_io_alsa *);

#define DC_IO_CAPTURE_MAX 4096u
enum dc_io_capture_flags { DC_IO_CAPTURE_READY=1, DC_IO_CAPTURE_GAP=2,
    DC_IO_CAPTURE_SEQUENCE_RESET=4, DC_IO_CAPTURE_METADATA_INVALID=8 };
enum dc_io_capture_mode { DC_IO_CAPTURE_LXA1=1, DC_IO_CAPTURE_LXA2=2 };
typedef struct dc_io_capture_frame {
    uint64_t sequence,producer_birth_ticks,available_mono_ms;
    uint32_t producer_pid,origin,rate,bytes,flags,lost;
    enum dc_io_capture_mode mode;
    uint64_t monotonic_ns;
    unsigned char pcm[DC_IO_CAPTURE_MAX];
} dc_io_capture_frame;
typedef struct dc_io_capture dc_io_capture;
/* Directory must already exist, root-owned, without group/other write. The
 * fixed name is a passive original LXA1 subscriber. No PNS/session/VOIP calls. */
dc_io_capture *dc_io_capture_open(const char *directory,const char *name);
/* Explicit stamped mode binds account.a2/account.a2.lock ONLY, never .sock.
 * LXA1 does not invent producer identity/availability timestamps. LXA2 keeps
 * every well-framed PCM byte even if metadata is invalid (flagged for the
 * endpoint's existing conservative fallback). No format auto-detection. */
dc_io_capture *dc_io_capture_open_mode(const char *directory,const char *name,enum dc_io_capture_mode);
int dc_io_capture_fd(const dc_io_capture *);
int dc_io_capture_next(dc_io_capture *,dc_io_capture_frame *); /* 1,0,-errno */
int dc_io_capture_stalled(const dc_io_capture *,uint64_t now_ns); /* >=3s */
void dc_io_capture_close(dc_io_capture *);

/* Light sink is inherited/trusted, not a path or a network service. Every
 * update replaces the pending update. DC_CLOSING is an ACTIVE light; only
 * DC_CLOSED can send ending. Stale epochs/sequences are rejected at enqueue
 * and rechecked by the worker immediately before its nonblocking send. */
typedef struct dc_io_lights dc_io_lights;
dc_io_lights *dc_io_lights_create(int trusted_fd,const char *socket_identity);
int dc_io_lights_set(dc_io_lights *,dc_tag,uint64_t state_sequence,enum dc_state);
/* No implicit END on I/O failure or destroy; core must explicitly close its
 * current epoch after all session resources have really been released. A
 * queued explicit state gets one final nonblocking send during destroy. */
void dc_io_lights_destroy(dc_io_lights *);
uint64_t dc_io_lights_sent(const dc_io_lights *);
uint64_t dc_io_lights_failed(const dc_io_lights *);

#endif

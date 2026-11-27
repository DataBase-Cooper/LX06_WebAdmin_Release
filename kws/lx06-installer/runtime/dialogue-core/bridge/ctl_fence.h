/* Local API2, PROVISIONAL(P-20261111-1938-B/P-20261112-2008-B). Isolated CTL audio admission.
 * No sockets, audio IO, timers, business OPENs or producer-fence fabrication.
 * One serial owner, immutable/disjoint borrowed state/buffers. No reentry.
 * Owner must validate READY audio independently, send encoded controls ONCE,
 * close on send uncertainty, and keep original CTL/discovery liveness running.
 * Packet/state off realtime stack. Never mutate internals to reopen a gate. */
#ifndef LX06_BR_CTL_FENCE_H
#define LX06_BR_CTL_FENCE_H
#include "ctl.h"
enum br_ctl_fence_phase { BR_FENCE_OFF=0,BR_FENCE_ACTIVE,BR_FENCE_FROZEN,
    BR_FENCE_RESUMING,BR_FENCE_CLOSED };
typedef int (*br_ctl_turn_released)(void *,uint64_t run,uint64_t epoch,uint64_t cycle,
    uint64_t cancel_sequence,uint64_t old_generation,uint64_t next_generation);
typedef struct {
    br_ctl *ctl;
    br_ctl_turn_released released; void *context;
    uint64_t run,last_run,epoch,last_epoch,cycle,last_cycle,cancel_sequence,last_cancel;
    uint64_t generation,next_generation,audio_sequence,mic_sequence,now_ms,resume_ms;
    uint64_t cancel_generation; /* original generation of this encoded CANCEL */
    unsigned confirmed_audio,next_audio;
    enum br_ctl_fence_phase phase;
    int acked,reply_ended,busy;
    const char *error; /* fixed diagnostic only */
} br_ctl_fence;
void br_ctl_fence_init(br_ctl_fence *); /* fresh storage ONLY */
/* Current validated ACTIVE CTL + READY authority. No default gen/audio.
 * Fresh greater run/different epoch after CLOSED. 1 accepted, -1 refused. */
int br_ctl_fence_begin(br_ctl_fence *,br_ctl *,uint64_t run,uint64_t generation,
    unsigned confirmed_audio,uint64_t now,br_ctl_turn_released,void *);
/* Control callback adapters: CTL must already be QUIET/RESUMING respectively.
 * 1 encoded: caller sends ONCE. -1 output unchanged. Cancel gates locally as
 * soon as encoded; send failure/uncertainty requires close, never retry. */
int br_ctl_fence_cancel(br_ctl_fence *,uint64_t run,uint64_t cycle,uint64_t sequence,
    uint64_t now,dc_wire_packet *scratch,void *wire,size_t capacity,size_t *length);
int br_ctl_fence_resume(br_ctl_fence *,uint64_t run,uint64_t cycle,uint64_t next_generation,
    unsigned confirmed_audio,uint64_t now,dc_wire_packet *scratch,void *wire,size_t capacity,size_t *length);
/* Validated authenticated WS binary message, current LOCAL callback identity.
 * AUDIO=1 requires confirmed speaker (ring is local ringtone only) and leaves
 * decoded packet for caller's existing player admission;
 * REPLY_END=2, ACK=3, restored RESUMED=4, safe PCM drop=0, refused=-1.
 * Scratch erased except AUDIO=1. Old local callbacks do not touch live clock.
 * Exact ACK/RESUMED duplicates of the CURRENT cycle return 0, no owner/event
 * repetition. After resume, old ACK uses original cancel_generation. Conflict
 * still closes; no previous-cycle history/timeout extension/retry authority.
 * ACK is not a producer/local release proof. RESUMED requires released()==1
 * AND successful original CTL restore before new generation is admitted. */
int br_ctl_fence_receive(br_ctl_fence *,uint64_t run,uint64_t cycle,uint64_t now,
    const void *,size_t,dc_wire_packet *scratch);
/* Existing MIC producer only; no reader creation. S16LE/16k/mono, even 2..8192B.
 * 1 encoded, -1 refused; no sequence consumed on invalid output/capacity. */
int br_ctl_fence_mic(br_ctl_fence *,uint64_t run,uint64_t cycle,uint64_t sequence,
    uint64_t now,const void *pcm,size_t,dc_wire_packet *scratch,
    void *wire,size_t capacity,size_t *length);
int br_ctl_fence_poll(br_ctl_fence *,uint64_t run,uint64_t cycle,uint64_t now);
/* Close admission first, then original CTL disconnect/owned cleanup. Never
 * claims resources reaped. Stale identity refused before clock checks. */
int br_ctl_fence_close(br_ctl_fence *,uint64_t run,uint64_t cycle,uint64_t now);
#endif

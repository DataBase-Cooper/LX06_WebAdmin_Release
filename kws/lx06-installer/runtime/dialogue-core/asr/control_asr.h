#ifndef LX06_CONTROL_ASR_H
#define LX06_CONTROL_ASR_H
#include <stddef.h>
#include <stdint.h>
#include "question-endpoint-vad.h"
typedef struct lx06_control_asr lx06_control_asr;
/* Tag recognition at the first partial, so a delayed final from listening
 * cannot cancel an unrelated response that has since started. Zero is idle. */
void lx06_control_asr_reply(lx06_control_asr *asr, uint64_t reply);
uint64_t lx06_control_asr_result_reply(const lx06_control_asr *asr);
/* Monotonic decoder-turn identity, independent of reply generations. Partial
 * and FINAL from a turn retain the same identity even if playback is canceled.
 * Zero means no corresponding accepted observation. Core-only extension. */
uint64_t lx06_control_asr_partial_serial(const lx06_control_asr *asr);
uint64_t lx06_control_asr_result_serial(const lx06_control_asr *asr);
uint64_t lx06_control_asr_active_serial(const lx06_control_asr *asr);
/* Bind the first observed near-end voice to the then-current reply. This is
 * identity metadata only; it neither interrupts nor submits a question. */
void lx06_control_asr_bind_source(lx06_control_asr *asr);
/* Optional observation for barge clients. Never an accepted final question.
 * Existing Xiaozhi/Doubao callers do not opt in and retain their behavior. */
const char *lx06_control_asr_partial(const lx06_control_asr *asr, uint64_t *reply);
/* NULL on unavailable service; callers must not silently enable native barge-in. */
lx06_control_asr *lx06_control_asr_open(const char *path);
/* Explicit opt-in for complete questions. Existing callers retain short
 * control-word endpointing. Unknown profiles fail, never silently change it. */
enum lx06_asr_profile { LX06_ASR_CONTROL=0, LX06_ASR_QUESTION=1 };
lx06_control_asr *lx06_control_asr_open_profile(const char *path,enum lx06_asr_profile);
/* Experimental account QUESTION-profile scheduling only; callers opt in.
 * Yield an entirely quiet auxiliary turn before first-sentence native TTS.
 * quiet_since is the first continuously observed quiet PCM timestamp, not a
 * wall-clock guess: it must cover >=300 ms and the whole current ASR turn.
 * No partial/ended utterance or preexisting buffered audio may be discarded.
 * 1=held, 0=refused/no change, -1=IPC failure. hold_ms is bounded to 1..1000.
 * While held, QUESTION retains up to four seconds of PCM (the extra two
 * seconds allocate only on actual backlog; allocation failure retains the
 * newest two seconds). New local voice or
 * synthesis completion must call resume BEFORE feeding that frame. Resume
 * invalidates only the held quiet turn, then next feed immediately starts and
 * replays buffered PCM. Late results from that old dialog cannot act.
 * CONTROL and callers that never invoke these APIs retain their behavior. */
int lx06_control_asr_tts_yield(lx06_control_asr *,uint64_t now,unsigned hold_ms,uint64_t quiet_since);
int lx06_control_asr_tts_resume(lx06_control_asr *,uint64_t now);
/* QUESTION-only TTS handoff with lossless replay of the current encoded turn.
 * Account callers MUST restrict this to the first reply phrase, before any
 * playback and without an observed user partial. Calling it during playback
 * or prefetch caused the 2026-10-31 control-word regression. The transport's
 * replay capability alone is not evidence that repeated handoff is safe.
 * Enable before the first feed. No level threshold: even weak speech without a
 * cloud partial remains in the replay. Provisional partials replay too, with
 * their original reply tag; an ending turn awaiting a final is left alone.
 * priority() cancels only our own auxiliary dialog, holds at most 500ms,
 * then restarts with the full original Opus stream plus newly buffered PCM.
 * Repeated low-level calls never extend the original deadline. Both APIs
 * return -1 on failure, 0 declined/disabled, 1 enabled/held.
 * CONTROL users allocate no replay storage and keep their existing behavior. */
int lx06_control_asr_tts_priority_enable(lx06_control_asr *);
int lx06_control_asr_tts_priority(lx06_control_asr *,uint64_t now,unsigned hold_ms);
/* QUESTION-only hint for a configured, non-echo control in the CURRENT
 * partial. This requests the released 1600ms text-stable FINAL endpoint even
 * under playback noise. It neither stops playback nor accepts partial text.
 * Clear/recompute after every poll. CONTROL ignores it; ordinary questions
 * retain the stronger complete-sentence PCM/silence gate. */
void lx06_control_asr_control_hint(lx06_control_asr *,int matched);
/* Feed continuously during a keyword-mode session, including listening.
 * PCM is mono s16le at 16kHz. Passing 0 explicitly cancels the auxiliary turn
 * and invalidates its results; do not do this merely because playback ended. */
int lx06_control_asr_feed(lx06_control_asr *,int protect,const void *,size_t,uint64_t now);
/* 1=one matching final, 0=no final, -1=service failure. Never returns partials. */
int lx06_control_asr_poll(lx06_control_asr *,char *,size_t,uint64_t now);
void lx06_control_asr_close(lx06_control_asr *);
int lx06_control_asr_feed_stamped(lx06_control_asr *,int,const void *,size_t,const struct qev_stamp *,uint64_t);
void lx06_control_asr_mic_pending(lx06_control_asr *,size_t);
#endif

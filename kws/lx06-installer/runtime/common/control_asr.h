#ifndef LX06_CONTROL_ASR_H
#define LX06_CONTROL_ASR_H
#include <stddef.h>
#include <stdint.h>
typedef struct lx06_control_asr lx06_control_asr;
/* Tag recognition at the first partial, so a delayed final from listening
 * cannot cancel an unrelated response that has since started. Zero is idle. */
void lx06_control_asr_reply(lx06_control_asr *asr, uint64_t reply);
uint64_t lx06_control_asr_result_reply(const lx06_control_asr *asr);
/* Optional observation for barge clients. Never an accepted final question.
 * Existing Xiaozhi/Doubao callers do not opt in and retain their behavior. */
const char *lx06_control_asr_partial(const lx06_control_asr *asr, uint64_t *reply);
/* Ask for a final on the current utterance after the caller observes quiet.
 * This never promotes the current hypothesis to a final. */
int lx06_control_asr_end_utterance(lx06_control_asr *, uint64_t reply, uint64_t now);
/* NULL on unavailable service; callers must not silently enable native barge-in. */
lx06_control_asr *lx06_control_asr_open(const char *path);
/* Explicit opt-in for complete questions. Existing callers retain short
 * control-word endpointing. Unknown profiles fail, never silently change it. */
/* WINDOW is the listen-window / control-window profile (WT1). CONTROL and
 * QUESTION keep their existing timing. Any other value still fails open. */
enum lx06_asr_profile { LX06_ASR_CONTROL=0, LX06_ASR_QUESTION=1, LX06_ASR_WINDOW=2 };
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
/* Live WINDOW readers only. Legacy CONTROL/QUESTION sessions are not counted:
 * those callers already keep an engine for their own interruption path. */
int lx06_control_asr_window_readers(void);
/* WINDOW only. Compatibility setter for post-speech max: 0 selects 10000,
 * clamp 3000..15000. Leaves the no-speech-start timeout unchanged. */
void lx06_control_asr_window_ms(lx06_control_asr *, unsigned ms);
/* WINDOW only. 0 selects 5000/10000; clamps 1000..15000 / 3000..15000. */
void lx06_control_asr_window_timing(lx06_control_asr *, unsigned start_ms, unsigned max_ms);
/* WINDOW PCM speech observation; writes its monotonic start timestamp. */
int lx06_control_asr_speech_seen(const lx06_control_asr *, uint64_t *started);
#endif

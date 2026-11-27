/* Bounded composition of the original A source and B16/session. No new
 * executor/store/identity allocator. Sole serial owner, off audio thread. */
#ifndef LX06_BR_PROFILE_OWNER_H
#define LX06_BR_PROFILE_OWNER_H
#include "profile.h"
#include "../mode_gate.h"
struct eo_saved_source;
struct cfg_bridge_app;
typedef struct br_profile_owner br_profile_owner;
br_profile_owner *br_profile_owner_new(br_session_io);
/* Refuses destruction while original tickets remain owned. */
int br_profile_owner_free(br_profile_owner *);
int br_profile_owner_start(br_profile_owner *,struct eo_saved_source *,const dc_mode_request *,
    uint64_t run,uint64_t epoch,const void *caps_open,size_t);
int br_profile_owner_pump(br_profile_owner *,uint64_t,uint64_t);
/* Borrow the original profile fence for the current source/run only. Old
 * command-only callers still refuse media through the unchanged pump. */
int br_profile_owner_pump_media(br_profile_owner *,uint64_t,uint64_t,br_profile_fence_rx,void *);
int br_profile_owner_start_one(br_profile_owner *,struct eo_saved_source *,const dc_mode_request *,uint64_t,uint64_t,const void *,size_t);
int br_profile_owner_send_one(br_profile_owner *,uint64_t,uint64_t,const void *,size_t);
/* Same retained generation-zero call.start command only. Before terminal
 * READY, a real correlated started GRANT permits setup MIC under the already
 * caps-validated explicit vector; this remains pending, never READY/audio
 * confirmation. After READY the complete confirmed/requested vectors must
 * agree. Source/transport stay current. 1 allowed, 0 denied, negative stale.
 * Revoke is local and precedes capture/player teardown. PCM is original 16kHz
 * mono S16LE, 10/20/30ms; sequence is the producer's increasing capture sequence.
 * No queue, OPEN, ASR, turn or full-call fence is created here. */
int br_profile_owner_microphone_allowed(br_profile_owner *,uint64_t,uint64_t);
void br_profile_owner_microphone_revoke(br_profile_owner *,uint64_t,uint64_t);
int br_profile_owner_microphone(br_profile_owner *,uint64_t,uint64_t,uint64_t,
    const void *,size_t);

const br_operation *br_profile_owner_operation(const br_profile_owner *);
int br_profile_owner_send(br_profile_owner *,uint64_t,uint64_t,uint64_t,const void *,size_t);
int br_profile_owner_release(br_profile_owner *,uint64_t,uint64_t);
void br_profile_owner_close(br_profile_owner *,uint64_t,uint64_t);
/* Exact current owner's transport observation only; retains all session
 * tickets, original source/lock obligations and UNKNOWN policy. */
int br_profile_owner_transport_closed(const br_profile_owner *,uint64_t,uint64_t);
struct selected_source_token;
/* Original source capture's process-local observation only. No job binding,
 * receipt, reaped callback, UNKNOWN release or production factory is added. */
struct selected_source_token *br_profile_owner_source_token(br_profile_owner *,uint64_t,uint64_t,const void *);
int br_profile_owner_controls(br_profile_owner *,uint64_t,uint64_t,struct cfg_bridge_app *);
const br_session *br_profile_owner_session(const br_profile_owner *);
const br_discovery *br_profile_owner_discovery(const br_profile_owner *);
#endif

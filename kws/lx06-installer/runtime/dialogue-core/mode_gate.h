/* Trusted internal phone/action input. No app execution, credentials or audio.
 * The router must provide the actual matched wake metadata in one private
 * inherited SOCK_SEQPACKET message. Passing a filename/argv phrase is not a
 * substitute for integrating that router. Current phone media stays disabled. */
#ifndef LX06_DC_MODE_GATE_H
#define LX06_DC_MODE_GATE_H
#include "protocol.h"
#include <stddef.h>
#define DC_MODE_REQUEST_CAP 2048u
enum dc_mode_kind { DC_MODE_PHONE=1, DC_MODE_ACTION_RESULT };
typedef struct dc_mode_request {
    char script[65],source[17],wake[97],target[17],action[65],status[9];
} dc_mode_request;
typedef struct dc_phone_gate {
    uint64_t epoch,deadline_ms;
    char command_id[32],ack_code[65];
    unsigned ack_state,capability_seen,capable,replayed,terminal;
} dc_phone_gate;
/* Existing JSON string escaping used by both legacy and normal OPEN. */
int dc_mode_escape_text(const char *,char *,size_t);
int dc_mode_parse_request(enum dc_mode_kind,const void *,size_t,dc_mode_request *);
/* Nonblocking, one datagram, root-owned local peer, no ancillary descriptors.
 * Caller must close the consumed descriptor before starting any worker. */
int dc_mode_read_request(int fd,enum dc_mode_kind,dc_mode_request *);
/* Builds the exact v2 OPEN. epoch/deadline are allocated by the core, not the
 * uploaded script; the request's wake/script/source are preserved verbatim. */
int dc_mode_phone_open(const dc_mode_request *,uint64_t epoch,uint64_t wall_ms,
                       uint64_t mono_ms,dc_phone_gate *,char *,size_t);
/* 0 metadata accepted, 1 valid readiness seam, -1 refusal. A false capability,
 * replay-only history, failed ACK, malformed/early READY or timeout cannot
 * mean ready; a matching replay after genuine completion is harmless metadata.
 * 1 is still not permission to run media: current worker has no turn fence. */
int dc_mode_phone_event(dc_phone_gate *,const dc_wire_packet *,uint64_t mono_ms);
int dc_mode_phone_tick(dc_phone_gate *,uint64_t mono_ms);
const char *dc_mode_action_text(const dc_mode_request *);
/* Prepare the limit in the parent; isolate is child-only async-safe syscalls.
 * It closes the inherited lease/light/input and all other descriptors >=3,
 * leaving only target (normally3); stdin/stdout point at the supplied sink. */
int dc_mode_child_limit(void);
int dc_mode_child_isolate(int source,int target,int sink,int limit);
/* Internal C only. Receiver consumes the source owner's current native match;
 * loaded files and request metadata alone never authorize selection. No
 * serialized marker/loan/credential extension to dc_mode_request or old probe. */
struct eo_saved_source;
struct br_profile_owner;
int dc_main_selected_start(struct br_profile_owner *,struct eo_saved_source *,
    const dc_mode_request *,uint64_t,uint64_t,const void *,size_t);
struct dc_phone_runtime;
/* Borrowed normal-main media ownership. Only a verified READY on the same
 * retained command can call audio; the packet remains borrowed for that call.
 * reaped observes the existing main player STOPPED/empty slot for THIS epoch,
 * never whole-process or remote-hook idleness. No new player or queue. */
typedef struct dc_phone_media_io {
    void *context;
    int (*audio)(void *,const dc_wire_packet *);
    int (*reaped)(void *,uint64_t run,uint64_t epoch);
} dc_phone_media_io;
/* Normal --phone-fd lifecycle; caller already verified original session-run and
 * consumed the private request FD. The existing reactor serializes ticks. */
struct dc_phone_runtime *dc_phone_runtime_open(const dc_mode_request *,uint64_t,uint64_t,const dc_phone_media_io *);
/* 0 pending, 1 complete, 2 local close needed, -1 failed. A close result does
 * not release source until the main's actual media observation succeeds. */
int dc_phone_runtime_tick(struct dc_phone_runtime *,uint64_t);
void dc_phone_runtime_close(struct dc_phone_runtime *);
int dc_phone_runtime_destroy(struct dc_phone_runtime *);
int dc_phone_selected_start(struct br_profile_owner *,struct eo_saved_source *,
    const dc_mode_request *,uint64_t,uint64_t,const void *,size_t);
#endif

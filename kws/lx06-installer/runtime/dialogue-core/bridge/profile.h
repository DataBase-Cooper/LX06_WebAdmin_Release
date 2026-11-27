/* t267 B16 isolated formal-profile candidate. No production/core/legacy probe
 * wiring. One serial owner, one object for the process; no audio/settings IO.
 * All calls off the realtime/audio thread. dial <=10s and each submit <=2s
 * in userspace socket waits; pump is nonblocking apart from control replies.
 * No retries/reconnect, no new queue or Ledger. TLS allocations/RSS and OEM
 * OpenSSL 1.0/glibc2.25 link/load remain separate release/device gates.
 * Executable owner must ignore SIGPIPE, as phone_worker already does; this
 * library does not install process-wide handlers. Fixed-session attachment
 * retains existing PROVISIONAL D1/D2 contracts and current feature admission;
 * this candidate does not approve new peer feature/echo obligations. */
#ifndef LX06_BR_PROFILE_H
#define LX06_BR_PROFILE_H
#include "session.h"
typedef struct br_profile br_profile;
typedef struct {
    const char *host; /* literal private/loopback IPv4, no DNS or URL */
    unsigned port; /* 1..65535; caller resolves script default 18765 */
    const char *ca_file; /* caller's pinned, stable private trust input */
    const char *token; /* 32..128 ASCII URL-safe bytes; never retained */
    unsigned char spki_sha256[32]; /* SHA256 of DER SubjectPublicKeyInfo */
} br_profile_config;
enum { BR_PROFILE_INVALID=-1,BR_PROFILE_CLOSED=-2,BR_PROFILE_IDLE=0,
       BR_PROFILE_CONNECTED=1 };
br_profile *br_profile_new(void);
void br_profile_free(br_profile *);
/* New strictly increasing run+epoch chosen by sole identity owner. A failed
 * attempt consumes both, never reuse. New dial closes old transport/discovery
 * and requests old session close, retaining owned ticket until original reap.
 * Caller must keep discovery/session and config/CA stable during calls; they
 * outlive transport. No profile-file or script parser. caps_open is A's fully
 * resolved envelope, checked by shared K3. First Wire frame is caps.request.
 * Success = authenticated+OPEN submitted, NOT caps/business/audio READY. */
int br_profile_dial(br_profile *,const br_profile_config *,br_discovery *,
    uint64_t run,uint64_t epoch,const void *caps_open,size_t);
/* Internal source-owner composition. guard is the ORIGINAL retained source
 * current check, not caller permission. It outlives transport and is checked
 * before effects, including after blocking TLS waits. Legacy dial is separate. */
typedef int (*br_profile_source_current)(void *);
int br_profile_dial_source(br_profile *,const br_profile_config *,br_discovery *,
    uint64_t run,uint64_t epoch,const void *caps_open,size_t,
    br_profile_source_current,void *);
/* After BR_DISC_CAPS, attach existing br_session begun on THIS discovery/run.
 * Uses original session receive/poll. No replacement/ownership fabrication. */
int br_profile_session(br_profile *,uint64_t run,uint64_t epoch,br_session *);
/* Current frozen protocol: one generation-zero operation on this authenticated
 * connection. Never enables same-session CTL or bypasses br_session admission. */
int br_profile_operation(br_profile *,uint64_t,uint64_t,br_operation *);
/* Optional original-session event borrow, AFTER shared receive/poll and BEFORE
 * transport cleanup erases its response. Sole serial FLOW owner only; no
 * profile reentry, send, retained pointers or fabricated receive event.
 * Closed event follows actual original session close and preserves UNKNOWN.
 * Old identity refuses before mutation; callback/context outlive the profile. */
typedef void (*br_profile_session_event)(void *,const br_session *,uint64_t now,int event);
int br_profile_events(br_profile *,uint64_t run,uint64_t epoch,br_profile_session_event,void *);
/* br_session_send callback and CTL's sole transport submit. Accepts complete
 * validated Wire bytes from original session/fence encoder, sends once; no
 * delivery/ACK authority. Invalid local identity leaves successor untouched.
 * Any current transport failure closes, never retry these bytes. */
int br_profile_submit(void *,uint64_t run,uint64_t epoch,const void *,size_t);
/* One message per call; events use BR_DISC_* before session, BR_OP_* after.
 * Pass current run/epoch. Old callback is refused before clocks/state change.
 * For audio/fence-only frames caller supplies its ORIGINAL fence consumer;
 * strict nonnegative return accepts, negative closes. No receive callback
 * means unsupported frames close. It must not reenter/mutate this profile.
 * Output payload is borrowed ONLY during callback. */
typedef int (*br_profile_fence_rx)(void *,uint64_t,uint64_t,const void *,size_t);
int br_profile_pump(br_profile *,uint64_t run,uint64_t epoch,
                    br_profile_fence_rx,void *);
void br_profile_close(br_profile *,uint64_t run,uint64_t epoch);
/* Read-only, same serialized owner. 1 only after this run/epoch's actual
 * socket/TLS objects and borrowed bindings have all gone and buffers cleared;
 * 0 while any remain (including a callback or a busy refused close); -1 for
 * null/old identity. Safe inspection during an event borrow; no IO or mutation.
 * This is transport scope only. It supplies no ticket receipt/reap, player,
 * microphone, SDK, child or global-idle authority. A failed dial can close its
 * own partial transport, but an unused object has no qualifying identity. */
int br_profile_transport_closed(const br_profile *,uint64_t run,uint64_t epoch);
const char *br_profile_error(const br_profile *); /* fixed names, no secrets */
#endif

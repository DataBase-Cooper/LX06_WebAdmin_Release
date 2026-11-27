/* T18 isolated local executor. PROVISIONAL(P-20261111-1245-B).
 * A owns effective configuration and atomic sound ownership. Not a Wire
 * endpoint, settings parser, phone Ledger or production profile attachment. */
#ifndef LX06_BR_SYSTEM_OP_H
#define LX06_BR_SYSTEM_OP_H
#include "../../common/bridge-contract/caps.h"
#include "system_file.h"
enum br_system_kind { BR_SYSTEM_TTS=1,BR_SYSTEM_FILE=2,BR_SYSTEM_ACK=3 };
enum br_system_status { BR_SYSTEM_IDLE=0,BR_SYSTEM_QUEUED=1,BR_SYSTEM_RUNNING=2,
    BR_SYSTEM_DRAINING=3,BR_SYSTEM_DONE=4,BR_SYSTEM_FAILED=5,BR_SYSTEM_CANCELLED=6 };
enum br_system_io_status { BR_SYSTEM_IO_RUNNING=0,BR_SYSTEM_IO_DONE=1,
    BR_SYSTEM_IO_FAILED=-1,BR_SYSTEM_IO_CANCELLED=-2 };
typedef struct {
    const char *op;
    enum br_system_kind kind;
    uint64_t timeout_ms; /* 1..120000, no default */
    int root; /* FILE only: owner-authorized resource directory FD */
    const char *const *paths; size_t path_count; /* FILE only, exact allowlist */
} br_system_rule;
typedef struct {
    void *context;
    /* start: atomic all-idle check + sound owner acquisition. 1 started,
     * 0 busy/no side effects, -1 failed (may hold resources: MUST poll).
     * Text and readonly FD are BORROWED until terminal poll/release. */
    int (*start_tts)(void *,uint64_t ticket,const char *,uint64_t now,uint64_t remaining_ms);
    int (*start_file)(void *,uint64_t ticket,int fd,uint64_t now,uint64_t remaining_ms);
    /* Terminal only after actual drain, backend reaping AND owner release. */
    enum br_system_io_status (*poll)(void *,uint64_t ticket,uint64_t now);
    void (*cancel)(void *,uint64_t ticket,uint64_t now); /* owned only, nonblocking */
} br_system_io;
typedef struct {
    br_system_io io;
    const br_caps *caps;
    uint64_t epoch,ticket,last_ticket,started_ms,now_ms,timeout_ms;
    enum br_system_kind kind;
    enum br_system_status status,terminal;
    const char *error; /* fixed static name, never payload/path */
    int fd;
    unsigned char request[LX06_BRIDGE_PAYLOAD_MAX+1]; size_t request_length;
    char text[2049];
} br_system;
/* Call init ONCE on fresh storage. Never re-init an unreaped operation. No
 * heap/thread allocation. State + caller scratch live outside realtime stack.
 * caps remains alive, immutable between serial owner calls. No aliasing of
 * input/state/caps/scratch. Backend callbacks must not reenter these APIs. */
int br_system_init(br_system *,br_system_io);
/* 1 accepted (NOT completed), 0 active/busy, -1 invalid. New ticket must be
 * strictly greater than this instance's last accepted ticket; owner supplies
 * globally fresh identities across restarts. Rule/path tables borrowed only
 * for this call. Accepted FILE FD is pinned immediately. No audio starts here.
 * ACK completes locally without audio and never produces a Wire GRANT. */
int br_system_submit(br_system *,const br_caps *,uint64_t epoch,uint64_t ticket,
                     uint64_t now,const void *,size_t,const br_system_rule *,size_t,br_json *scratch);
/* Returns local status. Old tickets ignored before clock/preempt checks.
 * Queue timeout includes busy wait. caps invalidation/preemption stops work;
 * DRAINING remains until backend proves resources released. No auto-retry.
 * Poll scratch must be disjoint and is wiped before returning. */
int br_system_poll(br_system *,uint64_t ticket,uint64_t now,int preempt,br_json *scratch);
int br_system_cancel(br_system *,uint64_t ticket,uint64_t now,br_json *scratch);
#endif

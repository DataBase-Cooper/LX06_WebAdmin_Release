/* T19 local lifecycle, API 3. PROVISIONAL(P-20261112-2234-B), inherits API2.
 * End work clarification: PROVISIONAL(P-20261111-2048-B).
 * Isolated: injected A matcher/window/effective settings and transport owner.
 * No speech recognition, matching, settings parser, phone Ledger or sockets.
 * Serial callbacks; nested CTL APIs refuse before clock/scratch/IO mutation.
 * Fresh init is never a callback API. Disjoint input/state/scratch/caps buffers;
 * allocate state/scratch off realtime stack. Keep borrowed caps storage alive
 * through drain. Owner-discovered ready/connection revocation is checked after
 * lifecycle callbacks before advancing; never replace caps to grant a new run.
 * A possibly accepted start retains work_owned until later strict reap; zero
 * still means no side effects. Cleanup remains possible after connection loss. */
#ifndef LX06_BR_CTL_H
#define LX06_BR_CTL_H
#include "flow.h"
enum br_ctl_phase { BR_CTL_IDLE=0,BR_CTL_ACTIVE,BR_CTL_QUIET,BR_CTL_WINDOW,
    BR_CTL_RELEASE,BR_CTL_START,BR_CTL_WORK,BR_CTL_WORK_DRAIN,BR_CTL_RESUMING,
    BR_CTL_DRAINING,BR_CTL_CLOSED };
enum br_ctl_match { BR_CTL_TIMEOUT=1,BR_CTL_MISS,BR_CTL_ASR_FAILED,BR_CTL_EXIT,
    BR_CTL_OP,BR_CTL_FLOW,BR_CTL_END };
enum br_ctl_proof { BR_CTL_READY=1,BR_CTL_LEDGER,BR_CTL_FLOW_DONE,BR_CTL_FAILED };
typedef struct { int declared,timeout_end; unsigned start_ms,max_ms; } br_ctl_config;
typedef struct {
    enum br_ctl_match kind;
    const void *open; size_t length; /* OP only: full resolved OPEN */
    const br_flow_definition *flow; const br_flow_binding *inputs; size_t input_count;
} br_ctl_route;
typedef struct {
    void *context;
    /* quiesce synchronously closes speaker/MIC admission + barge KWS, even on
     * failure. quiet=1 proves actual output/reader release, not just request. */
    int (*quiesce)(void *,uint64_t cycle);
    int (*quiet)(void *,uint64_t cycle);
    int (*window_open)(void *,uint64_t cycle,const char *script,unsigned start,unsigned max);
    void (*window_close)(void *,uint64_t cycle);
    int (*window_reaped)(void *,uint64_t cycle);
    /* start=0 busy/no side effects, 1 accepted, -1 uncertain; NEVER retry -1.
     * Must use shared caps/operation/flow admission, send once, authenticate
     * responses and retain original identity until reaped. END has end_op.
     * Isolated session adapter supports OP/END only. Production wiring and
     * FLOW/session ticket adapter remain pending. */
    int (*work_start)(void *,uint64_t cycle,uint64_t job,const br_ctl_route *,
                      br_text end_op,uint64_t now,uint64_t remaining);
    void (*work_cancel)(void *,uint64_t cycle,uint64_t job);
    int (*work_reaped)(void *,uint64_t cycle,uint64_t job);
    /* Sends existing type only; local snapshots/tickets are NOT Wire fields. */
    int (*control)(void *,unsigned type,uint64_t epoch);
    /* Atomically restore only after RESUMED. Failure MUST keep gates closed. */
    int (*restore)(void *,const void *local_state,size_t);
    void (*teardown)(void *,uint64_t run); /* immediate owned cleanup request */
    int (*reaped)(void *,uint64_t run);
    void (*notice)(void *,enum br_ctl_match); /* enqueue K5 category, no playback */
} br_ctl_io;
typedef struct {
    br_ctl_io io; const br_caps *caps; br_ctl_config config; br_ctl_route route;
    enum br_ctl_phase phase; enum br_ctl_match decision;
    uint64_t run,last_run,epoch,cycle,last_cycle,job,now_ms,since_ms,timeout_ms;
    char script[129]; br_text app,end_op; uint64_t end_timeout;
    unsigned char state[8193],s0[8193],request[8193];
    size_t state_length,s0_length,request_length;
    int window_owned,window_closed,work_owned,work_cancelled,ending,ends,oneway;
    int restore_s0,work_ok,close_sent,wake_sent,end_oneway,busy;
    const char *error; /* fixed diagnostics only */
} br_ctl;
int br_ctl_init(br_ctl *,br_ctl_io); /* fresh storage only */
/* Begin only idle/closed; app is a current caps app. Requires a session.cancel
 * declaration for enabled CTL. No CTL: barge returns 0, no IO/legacy changes. */
int br_ctl_begin(br_ctl *,uint64_t run,uint64_t epoch,uint64_t now,
                 const br_caps *,const char *app,const char *script,br_ctl_config);
/* Full last READY/STATE opaque object, never business interpretation. */
int br_ctl_state(br_ctl *,uint64_t run,uint64_t epoch,uint64_t now,
                 const void *,size_t,br_json *scratch);
int br_ctl_barge(br_ctl *,uint64_t run,uint64_t cycle,uint64_t now);
/* A's already scoped match, no raw ASR text. OP copied, FLOW typed definitions
 * borrowed through resource drain. Window close issued immediately. */
int br_ctl_window(br_ctl *,uint64_t run,uint64_t cycle,uint64_t now,
                  const br_ctl_route *,br_json *scratch);
/* READY only for synchronous OP; original associated durable LEDGER for
 * oneway; FLOW_DONE only from terminal/reaped stepper. ACK/progress never call
 * this. ended strict bool is used only for flow. State optional opaque JSON. */
int br_ctl_work(br_ctl *,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now,
                enum br_ctl_proof,int ended,const void *,size_t,br_json *scratch);
/* Transport owner has validated current RESUMED + generation/audio fence.
 * Local cycle is callback identity, not proof of a phone echoed field. */
int br_ctl_resumed(br_ctl *,uint64_t run,uint64_t cycle,uint64_t epoch,uint64_t now);
int br_ctl_official(br_ctl *,uint64_t run,uint64_t now,int enabled);
int br_ctl_disconnect(br_ctl *,uint64_t run,uint64_t epoch,uint64_t now);
int br_ctl_poll(br_ctl *,uint64_t run,uint64_t now,br_json *scratch);
int br_ctl_clear(br_ctl *); /* no reset of monotonic run/cycle counters */
#endif

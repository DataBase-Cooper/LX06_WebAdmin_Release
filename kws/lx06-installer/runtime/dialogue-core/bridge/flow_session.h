/* Local API1, PROVISIONAL(P-20261111-2315-B/P-20261112-1851-B). Sole serial FLOW/session adapter.
 * Reusable production source, isolated before T14/A/C signoff. No sockets,
 * auth, audio, persistence, settings or Ledger. Fresh init only, no reentry.
 * Owner may begin/output the flow via original APIs; all current FLOW IO,
 * dispatch/poll/clear MUST use this adapter. Never mutate borrowed state.
 * Idle-session TASK/heartbeats remain with the sole session transport owner;
 * this adapter does not own or stop global K6 result collection.
 * session/discovery, wire (exactly 8240B), config and callbacks outlive drain.
 * All disjoint except the documented discovery.scratch serial FLOW borrow.
 * Callback views expire on return; never retain or mutate them asynchronously. */
#ifndef LX06_BR_FLOW_SESSION_H
#define LX06_BR_FLOW_SESSION_H
#include "flow.h"
#include "session.h"
#define BR_FLOW_SESSION_WIRE 8240u
typedef struct {
    void *context;
    /* Via br_session_send's sole submit boundary, original ticket already owned.
     * Send ONCE. Strict 1 = entire encoded frame copied/accepted by transport.
     * All other returns are uncertain failures, even 0. NOT an ACK. */
    int (*send)(void *,uint64_t flow_run,uint64_t session_run,uint64_t ticket,
                uint64_t epoch,uint64_t generation,const void *,size_t);
    /* Synchronous original event, BEFORE flow can cancel/wipe operation.
     * Owner routes TASK to K6 / quota ERROR prose to existing K5, no bypass
     * of their original gates. No reentry or mutation of adapter state. */
    void (*event)(void *,uint64_t flow_run,uint64_t ticket,const br_operation *,int);
} br_flow_session_io;
typedef struct {
    br_flow *flow;br_session *session;unsigned char *wire;
    br_flow_session_io io;
    uint64_t run,session_run,epoch,generation,ticket;
    unsigned slot;
    int bound,active,busy;
} br_flow_session;
int br_flow_session_init(br_flow_session *,br_flow *,br_session *,void *wire,size_t,
                          br_flow_session_io);
/* After original flow_begin, before first dispatch; pin original live idle
 * session. All identity arguments nonzero, generation >= session generation. */
int br_flow_session_bind(br_flow_session *,uint64_t run,uint64_t session_run,
                          uint64_t epoch,uint64_t generation);
/* Returns original FLOW status, -1 rejection. Old identity before clock. */
int br_flow_session_dispatch(br_flow_session *,uint64_t run,uint64_t ticket,
                              uint64_t now,const char *command);
int br_flow_session_receive(br_flow_session *,uint64_t run,uint64_t ticket,
                             uint64_t now,const void *,size_t);
/* Original profile has already routed THIS event through this session.
 * No caller-created READY: validate the original operation/ticket before
 * delivering it to FLOW. Same serial borrow and event callback rules. */
int br_flow_session_event(br_flow_session *,uint64_t run,uint64_t ticket,
                           uint64_t now,int event);
int br_flow_session_poll(br_flow_session *,uint64_t run,uint64_t now,int preempt);
int br_flow_session_clear(br_flow_session *);
#endif

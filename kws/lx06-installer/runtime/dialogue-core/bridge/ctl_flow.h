/* Local API1, PROVISIONAL(P-20261112-0027-B). Original CTL FLOW job binding.
 * Isolated before T14/A/C signoff and aggregate memory budget closure.
 * Borrowed modules/buffers are disjoint, immutable and outlive drain, except
 * original discovery.scratch sharing. Fresh init, sole serial owner, no reentry.
 * No parser/ASR/player/Ledger/socket. Route audio/control to the original fence,
 * idle/cancelled TASK to the global transport/K6. Never bypass this adapter to
 * mutate the borrowed flow_session. CTL and FLOW run namespaces are distinct. */
#ifndef LX06_BR_CTL_FLOW_H
#define LX06_BR_CTL_FLOW_H
#include "flow_session.h"
#include "ctl_fence.h"
typedef struct {
    br_flow_session *io;br_ctl_fence *fence;
    uint64_t run,epoch,cycle,job,flow_run,generation,now_ms;
    int active,cancelled,delivered,released,busy;
} br_ctl_flow;
int br_ctl_flow_init(br_ctl_flow *,br_flow_session *,br_ctl_fence *,uint64_t now);
/* Original work_start callback, START/FLOW only. No send until dispatch after
 * CTL exits callback to WORK. Missing CANCEL_ACK: 0, unchanged. 1 accepted,
 * -1 refused. Definition/inputs and source/text must outlive original drain. */
int br_ctl_flow_start(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t flow_run,uint64_t now,const char *source,const char *text);
int br_ctl_flow_dispatch(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t ticket,uint64_t now,const char *command);
int br_ctl_flow_receive(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t ticket,uint64_t now,const void *,size_t);
/* FLOW status or -1. Never enters CTL. Also allowed from work_reaped while
 * cancelling; preserves original owned ticket until strict real release. */
int br_ctl_flow_poll(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now);
int br_ctl_flow_cancel(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job,uint64_t now);
/* Outside CTL callbacks ONLY. Real drained terminal, once. replacement_state
 * is explicit original owner state (JSON object or absent), NOT scalar output;
 * disjoint from all borrowed state/scratch. No copy of private flow output. */
int br_ctl_flow_finish(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job,
    uint64_t now,const void *replacement_state,size_t);
/* Original work_reaped callback, WORK_DRAIN/DRAINING. 1 strict released, 0
 * pending, -1 refused. No CTL recursion or implicit FLOW poll. */
int br_ctl_flow_reaped(br_ctl_flow *,uint64_t run,uint64_t cycle,uint64_t job);
/* Only after CTL work_owned cleared and genuine original drain. Retains
 * original FLOW highwaters. Does not close/clear fence or session. */
int br_ctl_flow_clear(br_ctl_flow *);
#endif

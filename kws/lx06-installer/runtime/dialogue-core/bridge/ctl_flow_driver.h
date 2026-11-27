/* Local API1, PROVISIONAL(P-20261112-0110-B). Isolated serial owner driver.
 * Borrow an initialized ACTIVE ctl_flow; all outer CTL events use apply.
 * Existing lifecycle callbacks use original APIs, NEVER reenter this driver.
 * Same immutable/disjoint buffers and resource lifetimes as ctl_flow. No heap,
 * sockets, parser, matcher, Ledger, timer thread or automatic business send.
 * Global TASK/K5/K6 and authenticated heartbeats remain with the sole transport
 * owner on the same serial executor, including idle/cancelled connections.
 * No production wiring or A/C/OEM/device acceptance is implied. */
#ifndef LX06_BR_CTL_FLOW_DRIVER_H
#define LX06_BR_CTL_FLOW_DRIVER_H
#include "ctl_flow.h"
#define BR_CTL_FLOW_DRIVER_API 1u
enum br_ctl_flow_event_kind {
    BR_DRIVER_TICK=1,BR_DRIVER_STATE,BR_DRIVER_BARGE,BR_DRIVER_ROUTE,
    BR_DRIVER_FENCE,BR_DRIVER_DISPATCH,BR_DRIVER_RECEIVE,BR_DRIVER_FINISH,
    BR_DRIVER_OFFICIAL,BR_DRIVER_DISCONNECT,BR_DRIVER_CLEAR
};
typedef struct {
    enum br_ctl_flow_event_kind kind;
    uint64_t run,epoch,cycle,job,generation,now_ms,ticket,next_cycle;
    const void *data;size_t length;
    const br_ctl_route *route;const char *command;
    int enabled;
} br_ctl_flow_event;
typedef struct {
    br_ctl_flow *adapter;
    uint64_t run,epoch,now_ms;
    int busy;
} br_ctl_flow_driver;
int br_ctl_flow_driver_init(br_ctl_flow_driver *,br_ctl_flow *,uint64_t now);
/* Current local identity BEFORE clock/poll; reject stale or reentrant -1.
 * Returns original event result, TICK original CTL phase. No auto finish or
 * dispatch. FLOW completion still requires explicit owner FINISH state.
 * FENCE=1 leaves original discovery.packet AUDIO view until next apply;
 * caller synchronously feeds existing player/erases it. All other packet views
 * expire on return. Event/data/state must be disjoint from borrowed workspace.
 * CLEAR retains highwaters; BARGE refuses uncleared original FLOW storage. */
int br_ctl_flow_driver_apply(br_ctl_flow_driver *,const br_ctl_flow_event *);
#endif

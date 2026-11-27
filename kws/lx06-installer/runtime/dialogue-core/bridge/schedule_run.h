/* Caller-clocked global scheduler and single phone op adapter, API 7.
 * PROVISIONAL(P-20261112-0900-B): original journal busy rejects poll/recover
 * and single-op events BEFORE any state/time/IO changes in recovery callbacks.
 * PROVISIONAL(P-20261111-1410-B/P-20261111-1436-B/P-20261111-1523-B).
 * No settings/phone Ledger/TTS/socket service.
 * All calls serial. Borrow registry/config/op/inbox through drain. Do not
 * mutate operation/discovery concurrently. Put state off realtime stack. */
#ifndef LX06_BR_SCHEDULE_RUN_H
#define LX06_BR_SCHEDULE_RUN_H
#include "schedule_policy.h"
#include "result_inbox.h"
#include "operation.h"
#include "flow_journal.h"
#define BR_RUN_BASE_BYTES (128u*1024u)
#define BR_RUN_FLOW_BYTES (256u*1024u)

enum br_schedule_run_status { BR_RUN_IDLE=0,BR_RUN_WAIT,BR_RUN_RECEIPT,
    BR_RUN_DRAINING,BR_RUN_UNKNOWN,BR_RUN_BLOCKED };
typedef struct { unsigned script,slot; } br_schedule_candidate;
typedef struct {
    const char *revision,*task_id; /* A's immutable ORIGINAL effective binding */
    const void *open; size_t length; /* full resolved OPEN, borrowed during start */
} br_schedule_command;
typedef struct {
    void *context;
    int (*acquire)(void *,uint64_t run); /* atomic idle + take A owner: 0/1 */
    /* 0 PROVEN zero bytes/no effects, 1 accepted, -1 uncertain. No reentry. */
    int (*send)(void *,uint64_t run,uint64_t epoch,const void *,size_t);
    void (*close)(void *,uint64_t run,uint64_t epoch,uint64_t now);
    int (*reaped)(void *,uint64_t run,uint64_t epoch); /* 1 = all owned released */
} br_schedule_run_io;
typedef struct {
    br_schedule *scripts[BR_SCHEDULE_TASKS]; size_t count;
    br_operation *op; br_schedule_run_io io;
    enum br_schedule_run_status status;
    unsigned script,slot;
    uint64_t run,last_run,serial,epoch,now,started,timeout;
    char command[37];
    int sampled,candidate,closed,oneway,acked,timed_out,flow_active;
    br_schedule_candidate next;
    unsigned char wire[48+LX06_BRIDGE_PAYLOAD_MAX];
    /* Optional one-time global resource registration, NOT execution authority. */
    br_flow *flow; br_json *flow_scratch; br_flow_journal *journal;
    br_json *journal_scratch; br_store *journal_store;
    unsigned char *journal_workspace; size_t journal_capacity;
} br_schedule_runner;
/* Fresh storage. Registry MUST cover all device scripts, including disabled
 * and unreconciled entries, not multiple independent runners. Global <=32
 * tasks; scheduler headers + record capacities + UNIQUE workspaces + runner
 * <=128KiB or fail explicitly. Borrowed mutable storage must not alias.
 * Re-register after reconfigure before obtaining a new candidate. */
int br_schedule_runner_init(br_schedule_runner *,br_schedule_run_io,br_operation *,br_schedule *const *,size_t);
/* PROVISIONAL(P-20261111-1653-B): once, IDLE/no candidate/no prior run.
 * Borrow disjoint flow, scratch, journal + its store/workspace/scratch until
 * runner destruction. Base still <=128KiB; full registered allocation <=256KiB.
 * Reject all overlaps with registry/config/op/discovery and address overflow.
 * 1 registered/-1 rejected, no IO/acquire/send. Does not enable flow dispatch. */
int br_schedule_runner_attach_flow(br_schedule_runner *,br_flow *,br_json *,br_flow_journal *);
/* Read-only actual sizeof/capacity accounting. 1 valid/-1 unsafe, output only
 * on success. Excludes existing op/caps/K6/backend and transient storage heap. */
int br_schedule_runner_memory(br_schedule_runner *,size_t *base,size_t *total);
/* 1 candidate/0 waiting/-1 unsafe. Writes candidate only on 1. Samples all
 * scripts; ANY active/owned/unknown or blocked script prevents new dispatch.
 * No IO except existing schedule persistence. */
int br_schedule_runner_poll(br_schedule_runner *,int trusted,int64_t wall,uint64_t mono,int idle,br_schedule_candidate *);
/* run monotonic and fresh across restarts (owner responsibility). Requires
 * latest poll's exact candidate + mono. Claims durably BEFORE send once.
 * Return status (IDLE = busy/no claim), -1 invalid. No system.op/flow action. */
int br_schedule_runner_start(br_schedule_runner *,uint64_t run,br_schedule_candidate,uint64_t epoch,
                            const char *uuid,uint64_t mono,const br_schedule_command *);
/* Internal shared coordinator claim gate, no dispatch. schedule_flow is the
 * typed consumer; caller must already validate/begin the registered flow.
 * Rechecks registry/budget/idle + current caps, then acquire/INTENT. */
int br_schedule_runner_claim_flow(br_schedule_runner *,uint64_t run,br_schedule_candidate,uint64_t epoch,
                                  const char *uuid,uint64_t mono);
/* Fixed-session owning coordinator only. No legacy journal registration;
 * original begun NEXT flow, same discovery, global acquire/INTENT gate.
 * Does not attach a checkpoint or authorize send/release/retirement. */
int br_schedule_runner_claim_session(br_schedule_runner *,br_flow *,uint64_t,
    br_schedule_candidate,uint64_t,const char *,uint64_t);
/* Recover original unresolved slot without sending. run identity is new,
 * original serial/epoch/command remain unchanged. Only after A recovers owner
 * state. Does not claim that unknown action was never executed. */
int br_schedule_runner_recover(br_schedule_runner *,uint64_t run,unsigned script,unsigned slot,uint64_t mono);
/* Serial control transport wrapper. Old run ignored BEFORE parsing/time. */
int br_schedule_runner_receive(br_schedule_runner *,uint64_t run,uint64_t epoch,uint64_t mono,const void *,size_t);
int br_schedule_runner_tick(br_schedule_runner *,uint64_t run,uint64_t mono,int preempt);
/* API4 PROVISIONAL(P-20261111-1614-B): explicitly adopt P-0532 timeout policy.
 * Original monotonic execution deadline proof required, never inferred from
 * a generic UNKNOWN or restored clock. Does NOT finish/release the schedule.
 * run is checked before time; origin2 owner association is mandatory. */
int br_schedule_runner_timeout_notice(br_schedule_runner *,uint64_t run,uint64_t mono,
                                      br_inbox *,uint64_t wall,int trusted);
/* Sync READY/ERROR only: owner proves original result/output policy durably
 * handled. No fabricated task id/version or second result queue. */
int br_schedule_runner_finish(br_schedule_runner *,uint64_t run,int proof);
/* API2 PROVISIONAL(P-20261111-1523-B): consume current validated synchronous
 * READY.text/ERROR.msg into SAME K6 queue, then save TERMINAL. Returns br_received.
 * Never fabricate remote task/version. Response must still be borrowed/current.
 * Empty text records receipt without playback; audible policy remains frozen. */
int br_schedule_runner_sync(br_schedule_runner *,uint64_t run,br_inbox *,uint64_t wall,int trusted);
/* API3 explicit terminal speech policy; legacy entry selects ORIGINAL. */
int br_schedule_runner_sync_policy(br_schedule_runner *,uint64_t run,br_inbox *,uint64_t wall,int trusted,enum br_speech_policy);
/* Recovered UNKNOWN: only an original already durable local receipt can close
 * it. 1 completed,0 absent,negative invalid/IO. No Wire send or new enqueue. */
int br_schedule_runner_resume_receipt(br_schedule_runner *,uint64_t run,br_inbox *);
/* Authenticated producer supplies shared decoded ORIGINAL result (TASK or
 * task.list); associate checks versions. Existing K6 owns durability/text/TTL.
 * Returns br_received; duplicate is durable. No TEXT/ACK completion. Late
 * originals may resolve UNKNOWN, even after transport has drained. */
int br_schedule_runner_result(br_schedule_runner *,uint64_t run,const br_result *,br_inbox *,
                             int live,uint64_t connection_epoch,uint64_t wall,int trusted);
int br_schedule_runner_result_policy(br_schedule_runner *,uint64_t run,const br_result *,br_inbox *,
                                    int live,uint64_t connection_epoch,uint64_t wall,int trusted,enum br_speech_policy);
#endif

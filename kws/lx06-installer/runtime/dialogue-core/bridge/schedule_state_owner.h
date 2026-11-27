/* Actual local global scheduler quota owner. No production A namespace or
 * idle authority is invented: caller supplies original anchored root/FDs.
 * Existing original schedule/log/owner codecs and proof gates stay in charge. */
#ifndef LX06_BR_SCHEDULE_STATE_OWNER_H
#define LX06_BR_SCHEDULE_STATE_OWNER_H
#include "schedule_state_catalogue.h"
#define BR_SCHEDULE_GLOBAL_STATE_BYTES 65536u
#define BR_SCHEDULE_GLOBAL_HISTORY 64u
#define BR_SCHEDULE_GLOBAL_PAIR_MAX 8741u
typedef struct br_schedule_state_owner br_schedule_state_owner;
typedef struct br_schedule_state_binding {
    br_schedule_state_owner *owner;
    br_store *store;br_schedule *schedule;
    br_store_guard store_guard;br_schedule_guard schedule_guard;
    const char *script,*revision;
    size_t frame,tasks,history,active,held_frame,held_tasks,held_history,held_active;
    size_t seen_frame,seen_tasks,seen_history,seen_active;
    size_t held_pair;uint32_t action_mask;
    int seen,present,pending,claiming,closed;
} br_schedule_state_binding;
struct br_schedule_state_owner {
    int root,parent,lock,poisoned;
    char entry[256];
    size_t attached,reserved_bytes,reserved_tasks,reserved_history,reserved_active;
    br_schedule_workspace *workspace;
    br_schedule_state_binding *bindings;size_t capacity,count,max_entries;
    br_schedule_catalogue_totals actual;
    size_t state_bytes,cold_pair;
    int configured,busy,dirty,readonly,cold_hold;
    br_schedule_state_binding *writing,*editing;
    char target_revision[65];
};
int br_schedule_state_owner_open(br_schedule_state_owner *,int root,int parent,const char *entry);
int br_schedule_state_owner_current(br_schedule_state_owner *);
/* Attach all original current EX writers before the first real namespace
 * scan. slots is borrowed disjoint storage >=count+2. flow/owner are the
 * ORIGINAL global stores in their explicitly authorized directory.
 * Existing over-quota bytes return1/read-only, never normalize/delete state.
 * Same original active identity retains its max future pair reservation. */
int br_schedule_state_owner_bind(br_schedule_state_owner *,br_schedule_workspace *,
    br_schedule_state_binding *slots,size_t capacity,br_schedule *const *schedules,
    br_store *const *stores,size_t count,br_store *flow,br_store *owner,size_t max_entries);
/* Full original catalogue plus actual global framed bytes. No caller list is
 * accepted as catalogue completeness. Uses only idle original shared scratch.
 * Returns1 even when readonly quota is exceeded; unsafe bytes/IO =>-1.
 * Release unused old/new reservation ONLY after complete successful readback. */
int br_schedule_state_owner_reconcile(br_schedule_state_owner *);
/* Only the actual current compact A run-ref may select action(0)/FLOW(1).
 * This does not claim or authorize a send; original claim rechecks the slot. */
int br_schedule_state_owner_claim_kind(br_schedule_state_owner *,const br_schedule *,unsigned slot,int flow);
int br_schedule_state_owner_contains(const br_schedule_state_owner *,const br_schedule *);
/* Entire current writer set must be genuinely original-unowned/quiescent.
 * Does not close stores, release original sources or manufacture reap proof. */
int br_schedule_state_owner_unbind(br_schedule_state_owner *);
int br_schedule_state_owner_close(br_schedule_state_owner *);
#endif

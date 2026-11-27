/* Local dependency API1. No production namespace owner is installed here.
 * Readonly, serial/off-audio measurement under the ORIGINAL full-namespace
 * writer lease. No recovery, claim, release or cross-file atomicity proof. */
#ifndef LX06_BR_SCHEDULE_STATE_CATALOGUE_H
#define LX06_BR_SCHEDULE_STATE_CATALOGUE_H
#include "schedule.h"
#include "inbox_store.h"
#define BR_SCHEDULE_CATALOGUE_API 1u
#define BR_SCHEDULE_CATALOGUE_ENTRIES 4096u
typedef struct {
    size_t directories,present,absent,tasks,history,payload_bytes,framed_bytes;
    size_t active,terminal;
} br_schedule_catalogue_totals;
/* Borrow child FD only during callback. 1 supplies its ORIGINAL held writer;
 * 0 says no writer is held and permits readonly observe_at; -1 refuses.
 * Never create/close/rebind a store here. A missed held writer safely refuses
 * at the existing lock; a second OFD must not reacquire one's own writer lock. */
typedef int (*br_schedule_catalogue_resolver)(void *,int,br_store **);
/* Provisional observation, valid only during this callback. No mutation,
 * IO/reentry, new registry, recovery or publication. Whole scan must return1
 * before a caller may use ANY observations; failure invalidates all visits.
 * Missing state has present0 and zero observations, never a guessed script. */
typedef int (*br_schedule_catalogue_visitor)(void *,int,int,
    const br_store_observation *,const br_schedule_observation *);
/* root/parent/entry are ORIGINAL authorized anchored FDs/name, not paths.
 * Names below root are opaque state-directory names, not script IDs. Every
 * root entry must be a private0700 directory, except the sole original
 * schedule-global.lock explicitly bound to held_global_lock_fd (private0600,
 * nlink1, exact named/FD identity before and after). An independent SH|NB
 * probe must observe an exclusive conflict; original owner proves same-OFD
 * provenance. This never obtains EX on the borrowed FD. Every child entry is
 * original private0600 schedule.v1 or schedule.lock. Temp/unknown/unsafe
 * entries refuse. No filesystem byte, lock, temp or directory is created,
 * deleted or repaired. Caller directory offsets stay unchanged.
 * max_entries1..4096 is an explicit work bound; overflow REFUSES, never
 * truncates to current32 sources. Empty/removed/>32 directories are scanned.
 * workspace is the original disjoint shared scratch, borrowed only while
 * idle, wiped on every acquired outcome. out publishes only on1; zero on
 * failure for valid disjoint storage. Rejected output/input aliases remain
 * untouched so validation cannot damage borrowed input. All arguments/callback state are disjoint and immutable except
 * workspace/out. A resolver is mandatory even for all-unopened catalogues.
 * This does not establish that the caller owns the required namespace lease
 * or that structurally observed calendars/UNKNOWN state may be recovered. */
int br_schedule_state_catalogue_observe(int root,int parent,const char *entry,int held_global_lock_fd,
    size_t max_entries,br_schedule_workspace *,
    br_schedule_catalogue_resolver,void *,br_schedule_catalogue_visitor,void *,
    br_schedule_catalogue_totals *);
#endif

/* Local API5, PROVISIONAL(P-20261112-0525-B), inherits P-0520/P-0450/P-0430/P-0405. Original owner's private
 * identity handoff codec only. Not a Ledger, persistence implementation,
 * authentication proof, resource receipt or permission to continue FLOW.
 * Serialized, non-realtime; all buffers/objects disjoint and immutable during
 * calls. No IO/heap. Actual durable owner and A/C production signoffs pending. */
#ifndef LX06_BR_FLOW_OWNER_HANDOFF_H
#define LX06_BR_FLOW_OWNER_HANDOFF_H
#include "flow_session_ledger.h"
#define BR_FLOW_OWNER_HANDOFF_API 6u
#define BR_FLOW_OWNER_HANDOFF_BYTES 7388u
/* Pure encoding. count = original prepared prefix, 0..binding.count (<=16).
 * steps and identities have count entries; NULL identity means association
 * not yet known, never permission to invent one. Present identities must be
 * origin0, empty code/text, exact epoch/UUID. No results are serialized.
 * Output capacity EXACTLY BYTES; length=0 on failure. A nonnull output with
 * exactly BYTES capacity is erased on failure (other capacities untouched).
 * 1 encoded, BR_INVALID refused. Success is not a durable/provenance proof. */
int br_flow_owner_handoff_encode(const br_flow_session_log_binding *,
    const br_flow_session_log_step *,const br_result *const *identities,unsigned count,
    void *output,size_t capacity,size_t *length);
/* Pure whole-image canonical validation (including empty/unknown prefixes).
 * 1 valid, BR_INVALID refused. No provenance, binding or durable authority. */
int br_flow_owner_handoff_validate(const void *,size_t);
/* Pure full-image/binding and common ordered PREPARE prefix comparison.
 * count=original log prefix0..binding.count, steps required unless count0.
 * Validates ALL rows on both sides, including non-common suffix; published
 * required, cleared on failure, image prefix length on success. Counts may
 * differ; 1 is identity comparison only, never authority to send or reconcile.
 * BR_INVALID malformed/conflicting. Disjoint inputs/output, no IO/heap. */
int br_flow_owner_handoff_prefix(const void *,size_t,
    const br_flow_session_log_binding *,const br_flow_session_log_step *,unsigned,unsigned *published);
/* API5 PROVISIONAL(P-20261112-0525-B). In-place one UNKNOWN PREPARE append.
 * EXACT BYTES capacity; length is old size in/new size out. Full old image,
 * binding and ALL steps validated before mutation. count=old count+1,1..16.
 * Preserves every old association byte. Failure leaves image/length unchanged.
 * Disjoint other arguments. Pure codec; no provenance or durable authority. */
int br_flow_owner_handoff_append_prepare(void *,size_t,size_t *,
    const br_flow_session_log_binding *,const br_flow_session_log_step *,unsigned);
/* One exact UNKNOWN row -> known identity. Validates full canonical old
 * image/binding/ordered prefix and typed original identity before mutation.
 * Existing associations and all other bytes stay unchanged. Same codec/schema,
 * no result text/hash or new store. Repeated/conflicting known rows refuse.
 * Exact BYTES capacity; failed calls leave length and bytes unchanged. */
int br_flow_owner_handoff_associate_step(void *,size_t,size_t *,
    const br_flow_session_log_binding *,const br_flow_session_log_step *,unsigned count,
    unsigned index,const br_result *identity);
enum br_flow_owner_event { BR_OWNER_INIT=1,BR_OWNER_PREPARE,BR_OWNER_ASSOCIATE };
/* Pure OLD-image -> proposed input transition check, no encoding/IO/proof.
 * INIT: NULL/0 old image, empty prefix. PREPARE: identical binding and old
 * prefix, exactly one new UNKNOWN step. ASSOCIATE: identical binding/steps,
 * exactly one UNKNOWN->known identity; all existing associations immutable.
 * Whole old image and all new typed arguments validated, no early trust of a
 * matching prefix. No-op/repeat/batch/shrink/rebind refused. 1/BR_INVALID.
 * All objects disjoint/immutable; no authority to send, authenticate or close. */
int br_flow_owner_handoff_transition(const void *,size_t,unsigned,
    const br_flow_session_log_binding *,const br_flow_session_log_step *,
    const br_result *const *,unsigned);
/* Strict1 proves THESE exact immutable bytes came from the ORIGINAL durable
 * owner registry under its current serialization/lock, not caller-encoded
 * guesses. No IO, mutation, release, receipt creation or API reentry. */
typedef int (*br_flow_owner_handoff_current)(void *,const void *,size_t);
typedef struct {
    const void *bytes;size_t length;
    br_flow_owner_handoff_current current;void *context;
    int busy;
} br_flow_owner_handoff;
/* Adapter for br_flow_session_ledger_result's mandatory associate callback.
 * Checks the ENTIRE canonical image on EVERY call, matches complete binding,
 * index/ticket/UUID/TASK/versions, then requires current provenance strictly1.
 * 1 original association, BR_INVALID for missing/malformed/unknown/refused.
 * No caching authority; missing task remains unresolved. K6 independently
 * checks its own CURRENT association. No IO, TTL, writes, playback or replay.
 * Owner must keep bytes locked/alive through original K6 query; not a borrowed
 * untrusted file mapping or a replacement for the production owner registry. */
int br_flow_owner_handoff_associate(void *,const br_flow_session_log_binding *,
    unsigned,const br_flow_session_log_step *,const br_result *);
#endif

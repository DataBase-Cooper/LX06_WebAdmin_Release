/* Internal A14 compact source API draft 1. Not production registration.
 * Original mode1 validation remains; retained source records the compact
 * script policy explicitly. Serial non-realtime
 * calls only; borrowed callbacks must not mutate, retain or reenter sources.
 * No clock, idle, caps, send, terminal, K6 or execution authority is issued. */
#ifndef LX06_APP_SCHEDULE_COMPACT_H
#define LX06_APP_SCHEDULE_COMPACT_H
#if !defined(LX06_EXTENSION_OBJECTS_FIXTURE) && !defined(LX06_PHONE_PROFILE_RUNTIME)
#error "Compact schedule source has no production registration"
#endif
#include "app-extension-objects.h"
#include <stddef.h>
#define EO_COMPACT_SCHEDULE_API 3u
#define EO_COMPACT_SCHEDULE_WORK_BYTES 8192u
enum eo_compact_schedule_kind { EO_SCHEDULE_ACTION=1,EO_SCHEDULE_FLOW=2 };
struct eo_compact_schedule_root;
struct eo_compact_schedule_source;
struct eo_saved_source;
/* Caller owns exactly one reusable off-thread workspace. Explicit busy guard
 * prevents sharing/reentry during any source operation or callback. It is
 * separate from B's original state workspace; no implicit alias/loan. */
union eo_compact_schedule_work {
    max_align_t alignment;
    unsigned char bytes[EO_COMPACT_SCHEDULE_WORK_BYTES];
};
struct eo_schedule_run_ref {
    uint32_t offset,length;
    uint16_t ordinal;
    uint8_t kind,reserved;
};
_Static_assert(sizeof(struct eo_schedule_run_ref)==12,"bounded run reference");
typedef int (*eo_compact_schedule_task_emit)(void *,unsigned,const struct eo_task *,
    const struct eo_schedule_run_ref *);
typedef int (*eo_compact_schedule_retire_proof)(void *,const struct eo_origin *);
void eo_compact_schedule_work_init(union eo_compact_schedule_work *);
/* Root retains duplicated original directories/parents and the EXISTING
 * settings .lock's exclusive open description. All anchors are revalidated.
 * entries are the three original directory entry names, not data filenames.
 * out must point to NULL; 1 complete/-1 refused. No extra store or writer. */
int eo_compact_schedule_root_open(const int directories[3],const int parents[3],
    const char *const entries[3],struct eo_compact_schedule_root **out);
/* Refuses while any source or operation still borrows the root. */
int eo_compact_schedule_root_close(struct eo_compact_schedule_root **);
/* Complete-directory scope may retain this exact root without fabricating
 * an empty source. Loans and source references are counted separately.
 * Directory FDs are borrowed read-only: never close/unlock/replace them. */
int eo_compact_schedule_root_retain(struct eo_compact_schedule_root *);
int eo_compact_schedule_root_unretain(struct eo_compact_schedule_root *);
int eo_compact_schedule_root_current(const struct eo_compact_schedule_root *);
int eo_compact_schedule_root_directory(const struct eo_compact_schedule_root *,unsigned index);
size_t eo_compact_schedule_root_bytes(const struct eo_compact_schedule_root *);
size_t eo_compact_schedule_source_bytes(const struct eo_compact_schedule_source *);
/* Successful-load metadata, borrowed immutable until close. NULL/0 for an
 * invalid source; an actual empty configuration also has count zero. */
const char *eo_compact_schedule_name(const struct eo_compact_schedule_source *);
size_t eo_compact_schedule_count(const struct eo_compact_schedule_source *);
/* Full production script name (<=64 bytes); settings/carrier use that exact
 * name + .json. The source holds original FDs/stat identities and computes
 * its own streaming SHA256; no supplied digest substitutes for source proof.
 * Mode2 script follows root/regular/nlink1/no0022; private payloads retain
 * their original policy. Whole carrier <=65536, original token/schema limits.
 * emit is provisional staging only: all task/refs become usable solely when
 * this call returns 1. Callbacks return strictly1; refusal erases/frees the
 * unpublished source and preserves all incumbents. task.run_node is -1.
 * out points to NULL; no published source is replaced by this entry. */
int eo_compact_schedule_load(union eo_compact_schedule_work *,
    struct eo_compact_schedule_root *,const char *script,uint64_t revision,
    eo_nfc,void *nfc_context,eo_compact_schedule_task_emit,void *emit_context,
    struct eo_compact_schedule_source **out);
/* Copies original full EO identity into caller's single bounded scratch.
 * No filesystem observation or execution permission. Returns strictly1. */
int eo_compact_schedule_origin(const struct eo_compact_schedule_source *,struct eo_origin *);
/* Rechecks all original file/anchor/lock identities and streamed bytes, before
 * and after reads. 1 current/0 refused; no cached-current authority. */
int eo_compact_schedule_current(union eo_compact_schedule_work *,
    struct eo_compact_schedule_source *,const struct eo_origin *expected);
/* Selected run is rederived by A's same full schema pass; caller offsets are
 * not authority. Every ref field must equal its actual original ordinal/run
 * in this retained source, and returned kind must match the load result.
 * Before/after current checks, staged canonical output; 1 complete/-1 refuse.
 * output is the complete canonical run OBJECT including closing }, with
 * length excluding NUL. B may replace that final } with its own identity
 * suffix without reparsing JSON or allocating another OPEN buffer. Output
 * and length are erased on refusal. FLOW is data only, never dispatch grant.
 * cap includes NUL, <=8193; the caller reserves space for its own suffix. */
int eo_compact_schedule_run_render(union eo_compact_schedule_work *,
    struct eo_compact_schedule_source *,const struct eo_schedule_run_ref *,
    const struct eo_origin *expected,char *output,size_t cap,size_t *length,unsigned *kind);
/* Typed FLOW projection from the same full original schema pass. Name is
 * decoded/UTF-8/NFC validated (caller storage exactly >=129 bytes); inputs
 * is a complete canonical object, {} when omitted. B need not parse run JSON.
 * Ref/identity/current checks are identical to run_render; both outputs and
 * length are erased on refusal. Disjoint name/inputs/work/source storage.
 * No independent parser, copied full view, dispatch or execution authority. */
int eo_compact_schedule_flow_select(union eo_compact_schedule_work *,
    struct eo_compact_schedule_source *,const struct eo_schedule_run_ref *,
    const struct eo_origin *expected,char *name,size_t name_capacity,
    char *inputs,size_t inputs_capacity,size_t *inputs_length);
/* Load the original independent FLOW source for this exact compact script.
 * Fresh empty owner/source only. Script/settings names, generation and original
 * exclusive lock description are derived internally from the verified source.
 * flow directory/parent/entry is an independently anchored private carrier
 * directory; filename is the same complete script name + .json. Original A
 * FLOW parser/NFC and eo_inputs_prepare remain the only definition/input path.
 * Source owns duplicated FDs/lock and can outlive the compact source/root.
 * Legacy eo_saved_load still requires private scripts; this explicit entry
 * admits only the compact-proven root-owned deployed script (no0022/nlink1).
 * 1 complete; -1 refuses without replacing an existing owner/source.
 * No hold, execution, second lock, script execution or resource-budget proof.
 * Original full FLOW view/transient saved-source reads remain budgeted costs. */
int eo_compact_schedule_flow_load(union eo_compact_schedule_work *,
    struct eo_compact_schedule_source *,const struct eo_origin *expected,
    struct eo_saved_source *,struct eo_owner *,int flow_directory,int flow_parent,const char *flow_entry);
/* Hold rechecks current source before taking ownership. Release intentionally
 * does NOT require disk bytes to remain current: original B terminal/K6/reap
 * proof must still retire the original identity after an on-disk edit. */
int eo_compact_schedule_hold(union eo_compact_schedule_work *,
    struct eo_compact_schedule_source *,const struct eo_origin *expected);
int eo_compact_schedule_release(struct eo_compact_schedule_source *,
    const struct eo_origin *expected,eo_compact_schedule_retire_proof,void *);
/* Refuses held/busy. Real last root close releases original flock; no fake
 * success/timeout proof. Successful source close frees it and NULLs pointer. */
int eo_compact_schedule_close(struct eo_compact_schedule_source **);
#endif

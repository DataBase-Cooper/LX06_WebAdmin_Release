/* Offline transaction model, NOT an installer or hardware authorization.
 * Trusted adapter boundary: observe must be fixed by firmware, never selected
 * by HTTP input. The binding, evidence and state must never be deserialized from
 * a client. Ordinary C memory cannot enforce privilege against its own caller.
 * All qualifications in tests are synthetic. No real adapter is provided.
 * No I/O, reboot, lock acquisition, persistence or recovery executor lives here.
 *
 * Integration contract (not implemented by this header):
 * - Firmware constructs the binding from an authenticated immutable package and
 *   qualified layout, and allocates unique nonzero transaction/lock/boot handles.
 *   Handles are NOT timestamps or truncated boot UUIDs. Never reuse them. Keep
 *   full identities in the future trusted adapter/journal. The caller must own
 *   non-NULL transaction storage and must not mutate/reinitialize a live object.
 * - observe is synchronous, read-only and non-reentrant. It must independently
 *   collect each event's evidence while holding the fenced writer lock, checking
 *   exact device identity, source/target kernel+root partition identities and the
 *   same NAND map. Do not echo the requested binding as proof. Lost/reacquired
 *   locks invalidate the epoch. No client boolean can be copied into evidence.
 * - mapping_digest identifies a reviewed write/readback/boot compatibility record
 *   bound to layout, bad-block map, tool/boot fingerprints and effective logical
 *   capacities of BOTH target images. Physical usable bytes alone do not qualify.
 *   policy.writer_verified means that complete record is qualified; neither a
 *   successful writer exit nor this model's test result sets a real qualification.
 * - package_authenticated is publisher authentication, not SHA equality;
 *   package_compatible includes release/model/kernel-rootfs compatibility.
 *   images_verified attests completed writes AND exact logical-length hashes of
 *   BOTH target images read through the SAME qualified mapping. On later events
 *   it also attests the receipt has remained valid under uninterrupted fencing.
 * - policy.boot_slot is the ACTUALLY booted kernel slot, not the next boot_part
 *   selection. policy.root_slot is the actually mounted root; both must be freshly
 *   observed. environment_read_slot is the separate next-boot selection. Success
 *   requires explicit complete write/readback receipts for the intended env plan,
 *   not merely a shell exit code. No vendor environment command is supplied here.
 * - services_verified bits 1/2/4 require actual webd/mico/mipns identity and health
 *   probes in the complete current-boot monitoring window, not mere PID existence.
 *   running_version must come from the mounted running image. Version buffers
 *   are canonical zero-padded 64-byte arrays, not client-provided version labels.
 * - preserve_data_verified includes BOTH target and fallback boot paths, including
 *   mount-failure behavior. All wipe requests are refused even if policy permits
 *   them: DATA reset, unmount, second confirmation and recovery are not implemented.
 *
 * Events and states (all transitions require fresh bound trusted observations):
 * NEW --PREFLIGHT--> LOCKED --IMAGES--> IMAGES_VERIFIED
 *     --ENV_BEGIN--> ENV_PENDING --ENV_RESULT--> REBOOT_PENDING
 *     --BOOT_HEALTH--> COMMITTED.
 * ENV_BEGIN models intent BEFORE any future environment operation. Once it has
 * been accepted, any error requires RECOVERY_REQUIRED and MUST NOT reboot. Before
 * it, errors latch REJECTED. Neither state permits automatic retry or fallback.
 * REBOOT_PENDING does not assert a reboot occurred or the target ever ran.
 * running_slot is only the LAST VERIFIED runtime snapshot (-1 if none), not live
 * status; only a new boot identity plus target slot/version/service evidence can
 * produce COMMITTED. Terminal states are sticky and have no recovery operation.
 *
 * This volatile model cannot survive process death/reboot: no serializer, journal,
 * fsync ordering, resume validation, boot acknowledgement or power-loss recovery
 * is implemented. ENV_PENDING must eventually be durably journaled before any
 * real environment write, with authenticated resume and fencing after reboot.
 * Until then no transition grants permission to deploy, write, switch or reboot.
 * Existing HTTP remains verify-only. Legacy policy's staging memory requirement
 * is intentionally retained; this is not a streaming-writer capacity redesign.
 */
#ifndef LX06_OTA_TRANSACTION_H
#define LX06_OTA_TRANSACTION_H
#include "ota-policy.h"
#define OTA_TX_SERVICES_REQUIRED 7u /* webd, mico, mipns: all required */
typedef enum {
    OTA_TX_NEW, OTA_TX_LOCKED, OTA_TX_IMAGES_VERIFIED, OTA_TX_ENV_PENDING,
    OTA_TX_REBOOT_PENDING, OTA_TX_COMMITTED, OTA_TX_REJECTED,
    OTA_TX_RECOVERY_REQUIRED
} ota_tx_state;
typedef enum {
    OTA_TX_PREFLIGHT, OTA_TX_IMAGES, OTA_TX_ENV_BEGIN, OTA_TX_ENV_RESULT,
    OTA_TX_BOOT_HEALTH
} ota_tx_event;
typedef struct {
    uint64_t transaction_id, lock_epoch, source_boot_id;
    int source_slot, target_slot;
    uint64_t kernel_bytes, rootfs_bytes;
    unsigned char package_digest[32], mapping_digest[32];
    char target_version[64];
} ota_tx_binding;
typedef struct {
    ota_tx_event event;
    ota_tx_binding binding;
    ota_policy policy;
    int lock_held, package_authenticated, package_compatible, images_verified;
    int environment_write_ok, environment_read_ok, environment_read_slot, running_slot;
    uint64_t observed_boot_id;
    char running_version[64];
    unsigned services_verified;
} ota_tx_evidence;
typedef int (*ota_tx_observe)(void *, ota_tx_event,
                              const ota_tx_binding *, ota_tx_evidence *);
typedef struct {
    ota_tx_state state;
    ota_tx_binding binding;
    ota_tx_observe observe;
    void *context;
    int running_slot;
    const char *reason;
} ota_transaction;
static inline void ota_tx_init(ota_transaction *t, const ota_tx_binding *b,
                               ota_tx_observe observe, void *context) {
    static const unsigned char zero_digest[32]={0};
    memset(t,0,sizeof(*t));
    t->running_slot=-1;
    if (!b || !observe || !b->transaction_id || !b->lock_epoch || !b->source_boot_id ||
        b->source_slot<0 || b->source_slot>1 || b->target_slot!=1-b->source_slot ||
        !b->target_version[0] || !memchr(b->target_version,0,sizeof(b->target_version)) ||
        !memcmp(b->package_digest,zero_digest,sizeof(zero_digest)) ||
        !memcmp(b->mapping_digest,zero_digest,sizeof(zero_digest))) {
        t->state=OTA_TX_REJECTED;
        t->reason="invalid_binding_or_adapter";
        return;
    }
    t->binding=*b;
    t->observe=observe;
    t->context=context;
}
static inline int ota_tx_binding_equal(const ota_tx_binding *a,
                                        const ota_tx_binding *b) {
    return a->transaction_id==b->transaction_id && a->lock_epoch==b->lock_epoch &&
        a->source_boot_id==b->source_boot_id && a->source_slot==b->source_slot &&
        a->target_slot==b->target_slot && a->kernel_bytes==b->kernel_bytes &&
        a->rootfs_bytes==b->rootfs_bytes &&
        !memcmp(a->package_digest,b->package_digest,sizeof(a->package_digest)) &&
        !memcmp(a->mapping_digest,b->mapping_digest,sizeof(a->mapping_digest)) &&
        !memcmp(a->target_version,b->target_version,sizeof(a->target_version));
}
static inline ota_tx_state ota_tx_fail(ota_transaction *t, const char *reason) {
    t->reason=reason;
    t->state=(t->state==OTA_TX_ENV_PENDING || t->state==OTA_TX_REBOOT_PENDING)?
        OTA_TX_RECOVERY_REQUIRED:OTA_TX_REJECTED;
    return t->state;
}
static inline ota_tx_state ota_tx_step(ota_transaction *t, ota_tx_event event) {
    ota_tx_evidence e;
    /* Terminal states are sticky; callbacks cannot restart a failed attempt. */
    if (t->state==OTA_TX_COMMITTED || t->state==OTA_TX_REJECTED ||
        t->state==OTA_TX_RECOVERY_REQUIRED) return t->state;
    if (!((t->state==OTA_TX_NEW && event==OTA_TX_PREFLIGHT) ||
          (t->state==OTA_TX_LOCKED && event==OTA_TX_IMAGES) ||
          (t->state==OTA_TX_IMAGES_VERIFIED && event==OTA_TX_ENV_BEGIN) ||
          (t->state==OTA_TX_ENV_PENDING && event==OTA_TX_ENV_RESULT) ||
          (t->state==OTA_TX_REBOOT_PENDING && event==OTA_TX_BOOT_HEALTH)))
        return ota_tx_fail(t,"invalid_transition");
    if (!t->observe) return ota_tx_fail(t,"missing_adapter");
    memset(&e,0,sizeof(e));
    e.event=(ota_tx_event)-1;
    if (t->observe(t->context,event,&t->binding,&e))
        return ota_tx_fail(t,"observation_failed");
    if (e.event!=event) return ota_tx_fail(t,"stale_event_evidence");
    t->reason=ota_policy_block(&e.policy,t->binding.kernel_bytes,t->binding.rootfs_bytes);
    if (t->reason) return ota_tx_fail(t,t->reason);
    if (!ota_tx_binding_equal(&t->binding,&e.binding))
        return ota_tx_fail(t,"binding_mismatch");
    if (e.lock_held!=1) t->reason="lock_not_held";
    else if (e.package_authenticated!=1) t->reason="package_not_authenticated";
    else if (e.package_compatible!=1) t->reason="package_not_compatible";
    else if (e.policy.wipe_data!=0) t->reason="data_reset_not_implemented";
    else if (event!=OTA_TX_BOOT_HEALTH &&
             (e.running_slot!=t->binding.source_slot ||
              e.policy.root_slot!=t->binding.source_slot ||
              e.observed_boot_id!=t->binding.source_boot_id))
        t->reason="source_changed";
    else if ((event==OTA_TX_PREFLIGHT || event==OTA_TX_IMAGES ||
              event==OTA_TX_ENV_BEGIN) &&
             (e.environment_read_ok!=1 || e.environment_read_slot!=t->binding.source_slot))
        t->reason="source_environment_unconfirmed";
    else if ((event==OTA_TX_IMAGES || event==OTA_TX_ENV_BEGIN ||
              event==OTA_TX_ENV_RESULT || event==OTA_TX_BOOT_HEALTH) && e.images_verified!=1)
        t->reason="images_not_verified";
    else if (event==OTA_TX_ENV_RESULT &&
             (e.environment_write_ok!=1 || e.environment_read_ok!=1 ||
              e.environment_read_slot!=t->binding.target_slot))
        t->reason="environment_unconfirmed";
    else if (event==OTA_TX_BOOT_HEALTH &&
             (e.environment_read_ok!=1 || e.environment_read_slot!=t->binding.target_slot ||
              e.running_slot!=t->binding.target_slot ||
              e.policy.root_slot!=t->binding.target_slot ||
              !e.observed_boot_id || e.observed_boot_id==t->binding.source_boot_id ||
              memcmp(e.running_version,t->binding.target_version,sizeof(e.running_version)) ||
              e.services_verified!=OTA_TX_SERVICES_REQUIRED))
        t->reason="boot_health_unconfirmed";
    if (t->reason) return ota_tx_fail(t,t->reason);
    switch (event) {
    case OTA_TX_PREFLIGHT: t->state=OTA_TX_LOCKED; t->running_slot=e.running_slot; break;
    case OTA_TX_IMAGES: t->state=OTA_TX_IMAGES_VERIFIED; break;
    case OTA_TX_ENV_BEGIN: t->state=OTA_TX_ENV_PENDING; break;
    case OTA_TX_ENV_RESULT: t->state=OTA_TX_REBOOT_PENDING; break;
    case OTA_TX_BOOT_HEALTH: t->state=OTA_TX_COMMITTED; t->running_slot=e.running_slot; break;
    }
    return t->state;
}
#endif

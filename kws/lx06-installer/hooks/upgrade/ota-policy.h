/* Fail-closed OTA policy. Qualification flags are trusted firmware constants,
 * never client request fields. Recheck metadata under the writer lock. */
#ifndef LX06_OTA_POLICY_H
#define LX06_OTA_POLICY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct {
    const char *model;
    int root_slot, boot_slot, shared_data;
    /* Trusted, fresh device observations under the writer lock, not HTTP flags.
     * Missing/unreadable monitoring evidence must fail closed. */
    int safety_known, safe_mode, health_window_complete, new_critical_errors;
    int geometry_known, bad_kernel, bad_rootfs, ecc_kernel, ecc_rootfs;
    /* Usable logical image bytes after the qualified bad-block mapping,
     * NEVER the raw /proc/mtd sizes on a NAND containing bad blocks. */
    uint64_t kernel_capacity, rootfs_capacity, package_bytes, tmp_free, mem_available;
    /* writer_verified covers write/readback/boot mapping compatibility and
     * effective capacity discovery, not merely a successful mtd exit status. */
    int writer_verified, rollback_verified, reset_verified;
    /* Both trial and fallback boot paths must preserve DATA on mount errors.
     * Stock automatic flash_erase-on-mount-failure is NOT qualified. */
    int preserve_data_verified;
    int wipe_data, wipe_confirmed;
} ota_policy;
static const char *ota_policy_block(const ota_policy *p, uint64_t kernel, uint64_t rootfs) {
    if (!p->model || strcmp(p->model, "LX06")) return "unsupported_model";
    if (p->root_slot < 0 || p->root_slot > 1 || p->boot_slot != p->root_slot) return "slot_mismatch";
    if (p->shared_data != 1) return "unknown_data_layout";
    if (p->safety_known != 1) return "unknown_safety_state";
    if (p->safe_mode != 0) return "device_in_safe_mode";
    if (p->health_window_complete != 1) return "health_window_incomplete";
    if (p->new_critical_errors != 0) return "recent_critical_errors";
    if (p->geometry_known != 1 || p->bad_kernel < 0 || p->bad_rootfs < 0 || p->ecc_kernel < 0 || p->ecc_rootfs < 0)
        return "unknown_mtd_health";
    /* A nonzero known bad-block count is not itself an OTA failure. */
    if (p->ecc_kernel || p->ecc_rootfs) return "inactive_slot_ecc_errors";
    if (kernel < 96 || rootfs < 96 || kernel > 6u*1024*1024 || rootfs > 40u*1024*1024 ||
        kernel > p->kernel_capacity || rootfs > p->rootfs_capacity) return "image_exceeds_partition";
    if (p->package_bytes != 4096 + kernel + rootfs) return "package_size_mismatch";
    if (p->tmp_free < p->package_bytes + 4u*1024*1024 ||
        p->mem_available < p->package_bytes + 16u*1024*1024) return "insufficient_staging_memory";
    if ((p->wipe_data != 0 && p->wipe_data != 1) ||
        (p->wipe_confirmed != 0 && p->wipe_confirmed != 1)) return "invalid_data_choice";
    if (p->wipe_data && !p->wipe_confirmed) return "shared_data_confirmation_required";
    if (p->wipe_data && p->reset_verified != 1) return "reset_not_verified";
    if (!p->wipe_data && p->preserve_data_verified != 1) return "data_preservation_not_verified";
    if (p->writer_verified != 1) return "writer_not_verified";
    if (p->rollback_verified != 1) return "rollback_not_verified";
    return NULL;
}
#endif

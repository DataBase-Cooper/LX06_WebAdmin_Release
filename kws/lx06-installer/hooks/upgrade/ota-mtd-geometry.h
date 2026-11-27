#ifndef LX06_OTA_MTD_GEOMETRY_H
#define LX06_OTA_MTD_GEOMETRY_H
#include <stddef.h>
#include <stdint.h>
/* Physical accounting only; not qualification of writer/readback/boot mapping. */
static int ota_mtd_usable(uint64_t bytes, uint64_t erase, uint64_t page,
                          int64_t bad, uint64_t *usable) {
    if (!usable) return -1;
    *usable=0;
    if (!bytes || !erase || !page || bytes%erase || erase%page || bad<0 ||
        (uint64_t)bad>bytes/erase) return -1;
    *usable=bytes-(uint64_t)bad*erase;
    return 0;
}
#endif

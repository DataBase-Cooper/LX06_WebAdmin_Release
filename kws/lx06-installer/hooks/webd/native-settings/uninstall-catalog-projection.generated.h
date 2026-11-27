/* Read-only host composition after generated-settings.inc. The same original
 * public projection is emitted twice, without reparsing the declaration or
 * serializing private records/defaults to disk. No production HTTP ABI. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "generated settings view is fixture-only"
#endif
/* Payload ceiling for this host slice, NOT a new approved product limit.
 * Terminator is additional. Whole settings records remain <=8192 bytes. */
#define GSV_BYTES (512u*1024u)
struct gsv_buf { char *data;size_t used,capacity;int error; };
static int gsv_bytes(struct gsv_buf *b,const char *s,size_t n) {
    if(b->error) return -1;
    if(b->used>b->capacity||n>b->capacity-b->used) {
        b->error=CFG_CAPACITY;return -1;
    }
    if(b->data) {
        memcpy(b->data+b->used,s,n);
        b->data[b->used+n]=0;
    }
    b->used+=n;return 0;
}
#include "generated-settings-view.generated.h"
static int udn_catalog_validate(const struct cfg *cfg,const struct ctl_map *map) {
    struct gsv_buf count={.capacity=GSV_BYTES};
    return gsv_public(cfg,map,&count);
}

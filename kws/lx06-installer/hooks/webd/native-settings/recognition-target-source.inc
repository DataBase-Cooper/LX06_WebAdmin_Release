/* Original retained editor snapshot -> SAME complete-script parser.
 * No read, capture, lock, parser grammar, persistence or runtime grant here. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) || !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "recognition target source is fixture-only"
#endif
static int rts_defaults(const struct ctl_revision *revision,struct cfg *out,size_t *extent) {
    if(!revision||!revision->bytes||!out||revision->size>CTL_SOURCE_SCRIPT_CAP||
       (revision->size>=4&&!memcmp(revision->bytes,"\177ELF",4))) return -1;
    size_t unused;
    /* cfg_parse_script owns header/body classification, UTF-8, declaration
     * capacity and atomic output. The caller owns source currency checks.
     * Executable bytes are opaque, never sourced or parsed as declarations. */
    return cfg_parse_script(revision->bytes,revision->size,out,extent?extent:&unused);
}
#include "target-source-extension.generated.h"
/* Run the ORIGINAL extension reader's exact leading-comment grammar over
 * the SAME retained header, including declarations beyond its old 16KiB read.
 * Schema JSON/field budgets and ext_parse semantics remain unchanged. */
static int rts_extensions(const char *raw,size_t extent,struct ext_schema *out) {
    if(!raw||!out||extent>CFG_DECL_BYTES) return -1;
    char *head=calloc(extent+1,1);
    if(!head) return -1;
    memcpy(head,raw,extent);
    int rc=rts_ext_header(head,out);
    volatile unsigned char *p=(void *)head;
    for(size_t i=0;i<extent+1;i++) p[i]=0;
    free(head);return rc;
}

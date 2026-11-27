#ifndef DB_OGG_AUDIO_H
#define DB_OGG_AUDIO_H
#include <stddef.h>
#include <stdint.h>
typedef struct OpusDecoder OpusDecoder;
#ifndef DB_OGG_CAP
#define DB_OGG_CAP (4u*1024u*1024u)
#endif
#define DB_OGG_PART (256u*1024u)
#define DB_OGG_FILES ((DB_OGG_CAP+DB_OGG_PART-1)/DB_OGG_PART)
typedef struct {
    /* fd+1 lets calloc/reset represent unopened files without a separate init. */
    int spool[DB_OGG_FILES];
    size_t head,used,peak;
    unsigned char page[65307],packet[65536];
    size_t page_size,body,packet_size;
    unsigned segment,segments,skip,preskip;
    uint64_t granule,produced;
    OpusDecoder *decoder;
    int eos;
} db_ogg;
int db_ogg_feed(db_ogg *,const unsigned char *,size_t);
int db_ogg_next(db_ogg *,int16_t *,size_t); /* PCM samples, 0 if incomplete, -1 error */
int db_ogg_empty(const db_ogg *);
size_t db_ogg_storage(const db_ogg *);
void db_ogg_reset(db_ogg *);
#endif

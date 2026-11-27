#define _POSIX_C_SOURCE 200809L
#define _LARGEFILE64_SOURCE
#include "ogg_audio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__arm__) && defined(__GLIBC__)
extern int __fxstat64(int,int,struct stat64 *);
#define DB_STAT struct stat64
#define DB_FSTAT(fd,st) __fxstat64(3,fd,st)
#else
#define DB_STAT struct stat
#define DB_FSTAT(fd,st) fstat(fd,st)
#endif
extern OpusDecoder *opus_decoder_create(int,int,int *);
extern void opus_decoder_destroy(OpusDecoder *);
extern int opus_decode(OpusDecoder *,const unsigned char *,int,int16_t *,int,int);

/* An unlinked tmpfs ring stores compressed audio with a fixed total ceiling.
 * Each lazy file respects keeper's 256 KiB FSIZE limit. Sixteen files also
 * leave room under its 32 descriptor limit. Consumed parts close immediately;
 * tmpfs still consumes system RAM, so check headroom before growing the ring.
 * It never writes DATA, and the kernel releases it even on SIGKILL.
 * Reading downlink keeps running while the player drains, so a pending long
 * response cannot hide cloud interrupt events behind TCP backpressure. */
static int headroom(void){
    FILE *f=fopen("/proc/meminfo","r");if(!f)return -1;
    char line[160];unsigned long kb=0;int found=0;
    while(fgets(line,sizeof(line),f))if(sscanf(line,"MemAvailable: %lu kB",&kb)==1){found=1;break;}
    fclose(f);return found && kb>=8192 ? 0:-1;
}
static int spool_io(db_ogg *o,size_t at,unsigned char *data,size_t n,int writing){
    while(n){
        size_t part=at/DB_OGG_PART,offset=at%DB_OGG_PART;
        size_t chunk=DB_OGG_PART-offset;
        if(chunk>DB_OGG_CAP-at)chunk=DB_OGG_CAP-at;
        if(chunk>n)chunk=n;
        if(!o->spool[part]){
            if(!writing || headroom())return -1;
            char path[]="/tmp/lx06-db-audio-XXXXXX";
            int fd=mkstemp(path);if(fd<0)return -1;
            if(unlink(path)||fcntl(fd,F_SETFD,FD_CLOEXEC)){close(fd);return -1;}
            o->spool[part]=fd+1;
        }
        int fd=o->spool[part]-1;
        ssize_t got=writing?pwrite(fd,data,chunk,(off_t)offset):pread(fd,data,chunk,(off_t)offset);
        if(got<0&&errno==EINTR)continue;
        if(got<=0)return -1;
        n-=(size_t)got;data+=got;at=(at+(size_t)got)%DB_OGG_CAP;
    }
    return 0;
}
int db_ogg_feed(db_ogg *o,const unsigned char *data,size_t n){
    if(n>DB_OGG_CAP-o->used)return -1;
    if(!n)return 0;
    if(spool_io(o,(o->head+o->used)%DB_OGG_CAP,(unsigned char *)data,n,1))return -1;
    o->used+=n;
    if(o->used>o->peak)o->peak=o->used;
    return 0;
}
static void consume(db_ogg *o,size_t size){
    o->head=(o->head+size)%DB_OGG_CAP;o->used-=size;
    /* Keep exactly the shards intersecting the remaining circular interval.
     * A shard can contain both the head and wrapped tail; do not close it
     * merely because the read cursor crossed its end. */
    for(size_t i=0;i<DB_OGG_FILES;i++)if(o->spool[i]){
        size_t start=i*DB_OGG_PART,end=start+DB_OGG_PART;
        if(end>DB_OGG_CAP)end=DB_OGG_CAP;
        size_t tail=o->head+o->used;
        int keep=o->used && ((start<tail && end>o->head) ||
                 (tail>DB_OGG_CAP && start<tail-DB_OGG_CAP));
        if(!keep){close(o->spool[i]-1);o->spool[i]=0;}
    }
}
static int page(db_ogg *o){
    if(o->used<27)return 0;
    if(spool_io(o,o->head,o->page,27,0))return -1;
    if(memcmp(o->page,"OggS",4)||o->page[4])return -1;
    unsigned segments=o->page[26];size_t size=27+segments;
    if(o->used<size)return 0;
    if(spool_io(o,(o->head+27)%DB_OGG_CAP,o->page+27,segments,0))return -1;
    for(unsigned i=0;i<segments;i++)size+=o->page[27+i];
    if(size>sizeof(o->page))return -1;
    if(o->used<size)return 0;
    if(spool_io(o,o->head,o->page,size,0))return -1;
    /* CRC-32, Ogg's non-reflected polynomial. */
    uint32_t expected=0,crc=0;
    for(int i=0;i<4;i++)expected|=(uint32_t)o->page[22+i]<<(i*8);
    for(size_t i=0;i<size;i++){
        crc^=(uint32_t)(i>=22&&i<26?0:o->page[i])<<24;
        for(int bit=0;bit<8;bit++)crc=crc&0x80000000u?(crc<<1)^0x04c11db7u:crc<<1;
    }
    if(crc!=expected)return -1;
    consume(o,size);
    if((o->packet_size!=0)!=((o->page[5]&1)!=0))return -1;
    o->granule=0;for(int i=0;i<8;i++)o->granule|=(uint64_t)o->page[6+i]<<(8*i);
    o->page_size=size;o->segment=0;o->segments=segments;o->body=27+segments;o->eos=(o->page[5]&4)!=0;
    return 1;
}
int db_ogg_next(db_ogg *o,int16_t *pcm,size_t cap){
    for(unsigned work=0;work<260;work++){
        if(o->segment==o->segments){
            o->page_size=0;
            int r=page(o);if(r<=0)return r;
        }
        if(!o->segments)continue;
        unsigned n=o->page[27+o->segment++];
        if(n>sizeof(o->packet)-o->packet_size || n>o->page_size-o->body)return -1;
        memcpy(o->packet+o->packet_size,o->page+o->body,n);o->packet_size+=n;o->body+=n;
        if(n==255)continue;
        size_t bytes=o->packet_size;o->packet_size=0;
        if(bytes>=19 && !memcmp(o->packet,"OpusHead",8)){
            if(o->packet[8]>15||o->packet[9]!=1||o->packet[18]!=0)return -1;
            if(o->decoder)opus_decoder_destroy(o->decoder);
            int error=0;o->decoder=opus_decoder_create(24000,1,&error);
            if(!o->decoder||error)return -1;
            o->preskip=o->skip=((unsigned)o->packet[10]|(unsigned)o->packet[11]<<8)/2;
            o->produced=0;continue;
        }
        if(bytes>=8 && !memcmp(o->packet,"OpusTags",8))continue;
        if(!o->decoder || cap<2880)return -1;
        int decoded=opus_decode(o->decoder,o->packet,(int)bytes,pcm,2880,0);
        if(decoded<0)return -1;
        unsigned skip=o->skip<(unsigned)decoded?o->skip:(unsigned)decoded;
        o->skip-=skip;decoded-=(int)skip;
        if(skip)memmove(pcm,pcm+skip,(size_t)decoded*sizeof(*pcm));
        if(o->eos && o->segment==o->segments && o->granule!=UINT64_MAX){
            uint64_t limit=o->granule/2>o->preskip?o->granule/2-o->preskip:0;
            if(o->produced+(unsigned)decoded>limit)decoded=limit>o->produced?(int)(limit-o->produced):0;
        }
        o->produced+=(unsigned)decoded;
        if(decoded)return decoded;
    }
    return 0;
}
int db_ogg_empty(const db_ogg *o){return !o->used && o->segment==o->segments && !o->packet_size;}
size_t db_ogg_storage(const db_ogg *o){
    size_t n=0;DB_STAT st;
    for(size_t i=0;i<DB_OGG_FILES;i++)if(o->spool[i] && !DB_FSTAT(o->spool[i]-1,&st))n+=(size_t)st.st_blocks*512;
    return n;
}
void db_ogg_reset(db_ogg *o){
    if(o->decoder)opus_decoder_destroy(o->decoder);
    for(size_t i=0;i<DB_OGG_FILES;i++)if(o->spool[i])close(o->spool[i]-1);
    memset(o,0,sizeof(*o));
}

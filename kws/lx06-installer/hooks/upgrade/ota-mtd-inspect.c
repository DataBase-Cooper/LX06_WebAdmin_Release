/* Read-only NAND accounting. Never writes, erases, switches slots or authorizes OTA.
 * Two matching MEMGETBADBLOCK samples supply bounded physical accounting only;
 * neither an atomic snapshot nor write/readback/boot mapping qualification.
 * There are no payload reads, so unchanged ECC is NOT proof of clean data or
 * absence of earlier corrections. Only fixed MTD nodes in CLI. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mtd/mtd-user.h>
#include "ota-mtd-geometry.h"
/* A fixed 8 KiB LSB-first bitmap bounds storage AND traversal cost. Bit i is
 * physical eraseblock i, at i*info.erasesize, in this exact info.size geometry.
 * This is sampled read-only evidence, never a qualified boot/write mapping. */
#define OTA_MTD_MAX_BLOCKS 65536U
#define OTA_MTD_BITMAP_BYTES (OTA_MTD_MAX_BLOCKS/8U)
struct ota_mtd_result {
    unsigned char bad_bitmap[OTA_MTD_BITMAP_BYTES];
    unsigned block_count, bitmap_bytes, scans_completed;
    int positions_stable;
    struct mtd_info_user info;
    struct mtd_ecc_stats ecc, ecc_before;
    int ecc_before_valid, ecc_after_valid;
    /* Signed observed counter difference, not a wrap-adjusted error total. */
    int64_t corrected_delta;
    uint64_t usable;
    unsigned bad;
    const char *error;
};
static int ota_mtd_inspect(const char *device,struct ota_mtd_result *r) {
    int fd,rc=-1,after_attempted=0;
    struct stat st;
    struct mtd_ecc_stats before;
    uint64_t offset;
    memset(r,0,sizeof(*r));
    fd=open(device,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){r->error="mtd_open_failed";return -1;}
    r->error="not_character_device";
    if(fstat(fd,&st) || !S_ISCHR(st.st_mode))goto done;
    r->error="mtd_geometry_unavailable";
    if(ioctl(fd,MEMGETINFO,&r->info))goto done;
    r->error="unsupported_mtd_geometry";
    if((r->info.type!=MTD_NANDFLASH && r->info.type!=MTD_MLCNANDFLASH) ||
       ota_mtd_usable(r->info.size,r->info.erasesize,r->info.writesize,0,&r->usable))goto done;
    r->error="bad_block_bitmap_limit";
    if(r->info.size/r->info.erasesize>OTA_MTD_MAX_BLOCKS)goto done;
    r->block_count=r->info.size/r->info.erasesize;
    r->bitmap_bytes=(r->block_count+7U)/8U;
    r->error="ecc_stats_unavailable";
    if(ioctl(fd,ECCGETSTATS,&before))goto done;
    r->ecc=before;r->ecc_before=before;r->ecc_before_valid=1;
    r->error="uncorrectable_ecc_recorded";
    if(before.failed)goto done;
    for(unsigned pass=0;pass<2;pass++) {
        int changed=0;
        for(offset=0;offset<r->info.size;offset+=r->info.erasesize) {
            unsigned block=(unsigned)(offset/r->info.erasesize);
            unsigned char mask=(unsigned char)(1U<<(block%8U));
            int64_t position=(int64_t)offset;
            int bad=ioctl(fd,MEMGETBADBLOCK,&position);
            r->error="bad_block_query_failed";
            if(bad<0)goto done;
            if(!pass && bad>0) {r->bad++;r->bad_bitmap[block/8U]|=mask;}
            if(pass && (bad>0)!=!!(r->bad_bitmap[block/8U]&mask))changed=1;
        }
        r->scans_completed++;
        r->error="bad_block_positions_changed";
        if(changed)goto done;
    }
    r->error="ecc_stats_unavailable";
    after_attempted=1;
    if(ioctl(fd,ECCGETSTATS,&r->ecc))goto done;
    r->ecc_after_valid=1;
    r->corrected_delta=(int64_t)r->ecc.corrected-(int64_t)before.corrected;
    r->error="health_changed_during_query";
    if(r->ecc.failed || r->corrected_delta || r->ecc.badblocks!=before.badblocks ||
       r->ecc.bbtblocks!=before.bbtblocks)goto done;
    r->error="bad_block_count_mismatch";
    if(r->bad!=r->ecc.badblocks)goto done;
    r->error="mtd_geometry_unavailable";
    {
        struct mtd_info_user after;
        if(ioctl(fd,MEMGETINFO,&after))goto done;
        r->error="mtd_geometry_changed";
        if(after.type!=r->info.type || after.flags!=r->info.flags ||
           after.size!=r->info.size || after.erasesize!=r->info.erasesize ||
           after.writesize!=r->info.writesize || after.oobsize!=r->info.oobsize)goto done;
    }
    r->error="invalid_usable_capacity";
    if(ota_mtd_usable(r->info.size,r->info.erasesize,r->info.writesize,r->bad,&r->usable))goto done;
    r->error=NULL;r->positions_stable=1;rc=0;
 done:
    /* Retain a final sample even on an interrupted traversal. It never turns
     * partial evidence into success; an unavailable sample stays explicitly so. */
    if(r->ecc_before_valid && !after_attempted) {
        struct mtd_ecc_stats after;
        if(!ioctl(fd,ECCGETSTATS,&after)) {
            r->ecc=after;r->ecc_after_valid=1;
            r->corrected_delta=(int64_t)after.corrected-(int64_t)before.corrected;
        }
    }
    close(fd);
    if(rc) {
        r->usable=0;r->positions_stable=0;r->bitmap_bytes=0;
        memset(r->bad_bitmap,0,sizeof(r->bad_bitmap));
    } /* Partial/failed evidence must never expose usable capacity or a map. */
    return rc;
}
#ifndef OTA_MTD_TEST
int main(int argc,char **argv) {
    static const char *names[]={"boot0","boot1","system0","system1"};
    static const char *devices[]={"/dev/mtd2","/dev/mtd3","/dev/mtd4","/dev/mtd5"};
    int failed=0;
    if(argc!=2 || strcmp(argv[1],"--read-only")) {
        fputs("Usage: ota-mtd-inspect --read-only\n",stderr);return 2;
    }
    puts("{\"read_only\":true,\"writer_verified\":false,\"partitions\":[");
    for(unsigned i=0;i<4;i++) {
        struct ota_mtd_result r;
        int rc=ota_mtd_inspect(devices[i],&r);failed|=rc!=0;
        printf("%s{\"name\":\"%s\",\"mtd\":%u,\"ok\":%s,\"raw_bytes\":%u,\"erase_bytes\":%u,\"page_bytes\":%u,\"bad_blocks\":%u,\"ecc_failed\":%u,\"bbt_blocks\":%u,\"physical_usable_bytes\":%llu,\"error\":\"%s\",",
            i?",":"",names[i],i+2,rc?"false":"true",r.info.size,r.info.erasesize,r.info.writesize,
            r.bad,r.ecc.failed,r.ecc.bbtblocks,(unsigned long long)r.usable,r.error?r.error:"");
        printf("\"read_only_evidence\":{\"mapping_qualified\":false,\"positions_stable\":%s,"
               "\"scans_completed\":%u,\"block_count\":%u,\"bitmap_bytes\":%u,"
               "\"bitmap_encoding\":\"hex-lsb0-physical-eraseblock\","
               "\"bitmap_geometry\":{\"raw_bytes\":%u,\"erase_bytes\":%u,\"page_bytes\":%u},"
               "\"bad_block_bitmap_hex\":\"",
               r.positions_stable?"true":"false",r.scans_completed,r.block_count,r.bitmap_bytes,
               r.info.size,r.info.erasesize,r.info.writesize);
        /* Stream the complete bounded bitmap; no fixed-size text buffer can
         * silently truncate its tail. Failed evidence emits an empty bitmap. */
        for(unsigned b=0;b<r.bitmap_bytes;b++)printf("%02x",(unsigned)r.bad_bitmap[b]);
        printf("\",\"ecc_before_valid\":%s,\"ecc_after_valid\":%s,\"ecc_corrected_before\":",
               r.ecc_before_valid?"true":"false",r.ecc_after_valid?"true":"false");
        if(r.ecc_before_valid)printf("%u",r.ecc_before.corrected);else fputs("null",stdout);
        fputs(",\"ecc_corrected_after\":",stdout);
        if(r.ecc_after_valid)printf("%u",r.ecc.corrected);else fputs("null",stdout);
        fputs(",\"ecc_corrected_delta\":",stdout);
        if(r.ecc_before_valid && r.ecc_after_valid)printf("%lld",(long long)r.corrected_delta);
        else fputs("null",stdout);
        puts(",\"ecc_delta_semantics\":\"signed-counter-difference-no-wrap-inference\"}}");
    }
    puts("]}");
    /* Output is evidence too: a full stream must reach its sink, not merely the
     * stdio buffer. Consumers must require both valid JSON and exit status 0. */
    if(fflush(stdout)!=0 || ferror(stdout))failed=1;
    return failed;
}
#endif

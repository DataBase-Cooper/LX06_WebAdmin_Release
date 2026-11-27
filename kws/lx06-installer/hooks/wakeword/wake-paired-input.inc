#include "wake-pair-match.h"
/* Some clients embed the detector after selecting POSIX feature visibility. */
#if defined(__GLIBC__) && !defined(__USE_GNU)
extern long syscall(long number,...);
extern int mkostemp(char *name,int flags);
#endif
#include "../xiaoai/session-epoch.h"
#include <time.h>
enum { PAIR_BLOCKS=250 };
/* Observed DNN completion bursts reached 294 ms. Bound recovery to 320 ms. */
enum { PAIR_STOCK_STAMPS=640, PAIR_CLOCK_TOLERANCE_MS=60, PAIR_DNN_BATCH_MAX_MS=320 };
static struct {
    unsigned char stock[16],physical[16],idle[16];
    uint64_t stock_base,stock_next,stock_floor,first,next,last_claim,stamp[PAIR_BLOCKS];
    uint64_t stock_stamp[PAIR_STOCK_STAMPS];
    unsigned count,start;
    int have_stock;
    int16_t pcm[PAIR_BLOCKS][BLOCK_SAMPLES];
} *paired;
struct pair_ticket {
    unsigned char stock[16], physical[16], idle[16];
    uint64_t stock_first,stock_end,first,end,stamp;
    int cloud_rescued, local_near, wait_for_tail, phrase_fast;
    float rival_score;
};
static struct {
    int samples,end_ms,local_near;
    uint64_t through,expires;
    float score,rival_score;
    char word[48];
} pair_retry;
static void pair_retry_finish(void);
static struct pair_ticket cloud_ticket, pending_ticket;
static int cloud_attempted;
static uint64_t pending_after;
static uint64_t queued_receipt_id, published_receipt_id;
static char pending_word[48];
static float pending_score;
static void pair_post_reply(void);
static uint64_t pair_le(const unsigned char *p,unsigned n) {
    uint64_t v=0;
    while(n) v=v*256+p[--n];
    return v;
}
static int pair_exact(void *dst,size_t bytes) {
    unsigned char *p=dst;
    size_t done=0;
    while(done<bytes) {
        ssize_t n=read(0,p+done,bytes-done);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return !done && !n ? 0 : -1;
        done+=(size_t)n;
    }
    return 1;
}
static uint64_t pair_now_ms(void) {
#ifdef LX06_PAIREDCLOCK_OFFLINE
    extern uint64_t lx06_offline_pair_ms;
    return lx06_offline_pair_ms;
#else
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
    return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
#endif
}
#ifdef LX06_PAIREDCLOCK_OFFLINE
uint64_t lx06_offline_pair_ms;
#endif
static void pair_observe(const unsigned char *p) {
    const unsigned char zero[16]={0};
    uint64_t first=pair_le(p+36,8),stamp=pair_le(p+44,8),now=pair_now_ms();
    if(!memcmp(p+4,zero,16) || !memcmp(p+20,zero,16) || !stamp || stamp>now ||
       now-stamp>=3000 || first>UINT64_MAX-BLOCK_SAMPLES) {
        if(pending_after) fputs("PAIR_POST invalid-frame\n",stderr);
        paired->count=0;pending_after=0;pair_retry.samples=0;return;
    }
    int changed=memcmp(paired->physical,p+4,16) || memcmp(paired->idle,p+20,16);
    if(!changed && first<paired->next) {
        if(pending_after) fputs("PAIR_POST backward-frame\n",stderr);
        paired->count=0;pending_after=0;pair_retry.samples=0;return;
    }
    if(!paired->count || changed ||
       first!=paired->next) {
        if(pending_after) fprintf(stderr,"PAIR_POST reset source=%d idle=%d gap=%lld\n",
            !!memcmp(paired->physical,p+4,16),!!memcmp(paired->idle,p+20,16),(long long)first-(long long)paired->next);
        pending_after=0;
        pair_retry.samples=0;
        paired->count=0;paired->start=0;paired->first=first;
        paired->stock_floor=paired->stock_next;
        if(changed) paired->last_claim=0;
        memcpy(paired->physical,p+4,16);memcpy(paired->idle,p+20,16);
    } else if(stamp<paired->stamp[(paired->start+paired->count-1)%PAIR_BLOCKS]) {
        if(pending_after) fputs("PAIR_POST backward-clock\n",stderr);
        paired->count=0;pending_after=0;pair_retry.samples=0;return;
    }
    if(paired->count==PAIR_BLOCKS) {
        paired->start=(paired->start+1)%PAIR_BLOCKS;
        paired->first+=BLOCK_SAMPLES;
        paired->count--;
    }
    unsigned slot=(paired->start+paired->count++)%PAIR_BLOCKS;
    memcpy(paired->pcm[slot],p+52,2*BLOCK_SAMPLES);
    paired->stamp[slot]=stamp;paired->next=first+BLOCK_SAMPLES;
    if(pending_after && stamp>=pending_after) pair_post_reply();
    if(pair_retry.samples && now>pair_retry.expires) pair_retry.samples=0;
    if(pair_retry.samples && paired->next>=pair_retry.through) pair_retry_finish();
}
static int read_paired_block(int16_t *block) {
    int half=0;
    if(pair_retry.samples && pair_now_ms()>pair_retry.expires) pair_retry.samples=0;
    while(half<2) {
        unsigned char p[LX06_WAKE_FRAME_BYTES],zero[16]={0};
        int ok=pair_exact(p,4);
        if(ok!=1) return half ? -1 : ok;
        if(!memcmp(p,"KWE2",4)) {
            if(pair_exact(p+4,sizeof(p)-4)!=1) return -1;
#ifdef LX06_PAIREDCLOCK_OFFLINE
            if(pair_le(p+44,8)>lx06_offline_pair_ms) lx06_offline_pair_ms=pair_le(p+44,8);
#endif
            pair_observe(p);continue;
        }
        if(memcmp(p,"KWT1",4) || pair_exact(p+4,360)!=1 ||
           !memcmp(p+4,zero,16) || pair_le(p+28,4)!=16000 || pair_le(p+32,4)!=320) return -1;
        uint64_t seq=pair_le(p+20,8);
        uint64_t stamp=pair_le(p+356,8);
#ifdef LX06_PAIREDCLOCK_OFFLINE
        if(stamp>lx06_offline_pair_ms) lx06_offline_pair_ms=stamp;
#endif
        if(!stamp || stamp>pair_now_ms() || seq>UINT64_MAX/160-1) return -1;
        if(!paired->have_stock) {
            if(seq%2) continue;
            memcpy(paired->stock,p+4,16);
            paired->stock_base=paired->stock_next=seq*160;
            paired->have_stock=1;
        }
        if(memcmp(paired->stock,p+4,16) || paired->stock_next!=seq*160) {
            fprintf(stderr,"PAIR_STREAM stock-discontinuity source=%d frames=%lld\n",
                    memcmp(paired->stock,p+4,16)!=0,(long long)seq-(long long)(paired->stock_next/160));
            return -1;
        }
        if(paired->stock_next>paired->stock_base && stamp<paired->stock_stamp[(seq-1)%PAIR_STOCK_STAMPS]) return -1;
        paired->stock_stamp[seq%PAIR_STOCK_STAMPS]=stamp;
        memcpy(block+half*160,p+36,320);
        paired->stock_next+=160;++half;
    }
    return 1;
}
static int pair_times_ok(uint64_t a,uint64_t b,unsigned tolerance) {
    return (a>b ? a-b : b-a)<=tolerance;
}
static int pair_reject(const char *reason,long long a,long long b) {
    fprintf(stderr,"PAIR_REJECT %s %lld %lld\n",reason,a,b);
    return 0;
}
static int pair_clock_span(uint64_t stock_first,int physical_offset,int samples) {
    if(paired->stock_next-stock_first>PAIR_STOCK_STAMPS*160)
        return pair_reject("clock-history",paired->stock_next-stock_first,samples);
    uint64_t s0=0,p0=0,sprev=0,pprev=0;
    uint64_t batch_stamp=0;
    int batch_frames=0;
    for(int j=0;j<samples;j+=160) {
        uint64_t s=paired->stock_stamp[(stock_first/160+(unsigned)j/160)%PAIR_STOCK_STAMPS];
        uint64_t p=paired->stamp[(paired->start+((unsigned)physical_offset+j)/BLOCK_SAMPLES)%PAIR_BLOCKS];
        if(!s || !p) return pair_reject("clock-delta",j,0);
        if(j && (s<sprev || p<pprev || p-pprev>PAIR_CLOCK_TOLERANCE_MS))
            return pair_reject("clock-step",(long long)s-(long long)sprev,(long long)p-(long long)pprev);
        if(!pair_times_ok(s,p,PAIR_CLOCK_TOLERANCE_MS)) {
            /* KWT1 timestamps mark DNN callback completion. A paused callback
             * can deliver a run of already captured frames together. Accept
             * one such run only between aligned anchors, with a continuous
             * physical clock and contiguous source sample numbers. */
            if(!j || s<=p || s-p>PAIR_DNN_BATCH_MAX_MS ||
               (!batch_stamp && (batch_frames || s-sprev<=PAIR_CLOCK_TOLERANCE_MS)))
                return pair_reject("clock-delta",j,(long long)s-(long long)p);
            if(!batch_stamp) batch_stamp=s;
            ++batch_frames;
            if(s-batch_stamp>PAIR_CLOCK_TOLERANCE_MS || batch_frames*3*160>samples)
                return pair_reject("clock-batch",batch_frames,(long long)(s-batch_stamp));
        } else {
            if(batch_stamp && s-batch_stamp>PAIR_CLOCK_TOLERANCE_MS)
                return pair_reject("clock-batch",batch_frames,(long long)(s-batch_stamp));
            if(j && !batch_stamp && s-sprev>PAIR_CLOCK_TOLERANCE_MS)
                return pair_reject("clock-step",(long long)(s-sprev),(long long)(p-pprev));
            batch_stamp=0;
        }
        if(!j) {s0=s;p0=p;}
        sprev=s;pprev=p;
    }
    if(batch_stamp) return pair_reject("clock-batch-open",batch_frames,0);
    unsigned duration=(unsigned)(samples-160)/16;
    if(!pair_times_ok(sprev-s0,duration,PAIR_CLOCK_TOLERANCE_MS) ||
       !pair_times_ok(pprev-p0,duration,PAIR_CLOCK_TOLERANCE_MS))
        return pair_reject("clock-duration",(long long)(sprev-s0)-duration,(long long)(pprev-p0)-duration);
    return 1;
}
static int pair_prepare(struct pair_ticket *ticket,const int16_t *clip,int samples,int end_ms) {
    memset(ticket,0,sizeof(*ticket));
    if(!paired || !paired->have_stock || !paired->count || end_ms<=0 || end_ms%20)
        return pair_reject("input",paired ? paired->count : 0,end_ms);
    uint64_t offset=(uint64_t)(end_ms/20)*BLOCK_SAMPLES;
    if(offset<(unsigned)samples || offset>paired->stock_next-paired->stock_base)
        return pair_reject("stock-window",offset,paired->stock_next-paired->stock_base);
    uint64_t stock_end=paired->stock_base+offset,stock_first=stock_end-samples;
    if(stock_first<paired->stock_floor)
        return pair_reject("discontinuity",paired->stock_floor-stock_first,paired->count);
    unsigned total=paired->count*BLOCK_SAMPLES;
    if(total<(unsigned)samples) return pair_reject("physical-short",total,samples);
    int16_t *pcm=malloc(total*sizeof(*pcm));
    if(!pcm) return pair_reject("memory",total,samples);
    for(unsigned i=0;i<paired->count;i++)
        memcpy(pcm+i*BLOCK_SAMPLES,paired->pcm[(paired->start+i)%PAIR_BLOCKS],2*BLOCK_SAMPLES);
    struct wake_pair_result match;
    int ok=wake_pair_match(clip,samples,pcm,(int)total,&match);
    free(pcm);
    if(!ok) {
        int last=(int)total-samples;
        int outside=match.next_offset<0 ? match.next_offset :
                    match.next_offset>last ? match.next_offset-last : 0;
        ticket->wait_for_tail=outside>0 && outside<=2*BLOCK_SAMPLES &&
                              (match.offset>=last-BLOCK_SAMPLES || match.next>=.10f);
        fprintf(stderr,"PAIR_REJECT wave samples=%d physical=%u offset=%d peak=%.4f second=%.4f support=%d/%d second_offset=%d outside=%d\n",
                samples,total,match.offset,match.peak,match.next,match.supporting,match.active,match.next_offset,outside);
        return 0;
    }
    if(!pair_clock_span(stock_first,match.offset,samples)) return 0;
    uint64_t first=paired->first+(unsigned)match.offset,end=first+samples;
    if(first<paired->last_claim) return pair_reject("overlap",paired->last_claim-first,samples);
    unsigned last=((unsigned)match.offset+samples-1)/BLOCK_SAMPLES;
    uint64_t stamp=paired->stamp[(paired->start+last)%PAIR_BLOCKS],now=pair_now_ms();
    if(stamp>now || now-stamp>=3000) return pair_reject("age",(long long)now-(long long)stamp,samples);
    memcpy(ticket->stock,paired->stock,16);memcpy(ticket->physical,paired->physical,16);
    memcpy(ticket->idle,paired->idle,16);
    ticket->stock_first=stock_first;ticket->stock_end=stock_end;
    ticket->first=first;ticket->end=end;ticket->stamp=stamp;
    return 1;
}
static int pair_ticket_valid(const struct pair_ticket *t) {
    if(!paired || !t->stamp || !paired->count || memcmp(t->stock,paired->stock,16) ||
       memcmp(t->physical,paired->physical,16) || memcmp(t->idle,paired->idle,16) ||
       t->first<paired->last_claim || t->first<paired->first || t->end>paired->next ||
       t->end<=t->first) return 0;
    uint64_t now=pair_now_ms();
    unsigned last=(unsigned)((t->end-1-paired->first)/BLOCK_SAMPLES);
    return last<paired->count && t->stamp==paired->stamp[(paired->start+last)%PAIR_BLOCKS] &&
           now>=t->stamp && now-t->stamp<3000;
}
static int pair_ticket_lease(const struct pair_ticket *t) {
    return pair_ticket_valid(t) ? lx06_idle_epoch_observe(t->idle) : -1;
}
static void pair_publish(const struct pair_ticket *t,const char *label,float score) {
    char stock[33],physical[33],idle[33];
    epoch_hex(t->stock,stock);epoch_hex(t->physical,physical);epoch_hex(t->idle,idle);
    const char *kind=t->rival_score>0 ? "WAKE_PAIRED_LOCAL_NEAR_SEPARATED" :
                     t->cloud_rescued ? "WAKE_PAIRED_CLOUD" : t->local_near ? "WAKE_PAIRED_LOCAL_NEAR" : "WAKE_PAIRED";
    printf("%s %s %.6f %s %llu %llu %s %s %llu %llu %llu",
           kind,label,score,stock,
           (unsigned long long)t->stock_first,(unsigned long long)t->stock_end,physical,idle,
           (unsigned long long)t->first,(unsigned long long)t->end,(unsigned long long)t->stamp);
    if(t->rival_score>0) printf(" %.6f",t->rival_score);
    putchar('\n');
    fflush(stdout);
    paired->last_claim=t->end;
    fprintf(stderr,"PAIRED label=%s score=%.6f\n",label,score);
}
static void pair_post_reply(void) {
    pending_after=0;
    int lease=pair_ticket_lease(&pending_ticket);
    if(lease<0) {
        fprintf(stderr,"PAIR_POST refused valid=%d age_ms=%lld\n",pair_ticket_valid(&pending_ticket),
                (long long)pair_now_ms()-(long long)pending_ticket.stamp);
        return;
    }
    pair_publish(&pending_ticket,pending_word,pending_score);
    published_receipt_id=queued_receipt_id;
    close(lease);
}
static int pair_cloud_finish(const char *word,float score,int samples,int end_ms) {
    if(!cloud_ticket.stamp || end_ms<=0 || (uint64_t)samples!=cloud_ticket.end-cloud_ticket.first ||
       paired->stock_base+(uint64_t)end_ms*16!=cloud_ticket.stock_end) return 0;
    int lease=pair_ticket_lease(&cloud_ticket);
    if(lease<0) return 0;
    pending_ticket=cloud_ticket;
    ++queued_receipt_id;
    memset(&cloud_ticket,0,sizeof(cloud_ticket));
    snprintf(pending_word,sizeof(pending_word),"%s",word);
    pending_score=score;pending_after=pair_now_ms();
    close(lease);
    return 1;
}

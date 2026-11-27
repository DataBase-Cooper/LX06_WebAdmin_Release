/* Input must come from the credential-checked physical-source reader.
 * A freshly assigned output identity prevents raw and filtered history
 * from satisfying each other's native WUW receipts. */
#ifndef LX06_WAKE_OEM_NS_STREAM_H
#define LX06_WAKE_OEM_NS_STREAM_H
#include "wake-capture-frame.h"
#include "wake-oem-ns.h"
#include <unistd.h>
#include <sys/syscall.h>
struct wake_ns_stream {
    struct wake_oem_ns ns;
    uint8_t input_id[32], output_id[16], packet[LX06_WAKE_FRAME_BYTES];
    uint64_t next_input, next_output, available;
    unsigned used, pending;
    int16_t hop[NS_HOP];
};
typedef int (*wake_ns_write)(const uint8_t *,size_t,void *);
static uint64_t wake_ns_le64(const uint8_t *p) {
    uint64_t n=0;
    for(unsigned i=0;i<8;i++) n|=(uint64_t)p[i]<<(8*i);
    return n;
}
static void wake_ns_put64(uint8_t *p,uint64_t n) {
    for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(n>>(8*i));
}
static void wake_ns_end(struct wake_ns_stream *s) {
    wake_oem_ns_close(&s->ns);
    memset(s,0,sizeof(*s));
}
static int wake_ns_frame(struct wake_ns_stream *s,const uint8_t *in,wake_ns_write send,void *ctx) {
    static const uint8_t zero[16]={0};
    uint64_t first=wake_ns_le64(in+LX06_WAKE_FRAME_FIRST);
    uint64_t available=wake_ns_le64(in+LX06_WAKE_FRAME_AVAILABLE);
    if(memcmp(in,LX06_WAKE_FRAME_MAGIC,4) || !available ||
       first>UINT64_MAX-NS_WINDOW-LX06_WAKE_FRAME_SAMPLES ||
       !memcmp(in+LX06_WAKE_FRAME_SOURCE,zero,16) || !memcmp(in+LX06_WAKE_FRAME_IDLE,zero,16)) {
        wake_ns_end(s);
        return -1;
    }
    if(!s->ns.handle || memcmp(s->input_id,in+LX06_WAKE_FRAME_SOURCE,32) ||
       s->next_input!=first || available<s->available) {
        wake_ns_end(s);
        if(wake_oem_ns_open(&s->ns) ||
           syscall(SYS_getrandom,s->output_id,sizeof(s->output_id),1)!=sizeof(s->output_id) ||
           !memcmp(s->output_id,zero,16) || !memcmp(s->output_id,in+LX06_WAKE_FRAME_SOURCE,16)) {
            wake_ns_end(s);
            return -1;
        }
        memcpy(s->input_id,in+LX06_WAKE_FRAME_SOURCE,32);
        memcpy(s->packet,LX06_WAKE_FRAME_MAGIC,4);
        memcpy(s->packet+LX06_WAKE_FRAME_SOURCE,s->output_id,16);
        memcpy(s->packet+LX06_WAKE_FRAME_IDLE,in+LX06_WAKE_FRAME_IDLE,16);
        s->next_output=first+3*NS_HOP;
    }
    s->next_input=first+LX06_WAKE_FRAME_SAMPLES;
    s->available=available;
    for(unsigned i=0;i<LX06_WAKE_FRAME_SAMPLES;i++) {
        const uint8_t *pcm=in+LX06_WAKE_FRAME_HEADER+i*2;
        s->hop[s->used++]=(int16_t)(pcm[0]|(uint16_t)pcm[1]<<8);
        if(s->used<NS_HOP) continue;
        s->used=0;
        int16_t out[NS_HOP];
        int status=wake_oem_ns_hop(&s->ns,s->hop,out);
        if(status<0) {wake_ns_end(s);return -1;}
        if(!status) continue;
        for(unsigned j=0;j<NS_HOP;j++) {
            uint16_t sample=(uint16_t)out[j];
            uint8_t *dst=s->packet+LX06_WAKE_FRAME_HEADER+s->pending++*2;
            dst[0]=(uint8_t)sample;dst[1]=(uint8_t)(sample>>8);
            if(s->pending!=LX06_WAKE_FRAME_SAMPLES) continue;
            wake_ns_put64(s->packet+LX06_WAKE_FRAME_FIRST,s->next_output);
            wake_ns_put64(s->packet+LX06_WAKE_FRAME_AVAILABLE,available);
            if(send(s->packet,sizeof(s->packet),ctx)) {wake_ns_end(s);return -1;}
            s->next_output+=s->pending;
            s->pending=0;
        }
    }
    return 0;
}
#endif

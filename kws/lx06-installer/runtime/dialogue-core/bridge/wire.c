#include "wire.h"
#include <string.h>
_Static_assert(sizeof(dc_wire_header)==LX06_BRIDGE_HEADER_BYTES,"frozen Wire ABI");
_Static_assert(DC_WIRE_CAP==LX06_BRIDGE_PAYLOAD_MAX,"frozen Wire payload");
int br_wire_heartbeat_type(unsigned type){return type==BR_WIRE_PING||type==BR_WIRE_PONG;}
int br_wire_heartbeat_check(void *context,const dc_wire_header *h,const unsigned char *data,size_t n){
    (void)context;(void)data;(void)n;
    return br_wire_heartbeat_type(h->type)&&!h->flags&&!h->sequence;
}
int br_wire_type_allowed(unsigned type,enum br_direction direction){
    if(direction==BR_FROM_SPEAKER){
        switch(type){case BR_WIRE_OPEN:case BR_WIRE_MIC:case BR_WIRE_CANCEL:case BR_WIRE_RESUME:
            case BR_WIRE_CLOSE:case BR_WIRE_PING:case BR_WIRE_PONG:case BR_WIRE_WAKE:return 1;default:return 0;}
    }
    if(direction==BR_FROM_PHONE){
        switch(type){case BR_WIRE_GRANT:case BR_WIRE_READY:case BR_WIRE_TEXT:case BR_WIRE_AUDIO:
            case BR_WIRE_REPLY_END:case BR_WIRE_CANCEL_ACK:case BR_WIRE_ERROR:case BR_WIRE_RESUMED:
            case BR_WIRE_PING:case BR_WIRE_PONG:case BR_WIRE_STATE:case BR_WIRE_TASK:return 1;default:return 0;}
    }
    return 0;
}
static uint64_t get(const unsigned char *p,unsigned bytes){
    uint64_t n=0;for(unsigned i=0;i<bytes;i++)n|=(uint64_t)p[i]<<(8*i);return n;
}
static void put(unsigned char *p,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;i++)p[i]=(unsigned char)(n>>(8*i));}
static int check(const dc_wire_header *h,enum br_direction d,uint64_t epoch,
                 br_payload_check validate,void *context,const unsigned char *payload){
    if(h->magic!=DC_WIRE_MAGIC||h->version!=LX06_BRIDGE_WIRE_VERSION)return BR_WIRE_VERSION;
    if(!epoch||h->epoch!=epoch||!br_wire_type_allowed(h->type,d)||h->reserved||
       h->length>DC_WIRE_CAP||(h->flags&~7u))return BR_WIRE_INVALID;
    if(h->type==BR_WIRE_MIC||h->type==BR_WIRE_AUDIO){
        if(!(h->flags&DC_WIRE_PCM_S16LE)||h->rate<8000||h->rate>48000||
           (h->channels!=1&&h->channels!=2)||!h->length||h->length%(2u*h->channels)||!h->sequence)return BR_WIRE_INVALID;
    }else{
        if(h->rate||h->channels||(h->type==BR_WIRE_GRANT&&!h->length)||!validate||
           validate(context,h,payload,h->length)!=1)return BR_WIRE_INVALID;
    }
    return BR_WIRE_OK;
}
int br_wire_decode(const void *data,size_t length,enum br_direction d,uint64_t epoch,
                   br_payload_check validate,void *context,dc_wire_packet *out){
    if(!data||!out||length<48||length>48+DC_WIRE_CAP)return BR_WIRE_INVALID;
    const unsigned char *p=data;
    dc_wire_header h={.magic=(uint32_t)get(p,4),.version=(uint16_t)get(p+4,2),.type=(uint16_t)get(p+6,2),
        .length=(uint32_t)get(p+8,4),.flags=(uint32_t)get(p+12,4),.epoch=get(p+16,8),
        .generation=get(p+24,8),.sequence=get(p+32,8),.rate=(uint32_t)get(p+40,4),
        .channels=(uint16_t)get(p+44,2),.reserved=(uint16_t)get(p+46,2)};
    if(h.length>DC_WIRE_CAP||length!=48+h.length)return BR_WIRE_INVALID;
    int rc=check(&h,d,epoch,validate,context,p+48);
    if(rc!=BR_WIRE_OK)return rc;
    memmove(out->payload,p+48,h.length);out->header=h;return BR_WIRE_OK;
}
int br_wire_encode(const dc_wire_packet *packet,enum br_direction d,uint64_t epoch,
                   br_payload_check validate,void *context,void *data,size_t capacity,size_t *length){
    if(!packet||!data||!length)return BR_WIRE_INVALID;
    dc_wire_header h=packet->header;
    if(h.length>DC_WIRE_CAP||capacity<48+h.length)return BR_WIRE_INVALID;
    int rc=check(&h,d,epoch,validate,context,packet->payload);
    if(rc!=BR_WIRE_OK)return rc;
    unsigned char *p=data;memmove(p+48,packet->payload,h.length);
    put(p,h.magic,4);put(p+4,h.version,2);put(p+6,h.type,2);put(p+8,h.length,4);put(p+12,h.flags,4);
    put(p+16,h.epoch,8);put(p+24,h.generation,8);put(p+32,h.sequence,8);put(p+40,h.rate,4);
    put(p+44,h.channels,2);put(p+46,h.reserved,2);*length=48+h.length;return BR_WIRE_OK;
}
int br_wire_pong(const void *data,size_t n,uint64_t epoch,dc_wire_packet *p,
                  void *out,size_t capacity,size_t *length){
    if(!p)return BR_WIRE_INVALID;
    memset(p,0,sizeof(*p));
    int rc=br_wire_decode(data,n,BR_FROM_PHONE,epoch,br_wire_heartbeat_check,NULL,p);
    if(rc==BR_WIRE_OK){
        if(p->header.type!=BR_WIRE_PING)rc=BR_WIRE_INVALID;
        else{p->header.type=BR_WIRE_PONG;
            rc=br_wire_encode(p,BR_FROM_SPEAKER,epoch,br_wire_heartbeat_check,NULL,out,capacity,length);}
    }
    memset(p,0,sizeof(*p));return rc;
}

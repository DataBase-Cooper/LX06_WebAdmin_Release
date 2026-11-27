/* Saved native settings continuation through the original B16 profile.
 * No transport/parser/settings implementation, session attachment, business
 * submission or retry. The native Web entry owns admission and one-shot save.
 * The historical host adapter ABI remains for its original consumers. */
#if !defined(LX06_TOKEN_SYNC_FIXTURE) && !defined(LX06_NATIVE_TOKEN_SETTINGS)
#error "token sync requires its normal native settings entry or isolated consumer"
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "profile.h"
#include "token-sync-profile.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct ts_b16 ts_b16;
struct ts_b16 { br_profile *profile;br_discovery discovery;int sync_busy; };
ts_b16 *ts_b16_new(void) {
    ts_b16 *s=calloc(1,sizeof(*s));if(!s)return NULL;
    s->profile=br_profile_new();if(!s->profile){free(s);return NULL;}return s;
}
void ts_b16_free(ts_b16 *s) {
    if(s){br_profile_free(s->profile);memset(s,0,sizeof(*s));free(s);}
}
static int ts_hex(unsigned char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static int ts_config(const struct ts_profile *saved,const char *stable_ca,br_profile_config *c) {
    if(!saved||!stable_ca||!c||strnlen(saved->spki,sizeof(saved->spki))!=64||
       !memchr(saved->host,0,sizeof(saved->host))||!memchr(saved->token,0,sizeof(saved->token)))
        return BR_PROFILE_INVALID;
    *c=(br_profile_config){.host=saved->host,.port=saved->port,
        .ca_file=stable_ca,.token=saved->token};
    for(unsigned i=0;i<32;i++) {
        int a=ts_hex((unsigned char)saved->spki[2*i]);
        int b=ts_hex((unsigned char)saved->spki[2*i+1]);
        if(a<0||b<0)return BR_PROFILE_INVALID;
        c->spki_sha256[i]=(unsigned char)(16*a+b);
    }
    return 0;
}
int ts_b16_dial(ts_b16 *s,const struct ts_profile *saved,const char *stable_ca,
                uint64_t run,uint64_t epoch,const void *open,size_t length) {
    br_profile_config c;
    if(!s||ts_config(saved,stable_ca,&c))return BR_PROFILE_INVALID;
    return br_profile_dial(s->profile,&c,&s->discovery,run,epoch,open,length);
}

int ts_b16_pump(ts_b16 *s,uint64_t run,uint64_t epoch) {
    if(!s)return BR_PROFILE_INVALID;
    int rc=br_profile_pump(s->profile,run,epoch,NULL,NULL);
    if(rc==BR_DISC_CAPS&&(!s->discovery.caps.ready||
        s->discovery.phase!=BR_DISC_READY||s->discovery.epoch!=epoch))return BR_DISC_INVALID;
    return rc;
}
const char *ts_b16_error(ts_b16 *s){return s?br_profile_error(s->profile):"unsupported";}
void ts_b16_close(ts_b16 *s,uint64_t run,uint64_t epoch) {
    if(s)br_profile_close(s->profile,run,epoch);
}

struct ts_live {ts_saved_current gate;void *context;};
static int ts_profile_current(void *context) {
    struct ts_live *live=context;
    return live&&live->gate&&live->gate(live->context)==0;
}
static uint64_t ts_now_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0)return 0;
    return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;
}
int ts_b16_caps_saved(ts_b16 *s,const struct ts_profile *saved,const char *stable_ca,
    uint64_t run,uint64_t epoch,const void *open,size_t length,
    ts_saved_current gate,void *context) {
    br_profile_config c;
    if(!s||!s->profile||s->sync_busy||!gate||!run||!epoch||!open||!length||
       ts_config(saved,stable_ca,&c))return TS_SYNC_UNCONFIRMED;
    struct ts_live live={gate,context};
    if(!ts_profile_current(&live))return TS_SYNC_UNCONFIRMED;
    s->sync_busy=1;
    int result=TS_SYNC_FAILED;
    int rc=br_profile_dial_source(s->profile,&c,&s->discovery,run,epoch,open,length,
        ts_profile_current,&live);
    uint64_t start=ts_now_ms();
    if(!start){result=TS_SYNC_UNCONFIRMED;goto done;}
    while(rc>=0) {
        if(!ts_profile_current(&live)){result=TS_SYNC_UNCONFIRMED;goto done;}
        rc=br_profile_pump(s->profile,run,epoch,NULL,NULL);
        if(rc==BR_DISC_CAPS) {
            if(s->discovery.caps.ready&&s->discovery.phase==BR_DISC_READY&&
               s->discovery.epoch==epoch&&ts_profile_current(&live))result=TS_SYNC_READY;
            else result=TS_SYNC_UNCONFIRMED;
            goto done;
        }
        if(rc<0)break;
        uint64_t now=ts_now_ms();
        if(!now||now<start||now-start>=6000)goto done;
        struct timespec pause={0,2000000};
        (void)nanosleep(&pause,NULL);
    }
    if(!ts_profile_current(&live))result=TS_SYNC_UNCONFIRMED;
    else if(rc==BR_PROFILE_CLOSED&&br_profile_error(s->profile)&&!strcmp(br_profile_error(s->profile),"token_invalid"))
        result=TS_SYNC_TOKEN_INVALID;
done:
    br_profile_close(s->profile,run,epoch);
    if(br_profile_transport_closed(s->profile,run,epoch)!=1) {
        /* No borrowed stack gate survives an incomplete transport close. */
        br_profile_free(s->profile);s->profile=NULL;result=TS_SYNC_UNCONFIRMED;
    }
    memset(&c,0,sizeof(c));s->sync_busy=0;return result;
}

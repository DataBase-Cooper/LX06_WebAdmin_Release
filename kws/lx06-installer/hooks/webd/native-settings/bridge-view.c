/* T21 isolated declaration adapter. Reuses cfg; no header/caps/voice parser. */
#include "ctl-config.h"
#include <stdio.h>
#include <string.h>

static int space(char c) { return c==' ' || c=='\t'; }
static const char *field(const struct cfg *c,const char *app,const char *name,int prefix) {
    char key[81];
    int n=prefix?snprintf(key,sizeof(key),"LX06-%s-%s",name,app):
                 snprintf(key,sizeof(key),"LX06-%s-%s",app,name);
    if(n<0||(size_t)n>=sizeof(key)) return "";
    for(char *p=key;*p;p++) if(*p>='a'&&*p<='z') *p=(char)(*p-'a'+'A');
    const char *v=cfg_value(c,key); return v?v:"";
}
static int equal(const char *p,size_t n,const char *s) {
    return n==strlen(s)&&!memcmp(p,s,n);
}
static int audio(const char *s,unsigned *present,unsigned *open) {
    *present=*open=0;
    while(space(*s)) s++;
    if(!*s) return CFG_OK;
    for(;;) {
        const char *end=strchr(s,','); if(!end) end=s+strlen(s);
        const char *a=s,*b=end;
        while(a<b&&space(*a)) a++;
        while(b>a&&space(b[-1])) b--;
        const char *eq=memchr(a,'=',(size_t)(b-a));
        if(!eq) return CFG_INVALID;
        const char *ke=eq,*v=eq+1;
        while(ke>a&&space(ke[-1])) ke--;
        while(v<b&&space(*v)) v++;
        unsigned bit=equal(a,(size_t)(ke-a),"mic")?CFG_AUDIO_MIC:
                     equal(a,(size_t)(ke-a),"speaker")?CFG_AUDIO_SPEAKER:
                     equal(a,(size_t)(ke-a),"ring")?CFG_AUDIO_RING:0;
        if(!bit||(*present&bit)) return CFG_INVALID;
        if(equal(v,(size_t)(b-v),"open")) *open|=bit;
        else if(!equal(v,(size_t)(b-v),"close")) return CFG_INVALID;
        *present|=bit;
        if(!*end) return CFG_OK;
        s=end+1; /* Empty and trailing components are rejected next iteration. */
    }
}
static int number(const char *s,uint32_t *out) {
    uint32_t n=0;
    if(!*s) return CFG_INVALID;
    for(;*s;s++) {
        if(*s<'0'||*s>'9'||n>(UINT32_MAX-(unsigned)(*s-'0'))/10) return CFG_INVALID;
        n=n*10+(unsigned)(*s-'0');
    }
    *out=n; return CFG_OK;
}
static int aliases(const char *s) {
    int meaningful=0;
    for(;;s++) {
        if(!*s||*s=='|') { if(!meaningful) return 0; meaningful=0; if(!*s) return 1; }
        else if(!space(*s)) meaningful=1;
    }
}
static enum cfg_field_state field_state(const char *s,const char *placeholder) {
    if(!s) return CFG_FIELD_ABSENT;
    if(!*s) return CFG_FIELD_EMPTY;
    return !strcmp(s,placeholder)?CFG_FIELD_PLACEHOLDER:CFG_FIELD_VALUE;
}
int cfg_metadata_view(const struct cfg *c,struct cfg_metadata *out) {
    if(!c||!out) return CFG_INVALID;
    struct cfg_metadata v={0};
    const char *mode=cfg_value(c,"LX06-AUDIO-MODE"),*alias=cfg_value(c,"LX06-BRIDGE");
    if(mode&&*mode) {
        if(!strcmp(mode,"cloud-e2e")) v.mode=CFG_MODE_CLOUD_E2E;
        else if(!strcmp(mode,"account-text")) v.mode=CFG_MODE_ACCOUNT_TEXT;
        else if(!strcmp(mode,"phone-bridge")) v.mode=CFG_MODE_PHONE_BRIDGE;
        else if(!strcmp(mode,"action")) v.mode=CFG_MODE_ACTION;
        else v.mode=CFG_MODE_UNKNOWN;
    } else if(mode) v.mode=CFG_MODE_UNKNOWN;
    if(alias) {
        if(strcmp(alias,"on")||(mode&&v.mode!=CFG_MODE_PHONE_BRIDGE)) return CFG_INVALID;
        v.mode=CFG_MODE_PHONE_BRIDGE;
    }
    v.app_count=c->app_count;
    v.bridge_enabled=v.mode==CFG_MODE_PHONE_BRIDGE;
    /* No implicit conversion of an old provider/client into a phone bridge.
     * Unknown mode preserves the old fallback without choosing an App. */
    if(v.bridge_enabled&&!c->app_count) return CFG_INVALID;
    if(c->app_count&&v.mode!=CFG_MODE_UNSPECIFIED&&v.mode!=CFG_MODE_UNKNOWN&&!v.bridge_enabled)
        return CFG_INVALID;
    const char *proto=cfg_value(c,"LX06-BRIDGE-PROTO");
    if(v.bridge_enabled&&proto) {
        if(strcmp(proto,"2.0")) return CFG_INVALID;
        v.protocol_declared=1;
    }
    const char *s=cfg_value(c,"LX06-LOCALWAKEUP");
    v.local_wakeup=s&&!strcmp(s,"on");
    s=cfg_value(c,"LX06-OFFICIALWAKEUP"); v.official_wakeup=s&&!strcmp(s,"on");
    s=cfg_value(c,"LX06-LOCAL-CHANNEL"); v.local_window=s&&*s;
    s=cfg_value(c,"小爱通道唤醒词"); v.official_channel=s&&*s;
    cfg_channel_timing(c,&v.start_ms,&v.max_ms);
    v.host_state=field_state(cfg_value(c,"LX06-PHONE-HOST"),"@@HOST@@");
    s=cfg_value(c,"LX06-PHONE-PORT"); v.port_state=field_state(s,"@@PORT@@");
    v.token_state=field_state(cfg_value(c,"LX06-PHONE-TOKEN"),"@@TOKEN@@");
    if(v.port_state==CFG_FIELD_VALUE) {
        uint32_t port;
        if(number(s,&port)||!port||port>65535) return CFG_INVALID;
        v.port=port;
    }
    *out=v; return CFG_OK;
}
const char *cfg_bridge_app_name(const struct cfg *c,size_t index) {
    return c&&index<c->app_count&&index<CFG_APPS?c->apps[index]:NULL;
}
int cfg_bridge_app_view(const struct cfg *c,const char *app,
                        enum cfg_session_kind session,struct cfg_bridge_app *out) {
    if(!c||!app||!out||session<CFG_SESSION_SILENT||session>CFG_SESSION_COMMAND) return CFG_INVALID;
    struct cfg_bridge_app v={0};
    if(cfg_control_view(c,app,&v.control)) return CFG_INVALID;
    /* Point at owned canonical identity rather than caller's transient string. */
    for(size_t a=0;a<c->app_count;a++) if(!strcmp(c->apps[a],app)) { v.app=c->apps[a]; break; }
    int rc=audio(field(c,app,"AUDIO",0),&v.audio_present,&v.audio_open); if(rc) return rc;
    const char *timeout=field(c,app,"CONNECT-TIMEOUT",0);
    if(*timeout) {
        if(number(timeout,&v.connect_timeout_ms)) return CFG_INVALID;
        v.connect_timeout_present=1;
    }
    v.ring_tone=field(c,app,"RING-TONE",0);
    v.workflow=field(c,app,"WORKFLOW",1); v.set=field(c,app,"SET",0);
    v.conn=field(c,app,"CONN",1); v.play_tpl=field(c,app,"PLAY-TPL",0);
    v.mute=field(c,app,"MUTE",1); v.unmute=field(c,app,"UNMUTE",1);
    v.muteout=field(c,app,"MUTEOUT",1);
    /* Explicit blank exits are invalid even in a silent declaration. A missing
     * irrelevant exit is fine; voice/command require their own active exit. */
    const char *exit=field(c,app,"EXIT",1),*hangup=field(c,app,"HANGUP",1);
    char key[81];
    const char *names[]={"EXIT","HANGUP"}; const char *values[]={exit,hangup};
    for(size_t i=0;i<2;i++) {
        snprintf(key,sizeof(key),"LX06-%s-%s",names[i],app);
        for(char *p=key;*p;p++) if(*p>='a'&&*p<='z') *p=(char)(*p-'a'+'A');
        if(cfg_value(c,key)&&!aliases(values[i])) return CFG_INVALID;
    }
    if((session==CFG_SESSION_VOICE&&!aliases(hangup))||
       (session==CFG_SESSION_COMMAND&&!aliases(exit))) return CFG_INVALID;
    *out=v; return CFG_OK;
}

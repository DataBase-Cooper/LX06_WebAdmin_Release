#define _GNU_SOURCE
#include "ctl-config.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int ascii_space(char c) { return c==' ' || c=='\t'; }
static char *trim(char *s) {
    while (ascii_space(*s)) s++;
    size_t n=strlen(s);
    while(n && ascii_space(s[n-1])) s[--n]=0;
    return s;
}
int cfg_utf8_valid(const char *s, size_t n) {
    if(!s) return 0;
    for(size_t i=0;i<n;) {
        unsigned char a=(unsigned char)s[i++];
        if(a<128) { if(!a || (a<32 && a!='\n' && a!='\r' && a!='\t')) return 0; continue; }
        unsigned need; uint32_t cp, minimum;
        if(a>=0xc2 && a<=0xdf) { need=1; cp=a&31; minimum=128; }
        else if(a>=0xe0 && a<=0xef) { need=2; cp=a&15; minimum=2048; }
        else if(a>=0xf0 && a<=0xf4) { need=3; cp=a&7; minimum=65536; }
        else return 0;
        if(i+need>n) return 0;
        while(need--) { unsigned char b=(unsigned char)s[i++]; if((b&0xc0)!=0x80) return 0; cp=(cp<<6)|(b&63); }
        if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return 0;
    }
    return 1;
}
static int uint32_text(const char *s, uint32_t *out) {
    uint32_t v=0;
    if(!*s) { *out=0; return 1; }
    for(;*s;s++) {
        if(*s<'0'||*s>'9'||v>(UINT32_MAX-(unsigned)(*s-'0'))/10) return 0;
        v=v*10+(unsigned)(*s-'0');
    }
    *out=v; return 1;
}
static int var_char(char c) { return (c>='a'&&c<='z') || c=='_'; }
static int vars(const char *s, size_t n, char names[4][17], size_t *count) {
    *count=0;
    for(size_t i=0;i<n;i++) {
        if(s[i]=='}') return CFG_INVALID;
        if(s[i]!='{') continue;
        size_t start=++i;
        while(i<n && var_char(s[i])) i++;
        size_t k=i-start;
        if(!k||k>16||i==n||s[i]!='}') return CFG_INVALID;
        size_t j;
        for(j=0;j<*count;j++) if(strlen(names[j])==k&&!memcmp(names[j],s+start,k)) break;
        if(j==*count) {
            if(*count==4) return CFG_CAPACITY;
            memcpy(names[j],s+start,k); names[j][k]=0; (*count)++;
        }
    }
    return CFG_OK;
}
static int same_vars(char a[4][17], size_t na, char b[4][17], size_t nb) {
    if(na!=nb) return 0;
    for(size_t i=0;i<na;i++) {
        size_t j=0; while(j<nb&&strcmp(a[i],b[j])) j++;
        if(j==nb) return 0;
    }
    return 1;
}
static int cfg_legacy_parse_row(const char *raw, struct cfg_row *out) {
    if(!raw||!out) return CFG_INVALID;
    size_t n=strnlen(raw,CFG_BYTES+1);
    if(n>CFG_BYTES) return CFG_CAPACITY;
    if(!cfg_utf8_valid(raw,n)||strpbrk(raw,"\r\n")) return CFG_INVALID;
    const char *arrow=strstr(raw," => ");
    if(!arrow||strstr(arrow+4," => ")) return CFG_INVALID;
    struct cfg_row r={0};
    const char *p=raw, *end=arrow;
    while(p<end) {
        const char *q=memchr(p,'|',(size_t)(end-p)); if(!q) q=end;
        const char *a=p,*b=q;
        while(a<b&&ascii_space(*a)) a++;
        while(b>a&&ascii_space(b[-1])) b--;
        /* Existing matcher has a 160-byte pattern buffer. */
        if(a==b || b-a>=160) return CFG_INVALID;
        char found[4][17]={{0}}; size_t nf;
        int rc=vars(a,(size_t)(b-a),found,&nf); if(rc) return rc;
        if(!r.phrase_count) { memcpy(r.refs,found,sizeof(found)); r.ref_count=nf; }
        else if(!same_vars(r.refs,r.ref_count,found,nf)) return CFG_INVALID;
        for(const char *prior=raw;prior<p;) {
            const char *e=memchr(prior,'|',(size_t)(end-prior)); if(!e) e=end;
            const char *x=prior,*y=e;
            while(x<y&&ascii_space(*x)) x++;
            while(y>x&&ascii_space(y[-1])) y--;
            if(y-x==b-a&&!memcmp(x,a,(size_t)(b-a))) return CFG_INVALID;
            prior=e+1;
        }
        if(++r.phrase_count>CFG_CANDIDATES) return CFG_CAPACITY;
        if(q==end) break;
        p=q+1; if(p==end) return CFG_INVALID;
    }
    if(!r.phrase_count) return CFG_INVALID;
    p=arrow+4; while(ascii_space(*p)) p++;
    const char *q=p; while(*q&&!ascii_space(*q)) q++;
    if(q==p || (size_t)(q-p)>=sizeof(r.op)) return CFG_INVALID;
    memcpy(r.op,p,(size_t)(q-p));
    if(!strncmp(r.op,"flow:",5)) return CFG_PENDING_FLOW;
    if(strpbrk(r.op,"{}")) return CFG_INVALID;
    char refs[4][17]={{0}}; size_t nr=0;
    p=q;
    while(*p) {
        while(ascii_space(*p)) p++;
        if(!*p) break;
        q=p; while(*q&&!ascii_space(*q)) q++;
        const char *eq=memchr(p,'=',(size_t)(q-p));
        if(!eq||eq==p||eq+1==q||eq-p>=81||r.arg_count==CFG_ARGS) return CFG_INVALID;
        struct cfg_arg *arg=&r.args[r.arg_count];
        memcpy(arg->key,p,(size_t)(eq-p));
        if(strpbrk(arg->key,"{}")) return CFG_INVALID;
        for(size_t i=0;i<r.arg_count;i++) if(!strcmp(r.args[i].key,arg->key)) return CFG_INVALID;
        arg->offset=(size_t)(eq+1-raw); arg->length=(size_t)(q-eq-1);
        char found[4][17]={{0}}; size_t nf;
        int rc=vars(eq+1,arg->length,found,&nf); if(rc) return rc;
        if(nf) {
            if(nf!=1||eq[1]!='{'||q[-1]!='}'||arg->length!=strlen(found[0])+2) return CFG_INVALID;
            strcpy(arg->ref,found[0]);
            size_t i=0; while(i<nr&&strcmp(refs[i],found[0])) i++;
            if(i==nr) { if(nr==4) return CFG_CAPACITY; strcpy(refs[nr++],found[0]); }
        } else {
            for(const char *v=eq+1;v<q;v++) if(strchr("\"'\\",*v)) return CFG_INVALID;
        }
        r.arg_count++; p=q;
    }
    if(!same_vars(r.refs,r.ref_count,refs,nr)) return CFG_INVALID;
    *out=r; return CFG_OK;
}

#include "ctl-expression.inc"

static int app_ident(const char *s) {
    size_t n=strlen(s); if(!n||n>32||s[0]<'a'||s[0]>'z') return 0;
    for(;*s;s++) if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_')) return 0;
    return 1;
}
static int app_key(char key[81],const char *app,const char *suffix) {
    if(!app_ident(app)) return 0;
    int n=snprintf(key,81,"LX06-%s-%s",app,suffix);
    if(n<0||n>=81) return 0;
    for(char *p=key;*p;p++) if(*p>='a'&&*p<='z') *p=(char)(*p-'a'+'A');
    return 1;
}
const char *cfg_value(const struct cfg *c,const char *key) {
    if(!c||!key) return NULL;
    for(size_t i=0;i<c->count;i++) if(!strcmp(c->entries[i].key,key)) return c->arena+c->entries[i].offset;
    return NULL;
}
static int is_ctl(const char *key) {
    size_t n=strlen(key); return n>4&&!strcmp(key+n-4,"-CTL");
}
static int recognized(const char *key) {
    static const char *const global[]={"LX06-LOCAL-CHANNEL","小爱通道唤醒词",
        "LX06-CHANNEL-WINDOW-START-MS","LX06-CHANNEL-WINDOW-MAX-MS","LX06-CHANNEL-WINDOW-MS",
        "LX06-PHONE-APP","LX06-PHONE-APPS","LX06-PHONE-HOST","LX06-PHONE-PORT","LX06-PHONE-TOKEN","LX06-PHONE-LABEL",
        "LX06-BRIDGE","LX06-BRIDGE-PROTO","LX06-AUDIO-MODE","LX06-LOCALWAKEUP","LX06-OFFICIALWAKEUP","LX06-RECOGNITION-FIELD"};
    static const char *const suffix[]={"CTL","BARGEIN","CTL-WINDOW-START-MS","CTL-WINDOW-MAX-MS",
        "CTL-WINDOW-MS","CTL-TIMEOUT","AUDIO","SET","RING-TONE","CONNECT-TIMEOUT","PLAY-TPL"};
    static const char *const prefix[]={"LX06-CONN-","LX06-EXIT-","LX06-HANGUP-","LX06-MUTE-",
        "LX06-UNMUTE-","LX06-MUTEOUT-","LX06-WORKFLOW-"};
    for(size_t i=0;i<sizeof(global)/sizeof(*global);i++) if(!strcmp(key,global[i])) return 1;
    for(size_t i=0;i<sizeof(prefix)/sizeof(*prefix);i++)
        if(!strncmp(key,prefix[i],strlen(prefix[i]))&&key[strlen(prefix[i])]) return 1;
    if(strncmp(key,"LX06-",5)) return 0;
    for(size_t i=0;i<sizeof(suffix)/sizeof(*suffix);i++) {
        size_t nk=strlen(key),ns=strlen(suffix[i]);
        if(nk>ns+6&&key[nk-ns-1]=='-'&&!strcmp(key+nk-ns,suffix[i])) return 1;
    }
    return 0;
}
static int add(struct cfg *c,const char *key,const char *value,int overridden) {
    size_t n=strlen(value)+1;
    if(strlen(key)>=sizeof(c->entries[0].key)) return CFG_CAPACITY;
    if(!is_ctl(key)) {
        const char *old=cfg_value(c,key);
        if(old) return !strcmp(key,"LX06-RECOGNITION-FIELD")||strcmp(old,value)?CFG_INVALID:CFG_OK;
    }
    if(c->count==CFG_ENTRIES||c->used+n>sizeof(c->arena)) return CFG_CAPACITY;
    struct cfg_entry *e=&c->entries[c->count++];
    strcpy(e->key,key); e->offset=(uint32_t)c->used; e->overridden=(unsigned char)overridden;
    memcpy(c->arena+c->used,value,n); c->used+=n;
    return CFG_OK;
}
static int validate_rows(struct cfg *c,cfg_list_approve approve,void *ctx) {
    const char *apps=cfg_value(c,"LX06-PHONE-APPS");
    const char *legacy=cfg_value(c,"LX06-PHONE-APP"); c->app_count=0;
    /* Old single-App declarations feed the same effective object. An explicit
     * APPS list never silently falls back to APP, including an empty list. */
    if(!apps) apps=legacy;
    if(apps&&*apps) {
        char list[CFG_DECL_BYTES+1]; strcpy(list,apps); char *p=list;
        while(p) {
            char *end=strchr(p,','); if(end) *end++=0;
            p=trim(p);
            if(!app_ident(p)) return CFG_INVALID;
            if(c->app_count==CFG_APPS) return CFG_CAPACITY;
            for(size_t i=0;i<c->app_count;i++) if(!strcmp(c->apps[i],p)) return CFG_INVALID;
            strcpy(c->apps[c->app_count++],p); p=end;
        }
    }
    if(legacy && (!app_ident(legacy) || c->app_count!=1 || strcmp(c->apps[0],legacy)))
        return CFG_INVALID;
    for(size_t i=0;i<c->count;i++) {
        const char *key=c->entries[i].key,*v=c->arena+c->entries[i].offset;
        if(strstr(key,"-WINDOW-")||strstr(key,"-CONNECT-TIMEOUT")||!strcmp(key,"LX06-PHONE-PORT")) {
            /* Public generated scripts carry this exact port placeholder.
             * It is unresolved declaration data, never a numeric default. */
            if(strcmp(key,"LX06-PHONE-PORT")||strcmp(v,"@@PORT@@")) {
                uint32_t number; if(!uint32_text(v,&number)) return CFG_INVALID;
            }
        }
        size_t n=strlen(key);
        if(n>=12&&!strcmp(key+n-12,"-CTL-TIMEOUT")&&*v&&strcmp(v,"resume")&&strcmp(v,"end")) return CFG_INVALID;
        if(is_ctl(key)&&*v) { struct cfg_row row; int rc=cfg_parse_row_scoped(key,v,&row,approve,ctx); if(rc) return rc; }
    }
    /* Candidate capacity applies separately to each app's control scope. */
    for(size_t a=0;a<c->app_count;a++) {
        char key[81]; app_key(key,c->apps[a],"CTL"); size_t count=0;
        for(size_t i=0;i<c->count;i++) if(!strcmp(key,c->entries[i].key)) {
            const char *v=c->arena+c->entries[i].offset;
            if(*v) { struct cfg_row r; int rc=cfg_parse_row_scoped(key,v,&r,approve,ctx); if(rc) return rc; count+=r.phrase_count; }
        }
        const char *types[]={"EXIT","HANGUP"};
        for(size_t j=0;j<2;j++) {
            snprintf(key,sizeof(key),"LX06-%s-%s",types[j],c->apps[a]);
            for(char *p=key;*p;p++) if(*p>='a'&&*p<='z') *p=(char)(*p-'a'+'A');
            const char *v=cfg_value(c,key);
            if(v&&*v) { count++; for(;*v;v++) if(*v=='|') count++; }
        }
        if(count>CFG_CANDIDATES) return CFG_CAPACITY;
    }
    return cfg_expr_channels(c);
}
static int cfg_parse_common(const char *header,size_t length,int script,
        struct cfg *out,size_t *extent,cfg_list_approve approve,void *ctx) {
    if(!header||!out) return CFG_INVALID;
    if(!script && length>CFG_DECL_BYTES) return CFG_CAPACITY;
    if(!script && !cfg_utf8_valid(header,length)) return CFG_INVALID;
    struct cfg *tmp=calloc(1,sizeof(*tmp)); if(!tmp) return CFG_CAPACITY;
    size_t limit=script && length>CFG_DECL_BYTES?CFG_DECL_BYTES+1:length;
    char line[CFG_DECL_BYTES+2]; size_t pos=0; int rc=CFG_OK;
    while(pos<limit) {
        size_t end=pos; while(end<limit&&header[end]!='\n') end++;
        size_t n=end-pos;
        memcpy(line,header+pos,n); line[n]=0;
        if(n&&line[n-1]=='\r') line[--n]=0;
        char *p=line; while(ascii_space(*p)) p++;
        /* Original classification is shared by both modes. A complete upload
         * stops before body bytes; no second header/metadata grammar. */
        if(script && *p && *p!='#') break;
        if(script) {
            if(end==limit || end+1>CFG_DECL_BYTES) {
                rc=end==limit && length<=CFG_DECL_BYTES?CFG_INVALID:CFG_CAPACITY;
                break;
            }
            if(!cfg_utf8_valid(header+pos,end+1-pos)) { rc=CFG_INVALID; break; }
        }
        if(!*p) { pos=end+(end<length); continue; }
        if(*p!='#') break;
        char *colon=strchr(p+1,':');
        /* The existing official router uses this historical '=' spelling.
         * Recognize it only for that key; CTL argument '=' is value data. */
        char *body=p+1; while(ascii_space(*body)) body++;
        const char *official="小爱通道唤醒词"; size_t official_n=strlen(official);
        if(!strncmp(body,official,official_n)) {
            char *eq=body+official_n; while(ascii_space(*eq)) eq++;
            if(*eq=='=') colon=eq;
        }
        if(colon) {
            *colon=0; const char *key=trim(p+1);
            if(recognized(key)) {
                /* Recognized metadata cut at caller's read boundary is unsafe. */
                if(end==length) { rc=CFG_INVALID; break; }
                char *value=colon+1; while(ascii_space(*value)) value++;
                if(!is_ctl(key)) value=trim(value);
                rc=add(tmp,key,value,0); if(rc) break;
            }
        }
        pos=end+(end<length);
    }
    if(!rc) rc=validate_rows(tmp,approve,ctx);
    if(!rc) { *out=*tmp; if(extent) *extent=pos; }
    free(tmp); return rc;
}
static int cfg_effective_impl(const struct cfg *defaults,const struct cfg_override *overrides,size_t count,struct cfg *out,cfg_list_approve approve,void *ctx) {
    if(!defaults||!out||(count&&!overrides)||count>CFG_ENTRIES) return CFG_INVALID;
    struct cfg *tmp=calloc(1,sizeof(*tmp)); if(!tmp) return CFG_CAPACITY;
    int rc=CFG_OK;
    for(size_t j=0;j<count&&!rc;j++) {
        const char *k=overrides[j].key,*v=overrides[j].value;
        if(!k||!v||strnlen(k,81)>=81||strnlen(v,CFG_BYTES+1)>CFG_BYTES||!recognized(k)||
           !strcmp(k,"LX06-PHONE-TOKEN")|| /* T22 must use explicit secret lifecycle, never generic overlay. */
           !cfg_utf8_valid(v,strlen(v))||strchr(v,'\r')||(!is_ctl(k)&&strchr(v,'\n'))) { rc=CFG_INVALID; break; }
        for(size_t i=0;i<j;i++) if(!strcmp(overrides[i].key,k)) rc=CFG_INVALID;
    }
    for(size_t i=0;i<defaults->count&&!rc;i++) {
        const struct cfg_entry *e=&defaults->entries[i]; size_t j=0;
        while(j<count&&strcmp(overrides[j].key,e->key)) j++;
        if(j==count) rc=add(tmp,e->key,defaults->arena+e->offset,0);
    }
    for(size_t j=0;j<count&&!rc;j++) {
        const char *k=overrides[j].key,*v=overrides[j].value;
        if(!is_ctl(k)) rc=add(tmp,k,v,1);
        else {
            char lines[CFG_BYTES+1]; strcpy(lines,v); char *p=lines;
            for(;;) {
                char *end=strchr(p,'\n'); if(end) *end=0;
                const char *nonblank=p; while(ascii_space(*nonblank)) nonblank++;
                if(!*nonblank&&*v) { rc=CFG_INVALID; break; }
                rc=add(tmp,k,p,1); if(rc||!end) break; p=end+1;
            }
        }
    }
    if(!rc) rc=validate_rows(tmp,approve,ctx);
    if(!rc) *out=*tmp;
    free(tmp); return rc;
}
int cfg_parse(const char *header,size_t length,struct cfg *out) {
    return cfg_parse_common(header,length,0,out,NULL,NULL,NULL);
}
int cfg_parse_rows(const char *header,size_t length,struct cfg *out,cfg_list_approve approve,void *ctx) {
    return approve?cfg_parse_common(header,length,0,out,NULL,approve,ctx):CFG_INVALID;
}
int cfg_parse_script(const char *script,size_t length,struct cfg *out,size_t *extent) {
    return extent?cfg_parse_common(script,length,1,out,extent,NULL,NULL):CFG_INVALID;
}
int cfg_parse_script_rows(const char *script,size_t length,struct cfg *out,size_t *extent,
        cfg_list_approve approve,void *ctx) {
    return extent&&approve?cfg_parse_common(script,length,1,out,extent,approve,ctx):CFG_INVALID;
}
int cfg_effective(const struct cfg *defaults,const struct cfg_override *overrides,size_t count,struct cfg *out) {
    return cfg_effective_impl(defaults,overrides,count,out,NULL,NULL);
}
int cfg_effective_rows(const struct cfg *defaults,const struct cfg_override *overrides,size_t count,struct cfg *out,cfg_list_approve approve,void *ctx) {
    return approve?cfg_effective_impl(defaults,overrides,count,out,approve,ctx):CFG_INVALID;
}
const char *cfg_ctl_row(const struct cfg *c,const char *app,size_t index) {
    char key[81]; if(!c||!app_key(key,app,"CTL")) return NULL;
    for(size_t i=0;i<c->count;i++) if(!strcmp(c->entries[i].key,key)) {
        const char *v=c->arena+c->entries[i].offset; if(*v&&!index--) return v;
    }
    return NULL;
}
static unsigned timing(const char *s,unsigned fallback,unsigned minimum) {
    uint32_t v; if(!s||!uint32_text(s,&v)||!v) return fallback;
    return v<minimum?minimum:(v>15000?15000:v);
}
static void timings(const struct cfg *c,const char *prefix,unsigned *start,unsigned *max) {
    char key[81]; snprintf(key,sizeof(key),"%s-WINDOW-START-MS",prefix);
    *start=timing(cfg_value(c,key),5000,1000);
    snprintf(key,sizeof(key),"%s-WINDOW-MAX-MS",prefix); const char *v=cfg_value(c,key);
    if(!v||!*v) { snprintf(key,sizeof(key),"%s-WINDOW-MS",prefix); v=cfg_value(c,key); }
    *max=timing(v,10000,3000);
}
void cfg_channel_timing(const struct cfg *c,unsigned *start,unsigned *max) {
    timings(c,"LX06-CHANNEL",start,max);
}
int cfg_control_view(const struct cfg *c,const char *app,struct cfg_control *out) {
    char prefix[81],key[81]; if(!c||!out||!app_key(prefix,app,"CTL")) return CFG_INVALID;
    size_t a=0; while(a<c->app_count&&strcmp(c->apps[a],app)) a++;
    if(a==c->app_count) return CFG_INVALID;
    struct cfg_control v={0}; timings(c,prefix,&v.start_ms,&v.max_ms);
    v.enabled=cfg_ctl_row(c,app,0)!=NULL;
    app_key(key,app,"CTL-TIMEOUT"); const char *timeout=cfg_value(c,key);
    v.end_on_timeout=timeout&&!strcmp(timeout,"end");
    app_key(key,app,"BARGEIN"); v.bargein=cfg_value(c,key); if(!v.bargein) v.bargein="";
    const char *types[]={"EXIT","HANGUP"}; const char **fields[]={&v.exit,&v.hangup};
    for(size_t i=0;i<2;i++) {
        snprintf(key,sizeof(key),"LX06-%s-%s",types[i],app);
        for(char *p=key;*p;p++) if(*p>='a'&&*p<='z') *p=(char)(*p-'a'+'A');
        *fields[i]=cfg_value(c,key); if(!*fields[i]) *fields[i]="";
    }
    *out=v; return CFG_OK;
}
int cfg_require_exit(const struct cfg *c,const char *app,int voice) {
    struct cfg_control v; if(cfg_control_view(c,app,&v)) return CFG_INVALID;
    const char *s=voice?v.hangup:v.exit;
    while(ascii_space(*s)) s++;
    return *s?CFG_OK:CFG_INVALID;
}

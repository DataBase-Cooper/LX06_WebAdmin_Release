/* PROVISIONAL(P-20261111-1235-A). Included only by the isolated helper patch.
 * The shipped tokenizer/schema and native cfg parser remain the only parsers.
 * No caps, transport, TTS, shell sourcing, secret sync, or production entry. */
#include "ctl-config.h"
#include "ctl-list-approval.h"
#define CTL_RECORD_KEY "lx06_ctl_v1"

struct ctl_buf { char *data; size_t used; };
struct ctl_map {
    char keys[CFG_ENTRIES][81], arena[CFG_BYTES+1];
    size_t offsets[CFG_ENTRIES], count, used;
};
struct ctl_edit {
    struct cfg defaults;
    struct ctl_map request;
    int restoring;
    char public_json[CFG_BYTES+1];
};

static int ctl_bytes(struct ctl_buf *b,const char *s,size_t n) {
    if(n>CFG_BYTES-b->used) return -1;
    memcpy(b->data+b->used,s,n); b->used+=n; b->data[b->used]=0; return 0;
}
static int ctl_put(struct ctl_buf *b,const char *s) { return ctl_bytes(b,s,strlen(s)); }
static int ctl_quote(struct ctl_buf *b,const char *s) {
    if(ctl_put(b,"\"")) return -1;
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s; char x[7];
        if(c=='"'||c=='\\') { x[0]='\\'; x[1]=(char)c; x[2]=0; }
        else if(c<32) snprintf(x,sizeof(x),"\\u%04x",c);
        else { x[0]=(char)c; x[1]=0; }
        if(ctl_put(b,x)) return -1;
    }
    return ctl_put(b,"\"");
}
static int ctl_number(struct ctl_buf *b,unsigned n) {
    char value[16]; snprintf(value,sizeof(value),"%u",n); return ctl_put(b,value);
}
static int ctl_find(const struct ctl_map *m,const char *key) {
    for(size_t i=0;i<m->count;i++) if(!strcmp(m->keys[i],key)) return (int)i;
    return -1;
}
static int ctl_add(struct ctl_map *m,const char *key,const char *value) {
    size_t n=strlen(value)+1;
    if(m->count==CFG_ENTRIES||strlen(key)>80||m->used+n>sizeof(m->arena)||ctl_find(m,key)>=0) return -1;
    strcpy(m->keys[m->count],key); m->offsets[m->count++]=m->used;
    memcpy(m->arena+m->used,value,n); m->used+=n; return 0;
}
/* Return 1 for an editable global, 2 for an editable App key, 0 otherwise.
 * Extract syntax only; semantics/ranges are delegated to cfg_effective. */
static int ctl_key(const char *key,char app[33]) {
    static const char *const globals[]={"LX06-LOCAL-CHANNEL","小爱通道唤醒词",
        "LX06-CHANNEL-WINDOW-START-MS","LX06-CHANNEL-WINDOW-MAX-MS","LX06-CHANNEL-WINDOW-MS"};
    static const char *const suffix[]={"-CTL","-BARGEIN","-CTL-WINDOW-START-MS",
        "-CTL-WINDOW-MAX-MS","-CTL-WINDOW-MS","-CTL-TIMEOUT"};
    app[0]=0;
    for(size_t i=0;i<sizeof(globals)/sizeof(*globals);i++) if(!strcmp(key,globals[i])) return 1;
    const char *start=NULL; size_t n=0;
    if(!strncmp(key,"LX06-EXIT-",10)) { start=key+10; n=strlen(start); }
    else if(!strncmp(key,"LX06-HANGUP-",12)) { start=key+12; n=strlen(start); }
    else if(!strncmp(key,"LX06-",5)) {
        for(size_t i=0;i<sizeof(suffix)/sizeof(*suffix);i++) {
            size_t nk=strlen(key),ns=strlen(suffix[i]);
            if(nk>ns+5&&!strcmp(key+nk-ns,suffix[i])) { start=key+5; n=nk-ns-5; break; }
        }
    }
    if(!start||!n||n>32||start[0]<'A'||start[0]>'Z') return 0;
    for(size_t i=0;i<n;i++) {
        char c=start[i];
        if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')) return 0;
        app[i]=(c>='A'&&c<='Z')?(char)(c-'A'+'a'):c;
    }
    app[n]=0; return 2;
}
static int ctl_active(const struct cfg *defaults,const char *key) {
    char app[33]; int kind=ctl_key(key,app);
    if(kind==1) return 1;
    if(kind==2) for(size_t i=0;i<defaults->app_count;i++) if(!strcmp(app,defaults->apps[i])) return 1;
    return 0;
}
static int ctl_exit_value(const char *key,const char *value) {
    if(strncmp(key,"LX06-EXIT-",10)&&strncmp(key,"LX06-HANGUP-",12)) return 1;
    int meaningful=0;
    for(;;value++) {
        if(!*value||*value=='|') { if(!meaningful) return 0; meaningful=0; if(!*value) return 1; }
        else if(*value!=' '&&*value!='\t') meaningful=1;
    }
}
static int ctl_map_parse(const char *raw,struct ctl_map *map) {
    xz_json_doc d;
    if(!cfg_utf8_valid(raw,strlen(raw))||xz_json_parse(&d,raw,strlen(raw))||d.tokens[0].type!=XZ_JSON_OBJECT) return -1;
    memset(map,0,sizeof(*map));
    for(int k=d.tokens[0].first_child;k>=0;) {
        int v=d.tokens[k].next_sibling; char key[81],value[CFG_BYTES+1],app[33];
        if(v<0||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||!ctl_key(key,app)||
           !ext_text(&d,0,key,value,sizeof(value),0)||!cfg_utf8_valid(value,strlen(value))||
           !ctl_exit_value(key,value)||ctl_add(map,key,value)) return -1;
        k=d.tokens[v].next_sibling;
    }
    return 0;
}
static int ctl_stored_map(const char *current,struct ctl_map *map) {
    xz_json_doc d; char raw[CFG_BYTES+1];
    if(xz_json_parse(&d,current,strlen(current))) return -1;
    if(!ext_text(&d,0,CTL_RECORD_KEY,raw,sizeof(raw),0)) return -1;
    if(xz_json_object_get(&d,0,CTL_RECORD_KEY)<0) { memset(map,0,sizeof(*map)); return 0; }
    return ctl_map_parse(raw,map);
}
/* The legacy helper is also built without the revision-12 compiler. Keep
 * that build independent of typed-only symbols and refuse typed requests. */
static int ctl_cfg_effective(const struct cfg *defaults,const struct cfg_override *overrides,
        size_t count,struct cfg *effective,cfg_list_approve approve,void *ctx,cfg_startup_approve startup) {
#if defined(LX06_NATIVE_TARGET_SETTINGS) || defined(LX06_NATIVE_TARGET_SETTINGS)
#ifdef LX06_STARTUP_LIST_FIXTURE
    if(startup)return cfg_effective_startup(defaults,overrides,count,effective,approve,ctx,startup);
#else
    if(startup)return CFG_INVALID;
#endif
    if(approve)return cfg_effective_rows(defaults,overrides,count,effective,approve,ctx);
#else
    (void)ctx;if(approve||startup)return CFG_INVALID;
#endif
    return cfg_effective(defaults,overrides,count,effective);
}
static int ctl_effective_impl(const struct cfg *defaults,const struct ctl_map *map,struct cfg *effective,
        cfg_list_approve approve,void *ctx,cfg_startup_approve startup) {
    struct cfg_override overrides[CFG_ENTRIES]; size_t n=0;
    for(size_t i=0;i<map->count;i++) overrides[i]=(struct cfg_override){map->keys[i],map->arena+map->offsets[i]};
    /* Validate even dormant overrides. Never silently drop malformed data. */
    int rc=ctl_cfg_effective(defaults,overrides,map->count,effective,approve,ctx,startup); if(rc) return rc;
    for(size_t i=0;i<map->count;i++) if(ctl_active(defaults,map->keys[i]))
        overrides[n++]=(struct cfg_override){map->keys[i],map->arena+map->offsets[i]};
    return ctl_cfg_effective(defaults,overrides,n,effective,approve,ctx,startup);
}
static int ctl_effective(const struct cfg *defaults,const struct ctl_map *map,struct cfg *effective) {
    return ctl_effective_impl(defaults,map,effective,NULL,NULL,NULL);
}
static int ctl_effective_rows(const struct cfg *defaults,const struct ctl_map *map,struct cfg *effective,
        cfg_list_approve approve,void *ctx) {
    return approve?ctl_effective_impl(defaults,map,effective,approve,ctx,NULL):CFG_INVALID;
}
static int ctl_effective_startup(const struct cfg *defaults,const struct ctl_map *map,struct cfg *effective,
        cfg_list_approve approve,void *ctx,cfg_startup_approve startup) {
    return startup?ctl_effective_impl(defaults,map,effective,approve,ctx,startup):CFG_INVALID;
}
static int ctl_public(const struct cfg *c,const struct ctl_map *map,char out[CFG_BYTES+1]) {
    /* One cfg parser supplies both metadata and per-App views. No second scan
     * of the uploaded script, and no raw endpoint/secret in public JSON. */
    struct cfg_metadata metadata;
    if(cfg_metadata_view(c,&metadata)) return -1;
    struct ctl_buf b={out,0}; unsigned start,maximum; cfg_channel_timing(c,&start,&maximum);
    const char *local=cfg_value(c,"LX06-LOCAL-CHANNEL"),*official=cfg_value(c,"小爱通道唤醒词");
    if(ctl_put(&b,"{\"version\":1,\"status\":\"offline_unverified\",\"channel\":{\"local\":")||
       ctl_quote(&b,local?local:"")||ctl_put(&b,",\"official\":")||ctl_quote(&b,official?official:"")||
       ctl_put(&b,",\"start_ms\":")||ctl_number(&b,start)||ctl_put(&b,",\"max_ms\":")||ctl_number(&b,maximum)||
       ctl_put(&b,"},\"apps\":[")) return -1;
    for(size_t a=0;a<c->app_count;a++) {
        /* Same declaration view as the future bridge consumer. Validate local
         * syntax before constructing output/renaming a saved record. Session
         * kind is unknown offline, so caps-specific exit requirements stay
         * pending; never infer them from an App name or the mic setting. */
        struct cfg_bridge_app declared;
        if(cfg_bridge_app_view(c,c->apps[a],CFG_SESSION_SILENT,&declared)) return -1;
        struct cfg_control view=declared.control;
        if((a&&ctl_put(&b,","))||ctl_put(&b,"{\"app\":")||ctl_quote(&b,c->apps[a])||ctl_put(&b,",\"rows\":[")) return -1;
        for(size_t i=0;;i++) {
            const char *row=cfg_ctl_row(c,c->apps[a],i); if(!row) break;
            if((i&&ctl_put(&b,","))||ctl_quote(&b,row)) return -1;
        }
        if(ctl_put(&b,"],\"start_ms\":")||ctl_number(&b,view.start_ms)||ctl_put(&b,",\"max_ms\":")||ctl_number(&b,view.max_ms)||
           ctl_put(&b,",\"timeout\":")||ctl_quote(&b,view.end_on_timeout?"end":"resume")||
           ctl_put(&b,",\"bargein\":")||ctl_quote(&b,view.bargein)||ctl_put(&b,",\"exit\":")||ctl_quote(&b,view.exit)||
           ctl_put(&b,",\"hangup\":")||ctl_quote(&b,view.hangup)||ctl_put(&b,"}")) return -1;
    }
    if(ctl_put(&b,"],\"overridden\":[")) return -1;
    for(size_t i=0;i<map->count;i++) if((i&&ctl_put(&b,","))||ctl_quote(&b,map->keys[i])) return -1;
    return ctl_put(&b,"]}");
}
static int ctl_map_json(const struct ctl_map *map,char out[CFG_BYTES+1]) {
    struct ctl_buf b={out,0}; if(ctl_put(&b,"{")) return -1;
    for(size_t i=0;i<map->count;i++) if((i&&ctl_put(&b,","))||ctl_quote(&b,map->keys[i])||
        ctl_put(&b,":")||ctl_quote(&b,map->arena+map->offsets[i])) return -1;
    return ctl_put(&b,"}");
}
/* Preserve complete existing raw key/value tokens, including escaped secrets
 * and fields removed from a reuploaded schema; replace only owned fields. */
static int ctl_merge(const char *current,const char *known,int replacing_ctl,
                     const struct ctl_map *map,char next[CFG_BYTES+1]) {
    xz_json_doc d; if(xz_json_parse(&d,current,strlen(current))) return -1;
    struct ctl_buf b={next,0}; size_t count=0;
    if(ctl_put(&b,"{")) return -1;
    for(int k=d.tokens[0].first_child;k>=0;) {
        int v=d.tokens[k].next_sibling; char key[33];
        if(v<0||decode_string(&d,k,key,sizeof(key))) return -1;
        int skip=replacing_ctl?!strcmp(key,CTL_RECORD_KEY):ext_field_index(&schema,key)>=0;
        if(!skip) {
            if((count++&&ctl_put(&b,","))||ctl_bytes(&b,current+d.tokens[k].start-1,
                d.tokens[v].end-d.tokens[k].start+2)) return -1;
        }
        k=d.tokens[v].next_sibling;
    }
    if(replacing_ctl&&map->count) {
        char inner[CFG_BYTES+1]; if(ctl_map_json(map,inner)) return -1;
        if((count++&&ctl_put(&b,","))||ctl_quote(&b,CTL_RECORD_KEY)||ctl_put(&b,":")||ctl_quote(&b,inner)) return -1;
    } else if(!replacing_ctl) {
        size_t n=strlen(known);
        if(n>2&&((count&&ctl_put(&b,","))||ctl_bytes(&b,known+1,n-2))) return -1;
    }
    return ctl_put(&b,"}");
}
static int ctl_legacy_edit(const char *current,char next[CFG_BYTES+1],void *ctx) {
    return ctl_merge(current,(const char *)ctx,0,NULL,next);
}
static int ctl_edit_record_impl(const char *current,char next[CFG_BYTES+1],void *opaque,
        cfg_list_approve approve,void *ctx,cfg_startup_approve startup) {
    struct ctl_edit *edit=opaque;
    struct ctl_map *old=calloc(1,sizeof(*old)),*merged=calloc(1,sizeof(*merged));
    struct cfg *effective=malloc(sizeof(*effective)); int rc=-1;
    if(!old||!merged||!effective||ctl_stored_map(current,old)) goto done;
    for(size_t i=0;i<old->count;i++) {
        int j=ctl_find(&edit->request,old->keys[i]);
        if(j>=0&&edit->restoring) continue;
        const char *v=j>=0?edit->request.arena+edit->request.offsets[j]:old->arena+old->offsets[i];
        if(ctl_add(merged,old->keys[i],v)) goto done;
    }
    if(!edit->restoring) for(size_t i=0;i<edit->request.count;i++)
        if(ctl_find(old,edit->request.keys[i])<0&&ctl_add(merged,edit->request.keys[i],edit->request.arena+edit->request.offsets[i])) goto done;
    if(ctl_effective_impl(&edit->defaults,merged,effective,approve,ctx,startup)||ctl_public(effective,merged,edit->public_json)||
       ctl_merge(current,NULL,1,merged,next)) goto done;
    rc=0;
done:
    free(old); free(merged); free(effective); return rc;
}
static int ctl_edit_record(const char *current,char next[CFG_BYTES+1],void *opaque) {
    return ctl_edit_record_impl(current,next,opaque,NULL,NULL,NULL);
}
static int ctl_request_bytes(struct ctl_edit *edit,const char *raw,size_t n) {
    xz_json_doc d;
    if(!n||n>CFG_BYTES||memchr(raw,0,n)||!cfg_utf8_valid(raw,n)||
       xz_json_parse(&d,raw,n)||!ext_keys(&d,0,edit->restoring?"|version||keys|":"|version||values|")) return -1;
    int version=xz_json_object_get(&d,0,"version");
    if(!ext_raw_eq(&d,version,"1")||d.tokens[version].type!=XZ_JSON_PRIMITIVE) return -1;
    int t=xz_json_object_get(&d,0,edit->restoring?"keys":"values");
    if(t<0) return -1;
    if(!edit->restoring) {
        if(d.tokens[t].type!=XZ_JSON_OBJECT) return -1;
        char values[CFG_BYTES+1]; size_t size=d.tokens[t].end-d.tokens[t].start;
        if(size>CFG_BYTES) return -1;
        memcpy(values,raw+d.tokens[t].start,size); values[size]=0;
        if(ctl_map_parse(values,&edit->request)||!edit->request.count) return -1;
        for(size_t i=0;i<edit->request.count;i++) if(!ctl_active(&edit->defaults,edit->request.keys[i])) return -1;
    } else {
        if(d.tokens[t].type!=XZ_JSON_ARRAY) return -1;
        for(int k=d.tokens[t].first_child;k>=0;k=d.tokens[k].next_sibling) {
            char key[81],app[33];
            if(d.tokens[k].type!=XZ_JSON_STRING||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||
               !ctl_key(key,app)||ctl_add(&edit->request,key,"")) return -1;
        }
        if(!edit->request.count) return -1;
    }
    return 0;
}
static int ctl_request(struct ctl_edit *edit) {
    char raw[CFG_BYTES+2]; size_t n=fread(raw,1,sizeof(raw)-1,stdin); raw[n]=0;
    return ferror(stdin)?-1:ctl_request_bytes(edit,raw,n);
}
static int ctl_main(int argc,char **argv) {
    if(argc!=3) return message("invalid_ctl_request",2);
    int saving=!strcmp(argv[1],"ctl-save"),restoring=!strcmp(argv[1],"ctl-restore"),showing=!strcmp(argv[1],"ctl-show");
    if(!saving&&!restoring&&!showing) return message("invalid_ctl_request",2);
    const char *script=argv[2],*name=strrchr(script,'/'); name=name?name+1:script;
    if(!settings_name(name)) return message("invalid_ctl_request",2);
    struct ctl_edit *edit=calloc(1,sizeof(*edit));
    struct ctl_map *map=calloc(1,sizeof(*map)); struct cfg *effective=malloc(sizeof(*effective));
    int dir=-1,fd=-1,rc=2; char header[CFG_BYTES+1],record[CFG_BYTES+1]; SETTINGS_STAT st;
    if(!edit||!map||!effective) goto done;
    fd=open(script,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0||SETTINGS_FSTAT(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0022)) goto done;
    size_t n=0;
    while(n<CFG_BYTES) {
        ssize_t z=read(fd,header+n,CFG_BYTES-n);
        if(z<0&&errno==EINTR) continue;
        if(z<0) goto done;
        if(!z) break;
        n+=(size_t)z;
    }
    /* A schema claiming the reserved field must never make it public or
     * overwrite it. Read metadata/schema from the same checked descriptor,
     * even if a concurrent reupload replaces its directory entry. */
    int ext=ext_read(fd,&schema); close(fd); fd=-1;
    if(cfg_parse(header,n,&edit->defaults)) goto done;
    if(ext<0||(ext==1&&ext_field_index(&schema,CTL_RECORD_KEY)>=0)) goto done;
    edit->restoring=restoring;
    if(!showing&&ctl_request(edit)) goto done;
    dir=cfg_store_open(!showing);
    if(dir<0) {
        if(!showing||errno!=ENOENT) goto done;
        strcpy(record,"{}");
    } else if(showing&&cfg_store_read(dir,name,record)<0) goto done;
    if(showing) {
        if(ctl_stored_map(record,map)||ctl_effective(&edit->defaults,map,effective)||
           ctl_public(effective,map,edit->public_json)) goto done;
    } else {
        int result=cfg_store_transaction(dir,name,ctl_edit_record,edit);
        if(result<0) goto done;
        if(result==1) { rc=3; goto done; }
    }
    puts(edit->public_json); rc=0;
done:
    if(fd>=0) close(fd);
    if(dir>=0) close(dir);
    free(edit); free(map); free(effective);
    return rc==0?0:message(rc==3?"durability_uncertain":"invalid_or_unavailable_ctl_configuration",rc);
}

/* HOST ONLY. Include after the original helper and recognition-policy.inc.
 * The owner supplies a private fixture field; no product disk key is assigned.
 * Reuse its flat record, tokenizer, CTL overlay, compiler and store transaction. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition record is fixture-only; production placement pending"
#endif
typedef int (*rec_resolve_prompt)(void *,int,const char *,const char *,char [CFG_BYTES+1]);
struct rec_record {
    struct cfg cfg;
    struct decl_table table;
    struct rec_settings settings;
    struct rec_effective effective;
    char connections[2][CFG_BYTES+1], overrides[2][CFG_BYTES+1];
    char prompts[2][CFG_BYTES+1];
};
static void rec_record_close(struct rec_record *r) {
    if(!r) return;
    volatile unsigned char *p=(void *)r;
    for(size_t i=0;i<sizeof(*r);i++) p[i]=0;
    free(r);
}
#include "recognition-startup-schema.inc"
static int rec_json_bool(const xz_json_doc *d,int o,const char *key,int *out) {
    int t=xz_json_object_get(d,o,key);
    if(t<0) return 0; /* retain default */
    if(d->tokens[t].type!=XZ_JSON_PRIMITIVE) return -1;
    if(ext_raw_eq(d,t,"true")) *out=1;
    else if(ext_raw_eq(d,t,"false")) *out=0;
    else return -1;
    return 0;
}
/* Preserve the shared schema membership/duplicate checks. Empty names and
 * names containing its pipe separator cannot be literal schema members. */
static int rec_record_keys(const xz_json_doc *d,int t,const char *allowed) {
    if(t<0||d->tokens[t].type!=XZ_JSON_OBJECT)return 0;
    for(int k=d->tokens[t].first_child;k>=0;) {
        int v=d->tokens[k].next_sibling;char key[64];
        if(v<0||decode_string(d,k,key,sizeof(key))||!key[0]||strchr(key,'|'))return 0;
        k=d->tokens[v].next_sibling;
    }
    return ext_keys(d,t,allowed);
}
static int rec_decode_channel(const xz_json_doc *d,int token,int scope,
        struct rec_record *r,rec_resolve_prompt resolve,void *ctx) {
    struct rec_channel *ch=scope==DECL_OFFICIAL?&r->settings.official:&r->settings.local;
    if(token<0) return 0; /* other channel keeps its legacy initial value */
    if(!rec_record_keys(d,token,scope==DECL_LOCAL?
        "|mode||connection_id||prompt_override||wake_dispatch|":
        "|mode||connection_id||prompt_override|")) return -1;
    char mode[16],dispatch[16];
    if(!ext_text(d,token,"mode",mode,sizeof(mode),1)) return -1;
    if(!strcmp(mode,"off")) ch->mode=REC_OFF;
    else if(!strcmp(mode,"regex")) ch->mode=REC_REGEX;
    else if(!strcmp(mode,"model")) ch->mode=REC_MODEL;
    else return -1;
    if(scope==DECL_LOCAL) {
        /* Read old snapshots, but never treat this as a user mode switch.
         * New callers may omit it; effective mode determines dispatch. */
        if(!ext_text(d,token,"wake_dispatch",dispatch,sizeof(dispatch),0)) return -1;
        if(xz_json_object_get(d,token,"wake_dispatch")>=0) {
            if(!strcmp(dispatch,"listen")) ch->wake_dispatch=REC_LISTEN;
            else if(!strcmp(dispatch,"direct")) ch->wake_dispatch=REC_DIRECT;
            else return -1;
        }
    }
    if(!ext_text(d,token,"connection_id",r->connections[scope],sizeof(r->connections[scope]),0)||
       !ext_text(d,token,"prompt_override",r->overrides[scope],sizeof(r->overrides[scope]),0)) return -1;
    ch->connection_id=r->connections[scope];ch->prompt=r->prompts[scope];
    if(ch->mode!=REC_MODEL) return 0; /* retain archived data without resolving */
    int connection=rec_content(ch->connection_id),override=rec_content(r->overrides[scope]);
    if(connection<0||override<0||(!connection&&override)) return -1;
    if(!connection) return 0; /* both empty -> off, no model callback */
    memset(r->prompts[scope],0xff,sizeof(r->prompts[scope]));
    if(!resolve||resolve(ctx,scope,ch->connection_id,r->overrides[scope],r->prompts[scope])||
       !memchr(r->prompts[scope],0,sizeof(r->prompts[scope]))||
       rec_content(r->prompts[scope])!=1) return -1;
    return 0;
}
/* Whole-record load is read-only. out is untouched on error. cfg/table and
 * decoded strings have one owned lifetime; no borrowed tokenizer pointers.
 * defaults is the owner's already parsed script; app is explicit, never first.
 * resolve/ready are owner stubs, not a model connector or permission check here.
 * ready must review the SAME proposed policy/dependencies supplied via ctx. */
static int rec_record_open_schema_common(const struct ext_schema *extension,const struct cfg *defaults,const char *record,const char *field,
        const char *app,rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,
        struct rec_record **out,cfg_list_approve approve,void *source_context,cfg_startup_approve startup) {
    if(!defaults||!record||!field||!ext_ident(field)||!strcmp(field,CTL_RECORD_KEY)||
       (!extension||ext_field_index(extension,field)>=0)||!out||!flat_record(record))
        return CFG_INVALID;
    struct rec_record *r=calloc(1,sizeof(*r));
    struct ctl_map *map=calloc(1,sizeof(*map));
    char *policy=malloc(CFG_BYTES+1);int rc=CFG_CAPACITY;
    if(!r||!map||!policy) goto done;
    rc=CFG_INVALID;
    if(ctl_stored_map(record,map)||(startup?ctl_effective_startup(defaults,map,&r->cfg,approve,source_context,startup):
            approve?ctl_effective_rows(defaults,map,&r->cfg,approve,source_context):
            ctl_effective(defaults,map,&r->cfg))||
       (app?(approve?decl_compile_rows(&r->cfg,app,&r->table,approve,source_context):decl_compile(&r->cfg,app,&r->table)):
            (
#ifdef LX06_STARTUP_LIST_FIXTURE
            startup?decl_compile_entry_channels_startup(&r->cfg,&r->table,startup,source_context):
#else
            startup?CFG_INVALID:
#endif
            decl_compile_entry_channels(&r->cfg,&r->table)))) goto done;
    rec_defaults(&r->settings);
    /* Initialize BOTH independent channels from the same legacy effective cfg.
     * Presence of a partial channels object must not disable the other entry. */
    r->settings.official.mode=rec_content(cfg_value(&r->cfg,"小爱通道唤醒词"))?REC_REGEX:REC_OFF;
    r->settings.local.mode=rec_content(cfg_value(&r->cfg,"LX06-LOCAL-CHANNEL"))?REC_REGEX:REC_OFF;
    r->settings.local.wake_dispatch=r->settings.local.mode?REC_LISTEN:REC_DIRECT;
    r->settings.channels_present=1;
    xz_json_doc d;
    if(xz_json_parse(&d,record,strlen(record))||!ext_text(&d,0,field,policy,CFG_BYTES+1,0)) goto done;
    if(xz_json_object_get(&d,0,field)>=0) {
        if(!cfg_utf8_valid(policy,strlen(policy))||xz_json_parse(&d,policy,strlen(policy))||
           !rec_record_keys(&d,0,"|recognition||channels||model_understanding|")) goto done;
        int t=xz_json_object_get(&d,0,"recognition");
        if(t>=0&&(!rec_record_keys(&d,t,"|fixed_enabled||regex_enabled|")||
           rec_json_bool(&d,t,"fixed_enabled",&r->settings.fixed_enabled)||
           rec_json_bool(&d,t,"regex_enabled",&r->settings.regex_enabled))) goto done;
        t=xz_json_object_get(&d,0,"model_understanding");
        /* Connection contents are opaque to this slice; owner ready reviews
         * them as a complete chain. Do not accept a second model_enabled key. */
        if(t>=0&&(!rec_record_keys(&d,t,"|enabled||connections||intent_connections||extract_connections|")||
           rec_json_bool(&d,t,"enabled",&r->settings.model_enabled))) goto done;
        if(t>=0) {
            static const char *const arrays[]={"connections","intent_connections","extract_connections"};
            for(size_t i=0;i<sizeof(arrays)/sizeof(*arrays);i++) {
                int a=xz_json_object_get(&d,t,arrays[i]);
                if(a>=0&&d.tokens[a].type!=XZ_JSON_ARRAY) goto done;
            }
        }
        t=xz_json_object_get(&d,0,"channels");
        if(t>=0&&(!rec_record_keys(&d,t,"|official||local|")||
           rec_decode_channel(&d,xz_json_object_get(&d,t,"official"),DECL_OFFICIAL,r,resolve,ctx)||
           rec_decode_channel(&d,xz_json_object_get(&d,t,"local"),DECL_LOCAL,r,resolve,ctx))) goto done;
    }
    rc=startup?rec_validate_startup(&r->table,&r->settings,ready,ctx,&r->effective,approve,source_context,startup):
        approve?rec_validate_model_rows(&r->table,&r->settings,ready,ctx,&r->effective,approve,source_context):
        rec_validate(&r->table,&r->settings,ready,ctx,&r->effective);
    if(!rc&&rec_startup_schema(extension,r))rc=CFG_INVALID;
    if(!rc) { *out=r;r=NULL; }
done:
    rec_record_close(r);
    if(map) { memset(map,0,sizeof(*map));free(map); }
    if(policy) { memset(policy,0,CFG_BYTES+1);free(policy); }
    return rc;
}
static int rec_record_open_schema_rows_impl(const struct ext_schema *extension,const struct cfg *defaults,
        const char *record,const char *field,const char *app,rec_resolve_prompt resolve,rec_model_ready ready,
        void *ctx,struct rec_record **out,cfg_list_approve approve,void *source_context) {
    return rec_record_open_schema_common(extension,defaults,record,field,app,resolve,ready,ctx,out,approve,source_context,NULL);
}
static int rec_record_open_schema_impl(const struct ext_schema *extension,const struct cfg *defaults,
        const char *record,const char *field,const char *app,rec_resolve_prompt resolve,rec_model_ready ready,
        void *ctx,struct rec_record **out) {
    return rec_record_open_schema_rows_impl(extension,defaults,record,field,app,resolve,ready,ctx,out,NULL,NULL);
}
static int rec_record_open_impl(const struct cfg *defaults,const char *record,const char *field,
        const char *app,rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,
        struct rec_record **out) {
    return rec_record_open_schema_impl(&schema,defaults,record,field,app,resolve,ready,ctx,out);
}
/* Preserve the original explicit-App API. Entry recognition instead projects
 * script-wide channels without guessing an App or inventing control defaults. */
static int rec_record_open(const struct cfg *defaults,const char *record,const char *field,
        const char *app,rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,
        struct rec_record **out) {
    if(!app) return CFG_INVALID;
    return rec_record_open_impl(defaults,record,field,app,resolve,ready,ctx,out);
}
static int rec_record_open_entry(const struct cfg *defaults,const char *record,const char *field,
        rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,struct rec_record **out) {
    return rec_record_open_impl(defaults,record,field,NULL,resolve,ready,ctx,out);
}
/* One original flat-record field replacement, preserving unrelated tokens. */
static int rec_record_replace(const char *current,const char *field,const char *policy,
                              char next[CFG_BYTES+1]) {
    xz_json_doc d;struct ctl_buf b={next,0};int count=0;
    if(xz_json_parse(&d,current,strlen(current))) return CFG_INVALID;
    char old[CFG_BYTES+1];
    if(!ext_text(&d,0,field,old,sizeof(old),0)) return CFG_INVALID;
    if(xz_json_object_get(&d,0,field)>=0&&!strcmp(old,policy)) strcpy(next,current);
    else {
        if(ctl_put(&b,"{")) goto capacity;
        for(int k=d.tokens[0].first_child;k>=0;) {
            int v=d.tokens[k].next_sibling;
            if(!ext_raw_eq(&d,k,field)) {
                if((count++&&ctl_put(&b,","))||ctl_bytes(&b,current+d.tokens[k].start-1,
                    d.tokens[v].end-d.tokens[k].start+2)) goto capacity;
            }
            k=d.tokens[v].next_sibling;
        }
        if((count&&ctl_put(&b,","))||ctl_quote(&b,field)||ctl_put(&b,":")||
           ctl_quote(&b,policy)||ctl_put(&b,"}")) goto capacity;
    }
    return CFG_OK;
capacity:
    return CFG_CAPACITY;
}
/* Only explicit save normalizes an already-present legacy dispatch token.
 * Omitted derived metadata stays omitted. Keep all other policy bytes exact,
 * including the independent official mode and archived connection contents.
 * Input was already decoded/validated by rec_record_open. */
static int rec_record_normalize(const char *policy,int dispatch,char out[CFG_BYTES+1]) {
    xz_json_doc d;
    if(xz_json_parse(&d,policy,strlen(policy))) return CFG_INVALID;
    int channels=xz_json_object_get(&d,0,"channels");
    int local=channels<0?-1:xz_json_object_get(&d,channels,"local");
    int token=local<0?-1:xz_json_object_get(&d,local,"wake_dispatch");
    if(token<0) {strcpy(out,policy);return CFG_OK;}
    const char *value=dispatch==REC_DIRECT?"direct":"listen";
    if(ext_raw_eq(&d,token,value)) {strcpy(out,policy);return CFG_OK;}
    struct ctl_buf b={out,0};
    if(ctl_bytes(&b,policy,d.tokens[token].start)||ctl_put(&b,value)||
       ctl_put(&b,policy+d.tokens[token].end)) return CFG_CAPACITY;
    return CFG_OK;
}
/* Owner calls from the ORIGINAL cfg_store_guarded_locked edit callback.
 * Full proposed policy replaces only the explicit owner-selected fixture field.
 * Load never writes; explicit save normalizes archived local dispatch before
 * revalidating the final candidate. Canonical no-op keeps the whole record.
 * Whole encoded 8192B limit; no production save entry or binding transaction. */
static int rec_record_merge_impl(const struct cfg *defaults,const char *current,const char *field,
        const char *app,const char *policy,rec_resolve_prompt resolve,rec_model_ready ready,
        void *ctx,char out[CFG_BYTES+1]) {
    if(!current||!policy||!out||!field||!ext_ident(field)||!flat_record(current)) return CFG_INVALID;
    size_t n=strnlen(policy,CFG_BYTES+1);
    if(n>CFG_BYTES) return CFG_CAPACITY;
    if(!cfg_utf8_valid(policy,n)) return CFG_INVALID;
    char *next=malloc(CFG_BYTES+1),*normalized=malloc(CFG_BYTES+1);
    struct rec_record *r=NULL;int rc=CFG_CAPACITY;
    if(!next||!normalized) goto done;
    rc=rec_record_replace(current,field,policy,next);
    if(!rc) rc=rec_record_open_impl(defaults,next,field,app,resolve,ready,ctx,&r);
    if(rc) goto done;
    rc=rec_record_normalize(policy,r->effective.wake_dispatch,normalized);
    if(rc) goto done;
    if(strcmp(normalized,policy)) {
        rec_record_close(r);r=NULL;
        rc=rec_record_replace(current,field,normalized,next);
        if(!rc) rc=rec_record_open_impl(defaults,next,field,app,resolve,ready,ctx,&r);
    }
    if(!rc) memcpy(out,next,strlen(next)+1);
done:
    rec_record_close(r);
    if(next) {memset(next,0,CFG_BYTES+1);free(next);}
    if(normalized) {memset(normalized,0,CFG_BYTES+1);free(normalized);}
    return rc;
}
/* Keep the old explicit-App contract. Entry settings instead use script-wide
 * channels, including scripts with zero/multiple Apps; never pick a first App
 * or invent control/session defaults merely to edit an entry channel. */
static int rec_record_merge(const struct cfg *defaults,const char *current,const char *field,
        const char *app,const char *policy,rec_resolve_prompt resolve,rec_model_ready ready,
        void *ctx,char out[CFG_BYTES+1]) {
    if(!app) return CFG_INVALID;
    return rec_record_merge_impl(defaults,current,field,app,policy,resolve,ready,ctx,out);
}
static int rec_record_merge_entry(const struct cfg *defaults,const char *current,const char *field,
        const char *policy,rec_resolve_prompt resolve,rec_model_ready ready,
        void *ctx,char out[CFG_BYTES+1]) {
    return rec_record_merge_impl(defaults,current,field,NULL,policy,resolve,ready,ctx,out);
}

/* A-05/A-06: private composition, AFTER ctl-owner.inc/recognition-record.inc.
 * No new disk key, parser, lock, HTTP entry or runtime permission. The trusted
 * host selects the fixture field/App and supplies existing model callbacks. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition owner is fixture-only; production binding transaction pending"
#endif
static void rec_owner_wipe(void *memory,size_t size) {
    volatile unsigned char *p=memory;
    while(size--) *p++=0;
}
/* Raw token spans from the shipped tokenizer (string spans exclude quotes). */
static int rec_owner_token(struct ctl_buf *b,const xz_json_doc *d,int t) {
    size_t start=d->tokens[t].start,end=d->tokens[t].end;
    if(d->tokens[t].type==XZ_JSON_STRING) { start--;end++; }
    return ctl_bytes(b,d->source+start,end-start);
}
/* Replace one trusted path, at most channels/<scope>/<field>. Keep every
 * unrelated value's encoded bytes. This edits JSON DATA with the original
 * tokenizer; semantic validation remains rec_record_open/rec_validate. */
static int rec_owner_member(const char *raw,const char *const *path,size_t depth,
                            const char *value,char out[CFG_BYTES+1]) {
    if(!depth||depth>3) return -1;
    struct rec_member_work { xz_json_doc d;char child[CFG_BYTES+1],value[CFG_BYTES+1]; };
    struct rec_member_work *w=calloc(1,sizeof(*w));
    if(!w) return -1;
    int rc=-1,found=0,count=0;struct ctl_buf b={out,0};
    if(xz_json_parse(&w->d,raw,strlen(raw))||w->d.tokens[0].type!=XZ_JSON_OBJECT) goto done;
    if(depth>1) {
        int t=xz_json_object_get(&w->d,0,path[0]);
        if(t>=0) {
            if(w->d.tokens[t].type!=XZ_JSON_OBJECT) goto done;
            struct ctl_buf child={w->child,0};
            if(rec_owner_token(&child,&w->d,t)) goto done;
        } else strcpy(w->child,"{}");
        if(rec_owner_member(w->child,path+1,depth-1,value,w->value)) goto done;
        value=w->value;
    }
    if(ctl_put(&b,"{")) goto done;
    for(int k=w->d.tokens[0].first_child;k>=0;) {
        int v=w->d.tokens[k].next_sibling;
        if(v<0||(count++&&ctl_put(&b,","))||rec_owner_token(&b,&w->d,k)||ctl_put(&b,":")) goto done;
        if(ext_raw_eq(&w->d,k,path[0])) {
            if(found++||ctl_put(&b,value)) goto done;
        } else if(rec_owner_token(&b,&w->d,v)) goto done;
        k=w->d.tokens[v].next_sibling;
    }
    if(!found&&((count&&ctl_put(&b,","))||ctl_quote(&b,path[0])||ctl_put(&b,":")||ctl_put(&b,value))) goto done;
    if(ctl_put(&b,"}")) goto done;
    rc=0;
done:
    rec_owner_wipe(w,sizeof(*w));free(w);return rc;
}
/* Remove a selected override, never replace it with today's default. Channels
 * are atomic restore units: mode/connection/prompt/derived dispatch belong to
 * one declaration. Prune ONLY containers made empty by this removal. Missing
 * selections preserve the original bytes, including an absent outer field. */
static int rec_owner_remove(const char *raw,const char *const *path,size_t depth,
                            char out[CFG_BYTES+1]) {
    if(!depth||depth>3) return -1;
    struct rec_remove_work {xz_json_doc d;char child[CFG_BYTES+1],next[CFG_BYTES+1];};
    struct rec_remove_work *w=calloc(1,sizeof(*w));
    if(!w) return -1;
    int rc=-1,found=0,count=0;struct ctl_buf b={out,0};
    if(xz_json_parse(&w->d,raw,strlen(raw))||w->d.tokens[0].type!=XZ_JSON_OBJECT) goto done;
    int token=xz_json_object_get(&w->d,0,path[0]);
    if(token<0) {strcpy(out,raw);rc=0;goto done;}
    if(depth>1) {
        struct ctl_buf child={w->child,0};
        if(rec_owner_token(&child,&w->d,token)||
           rec_owner_remove(w->child,path+1,depth-1,w->next)) goto done;
        if(!strcmp(w->child,w->next)) {strcpy(out,raw);rc=0;goto done;}
    }
    if(ctl_put(&b,"{")) goto done;
    for(int k=w->d.tokens[0].first_child;k>=0;) {
        int v=w->d.tokens[k].next_sibling;
        if(v<0) goto done;
        int selected=ext_raw_eq(&w->d,k,path[0]);
        if(selected&&found++) goto done;
        if(!selected||(depth>1&&strcmp(w->next,"{}"))) {
            if((count++&&ctl_put(&b,","))||rec_owner_token(&b,&w->d,k)||ctl_put(&b,":")||
               (selected?ctl_put(&b,w->next):rec_owner_token(&b,&w->d,v))) goto done;
        }
        k=w->d.tokens[v].next_sibling;
    }
    if(ctl_put(&b,"}")) goto done;
    rc=0;
done:
    rec_owner_wipe(w,sizeof(*w));free(w);return rc;
}
/* A script-wide edit changes the recognition flags shared by ALL its Apps.
 * Check every App against the SAME retained effective cfg/settings. Keep the
 * entry result separate; neither the first nor the last App supplies dispatch.
 * Local declaration/compiler/policy checks only, not caps/session permission. */
static int rec_owner_apps(const struct rec_record *r,rec_model_ready ready,void *ctx) {
    if(!r->cfg.app_count) return 0;
    struct decl_table *table=calloc(1,sizeof(*table));
    if(!table) return -1;
    int rc=-1;
    for(size_t i=0;i<r->cfg.app_count;i++) {
        struct rec_effective app_effective;
        if(decl_compile(&r->cfg,r->cfg.apps[i],table)||
           rec_validate(table,&r->settings,ready,ctx,&app_effective)) goto done;
    }
    rc=0;
done:
    rec_owner_wipe(table,sizeof(*table));free(table);return rc;
}
static int rec_owner_project_effective(const struct cfg *defaults,const char *record,const char *field,
        const char *app,rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,
        char out[CFG_BYTES+1],struct rec_effective *effective) {
    struct rec_record *r=NULL;
    if(app?rec_record_open(defaults,record,field,app,resolve,ready,ctx,&r):
           rec_record_open_entry(defaults,record,field,resolve,ready,ctx,&r)) return -1;
    static const char *const modes[]={"off","regex","model"};
    struct ctl_buf b={out,0};int rc=-1;
    if(!app&&rec_owner_apps(r,ready,ctx)) goto done;
    if(ctl_put(&b,"{\"settings\":{\"fixed_enabled\":")||
       ctl_put(&b,r->settings.fixed_enabled?"true":"false")||
       ctl_put(&b,",\"regex_enabled\":")||ctl_put(&b,r->settings.regex_enabled?"true":"false")||
       ctl_put(&b,",\"model_enabled\":")||ctl_put(&b,r->settings.model_enabled?"true":"false")) goto done;
    for(int scope=0;scope<2;scope++) {
        struct rec_channel *ch=scope==DECL_OFFICIAL?&r->settings.official:&r->settings.local;
        if(ctl_put(&b,scope==DECL_OFFICIAL?",\"official\":{\"mode\":":",\"local\":{\"mode\":")||
           ctl_quote(&b,modes[ch->mode])||ctl_put(&b,",\"connection_id\":")||ctl_quote(&b,r->connections[scope])||
           ctl_put(&b,",\"prompt_override\":")||ctl_quote(&b,r->overrides[scope])||ctl_put(&b,"}")) goto done;
    }
    if(ctl_put(&b,"},\"effective\":{\"local\":")||ctl_quote(&b,modes[r->effective.local_mode])||
       ctl_put(&b,",\"official\":")||ctl_quote(&b,modes[r->effective.official_mode])||
       ctl_put(&b,",\"dispatch\":")||ctl_quote(&b,r->effective.wake_dispatch==REC_DIRECT?"direct":"listen")||
       ctl_put(&b,"}}")) goto done;
    if(effective) *effective=r->effective;
    rc=0;
done:
    rec_record_close(r);return rc;
}
static int rec_owner_project(const struct cfg *defaults,const char *record,const char *field,
        const char *app,rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,
        char out[CFG_BYTES+1]) {
    return rec_owner_project_effective(defaults,record,field,app,resolve,ready,ctx,out,NULL);
}
/* Optional synchronous dependency guard, borrowed until the original writer
 * returns (including uncertain durability). No second transaction or lock. */
struct rec_owner_dependency { int (*current)(void *);void *ctx; };
struct rec_owner_work {
    xz_json_doc patch,policy_doc;
    char policy[CFG_BYTES+1],next_policy[CFG_BYTES+1],value[CFG_BYTES+1];
    char view[CFG_BYTES+1];
    const char *field,*app,*changes;
    size_t length;
    const struct cfg *defaults;
    rec_resolve_prompt resolve;rec_model_ready ready;void *ctx;
    const struct rec_owner_dependency *dependency;
    struct rec_effective effective;
};
static int rec_owner_compose(const struct cfg *defaults,const char *record,char next[CFG_BYTES+1],void *ctx) {
    struct rec_owner_work *w=ctx;
    w->defaults=defaults;
    static const char *const keys[]={"fixed_enabled","regex_enabled","model_enabled",
        "official.mode","official.connection_id","official.prompt_override",
        "local.mode","local.connection_id","local.prompt_override"};
    if(!w->length||w->length>CFG_BYTES||memchr(w->changes,0,w->length)||
       !cfg_utf8_valid(w->changes,w->length)||xz_json_parse(&w->patch,w->changes,w->length)||
       !ext_keys(&w->patch,0,"|fixed_enabled||regex_enabled||model_enabled||official.mode|"
            "|official.connection_id||official.prompt_override||local.mode||local.connection_id||local.prompt_override||$restore|")) return -1;
    /* Transaction-only HOST envelope; never persisted as recognition policy.
     * Five explicit restore units, no wildcard, path syntax, null or defaults
     * supplied by the caller. A unit cannot be restored and edited together. */
    static const char *const units[]={"fixed_enabled","regex_enabled","model_enabled","official","local"};
    unsigned removed=0;int altered=0,edited=0;
    int restore=xz_json_object_get(&w->patch,0,"$restore");
    if(restore>=0) {
        if(w->patch.tokens[restore].type!=XZ_JSON_ARRAY||w->patch.tokens[restore].first_child<0) return -1;
        for(int t=w->patch.tokens[restore].first_child;t>=0;t=w->patch.tokens[t].next_sibling) {
            size_t i=0;
            if(w->patch.tokens[t].type!=XZ_JSON_STRING) return -1;
            while(i<5&&!ext_raw_eq(&w->patch,t,units[i])) i++;
            if(i==5||(removed&(1u<<i))) return -1;
            removed|=1u<<i;
        }
        for(size_t i=0;i<9;i++) {
            unsigned unit=i<3?(unsigned)i:i<6?3:4;
            if((removed&(1u<<unit))&&xz_json_object_get(&w->patch,0,keys[i])>=0) return -1;
        }
    }
    /* Validate shapes and strings before transforming anything. */
    for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++) {
        int t=xz_json_object_get(&w->patch,0,keys[i]);
        if(t<0) continue;
        if(i<3) {
            if(w->patch.tokens[t].type!=XZ_JSON_PRIMITIVE||
               (!ext_raw_eq(&w->patch,t,"true")&&!ext_raw_eq(&w->patch,t,"false"))) return -1;
        } else {
            if(!ext_text(&w->patch,0,keys[i],w->value,sizeof(w->value),0)) return -1;
            if((i==3||i==6)&&strcmp(w->value,"off")&&strcmp(w->value,"regex")&&strcmp(w->value,"model")) return -1;
        }
    }
    if(xz_json_parse(&w->policy_doc,record,strlen(record))||
       !ext_text(&w->policy_doc,0,w->field,w->policy,sizeof(w->policy),0)) return -1;
    if(xz_json_object_get(&w->policy_doc,0,w->field)<0) strcpy(w->policy,"{}");
    for(unsigned i=0;i<5;i++) if(removed&(1u<<i)) {
        const char *path[2]={i<2?"recognition":i==2?"model_understanding":"channels",
                            i==2?"enabled":units[i]};
        if(rec_owner_remove(w->policy,path,2,w->next_policy)) return -1;
        altered|=strcmp(w->policy,w->next_policy)!=0;
        strcpy(w->policy,w->next_policy);
    }
    /* Seed mode only when an absent channel is edited without its mode.
     * Use the SAME final CTL overrides; never infer it from a browser view. */
    for(int local=0;local<2;local++) {
        size_t base=local?6:3;
        if(xz_json_object_get(&w->patch,0,keys[base])>=0||
           (xz_json_object_get(&w->patch,0,keys[base+1])<0&&xz_json_object_get(&w->patch,0,keys[base+2])<0)) continue;
        if(xz_json_parse(&w->policy_doc,w->policy,strlen(w->policy))) return -1;
        int channels=xz_json_object_get(&w->policy_doc,0,"channels");
        if(channels>=0&&xz_json_object_get(&w->policy_doc,channels,local?"local":"official")>=0) continue;
        struct ctl_map *map=calloc(1,sizeof(*map));struct cfg *effective=calloc(1,sizeof(*effective));
        int rc=-1;
        if(map&&effective&&!ctl_stored_map(record,map)&&!ctl_effective(defaults,map,effective)) {
            const char *path[]={"channels",local?"local":"official","mode"};
            const char *table=cfg_value(effective,local?"LX06-LOCAL-CHANNEL":"小爱通道唤醒词");
            rc=rec_owner_member(w->policy,path,3,rec_content(table)?"\"regex\"":"\"off\"",w->next_policy);
        }
        if(map) {rec_owner_wipe(map,sizeof(*map));free(map);}
        if(effective) {rec_owner_wipe(effective,sizeof(*effective));free(effective);}
        if(rc) return -1;
        strcpy(w->policy,w->next_policy);
    }
    for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++) {
        int t=xz_json_object_get(&w->patch,0,keys[i]);
        if(t<0) continue;
        edited=1;
        struct ctl_buf value={w->value,0};
        if(rec_owner_token(&value,&w->patch,t)) return -1;
        const char *path[3];size_t depth;
        if(i<3) {path[0]=i==2?"model_understanding":"recognition";path[1]=i==2?"enabled":keys[i];depth=2;}
        else {path[0]="channels";path[1]=i<6?"official":"local";path[2]=strchr(keys[i],'.')+1;depth=3;}
        if(rec_owner_member(w->policy,path,depth,w->value,w->next_policy)) return -1;
        strcpy(w->policy,w->next_policy);
    }
    if(!edited&&!altered) strcpy(next,record);
    else if(removed&&!strcmp(w->policy,"{}")) {
        const char *path[]={w->field};
        if(rec_owner_remove(record,path,1,next)) return -1;
    } else if(w->patch.tokens[0].first_child<0) strcpy(next,record);
    else if(w->app?rec_record_merge(defaults,record,w->field,w->app,w->policy,w->resolve,w->ready,w->ctx,next):
                   rec_record_merge_entry(defaults,record,w->field,w->policy,w->resolve,w->ready,w->ctx,next)) return -1;
    strcpy(w->next_policy,next); /* retain the final candidate for the guard */
    return rec_owner_project_effective(defaults,next,w->field,w->app,w->resolve,w->ready,w->ctx,
        w->view,&w->effective);
}
static int rec_owner_guard(void *ctx) {
    struct rec_owner_work *w=ctx;
    if(w->dependency&&w->dependency->current(w->dependency->ctx)) return -1;
    if(rec_owner_project(w->defaults,w->next_policy,w->field,w->app,
        w->resolve,w->ready,w->ctx,w->value)||strcmp(w->view,w->value)) return -1;
    /* The target's resolver can invalidate a peer after its earlier check. */
    return w->dependency?w->dependency->current(w->dependency->ctx):0;
}
/* The existing loaded revision is private to the original form session. This
 * view projects that snapshot; it is NOT a fresh read or runtime grant. */
int rec_owner_loaded_view(const struct ctl_revision *revision,const char *field,const char *app,
        rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,char out[CFG_BYTES+1]) {
    if(!revision||!revision->record_ready||!out) return -1;
    struct cfg *defaults=calloc(1,sizeof(*defaults));char *view=calloc(CFG_BYTES+1,1);
    int rc=-1;
    if(defaults&&view&&!rts_defaults(revision,defaults,NULL)&&
       !rec_owner_project(defaults,revision->record,field,app,resolve,ready,ctx,view)) {
        strcpy(out,view);rc=0;
    }
    if(defaults) {rec_owner_wipe(defaults,sizeof(*defaults));free(defaults);}
    if(view) {rec_owner_wipe(view,CFG_BYTES+1);free(view);}return rc;
}
/* Atomic recognition dirty patch + optional CTL edit (NULL means absent).
 * Preview runs identical merge/audits/final guards under the original .lock,
 * then leaves disk and loaded revision unchanged. Outputs publish only on 0.
 * official.off exposes the required unbind to the host; real binding storage
 * integration is still pending, so this entry remains fixture-only.
 * app=NULL explicitly selects script-wide entry settings. The original owner
 * still validates every declared App/CTL in ctl_public before composition;
 * entry selection is not permission to skip full native CTL validation. */
static int rec_owner_apply_loaded_impl(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1],const struct rec_owner_dependency *dependency) {
    if(parent<0||!revision||!field||!ext_ident(field)||!strcmp(field,CTL_RECORD_KEY)||
       (app&&!ext_ident(app))||!changes||!rec_out||!ctl_out||(preview!=0&&preview!=1)||
       (dependency&&!dependency->current)) return -1;
    struct rec_owner_work *w=calloc(1,sizeof(*w));char *public=calloc(CFG_BYTES+1,1);
    int rc=-1;
    if(!w||!public) goto done;
    w->field=field;w->app=app;w->changes=changes;w->length=changes_length;
    w->resolve=resolve;w->ready=ready;w->ctx=model_ctx;
    w->dependency=dependency;
    struct ctl_owner_transform transform={rec_owner_compose,w,preview,rec_owner_guard};
    rc=ctl_owner_apply_composed_impl(scripts,name,revision,ctl,ctl_length,restoring,NULL,audit_ctx,
        gate,gate_ctx,NULL,NULL,audit,parent,entry,revision,&transform,public);
    if(!rc) {strcpy(ctl_out,public);strcpy(rec_out,w->view);}
done:
    if(w) {rec_owner_wipe(w,sizeof(*w));free(w);}
    if(public) {rec_owner_wipe(public,CFG_BYTES+1);free(public);}return rc;
}
/* Preserve the old entry and its exact call shape. Catalog-aware callers opt
 * into the same implementation with an additional borrowed dependency. */
int rec_owner_apply_loaded(int scripts,int parent,const char *entry,const char *name,
        struct ctl_revision *revision,const char *field,const char *app,
        const char *changes,size_t changes_length,const char *ctl,size_t ctl_length,int restoring,int preview,
        rec_resolve_prompt resolve,rec_model_ready ready,void *model_ctx,
        ctl_scoped_audit audit,void *audit_ctx,ctl_owner_gate gate,void *gate_ctx,
        char ctl_out[CFG_BYTES+1],char rec_out[CFG_BYTES+1]) {
    return rec_owner_apply_loaded_impl(scripts,parent,entry,name,revision,field,app,changes,changes_length,
        ctl,ctl_length,restoring,preview,resolve,ready,model_ctx,audit,audit_ctx,gate,gate_ctx,ctl_out,rec_out,NULL);
}

/* Included after declaration-expression.inc in its original private TU. */
#include "recognition-policy.h"
#ifdef LX06_RECOGNITION_MODEL_FIXTURE
#include "recognition-model.h"
#endif
static int rec_bool(int n) { return n==0 || n==1; }
static int rec_content(const char *s) {
    if(!s) return 0;
    size_t n=strnlen(s,CFG_BYTES+1);
    if(n>CFG_BYTES || !cfg_utf8_valid(s,n)) return -1;
    /* Unicode White_Space, not punctuation normalization. cfg_utf8_valid
     * above guarantees bounded complete codepoints, including at the end. */
    for(size_t i=0;i<n;) {
        unsigned c=(unsigned char)s[i++];
        if(c>=0xc0) {
            unsigned extra=c<0xe0?1:c<0xf0?2:3;
            c&=extra==1?0x1f:extra==2?0x0f:0x07;
            while(extra--) c=(c<<6)|((unsigned char)s[i++]&0x3f);
        }
        if(!((c>=9 && c<=13) || c==32 || c==0x85 || c==0xa0 || c==0x1680 ||
             (c>=0x2000 && c<=0x200a) || c==0x2028 || c==0x2029 ||
             c==0x202f || c==0x205f || c==0x3000)) return 1;
    }
    return 0;
}
void rec_defaults(struct rec_settings *s) {
    if(!s) return;
    memset(s,0,sizeof(*s)); s->fixed_enabled=1; s->regex_enabled=1;
}
/* Classification is the existing declaration compiler, never a new matcher.
 * Operators/quoted literals are regex even when they contain no slots. */
static int rec_inventory_startup(const struct decl_table *t, int scope,
                         unsigned char *kinds, unsigned *available,cfg_list_approve approve,void *source_context,cfg_startup_approve startup) {
    if(t->count[scope]>CFG_CANDIDATES) return CFG_CAPACITY;
    struct expr_program *p=malloc(sizeof(*p));
    if(!p) return CFG_CAPACITY;
    int rc=0; unsigned found=0;
    for(size_t i=0;i<t->count[scope];i++) {
        struct lx06_match_candidate c;
        rc=decl_candidate(t,scope,i,"",&c); if(rc) break;
        rc=lx06_expr_compile(c.phrase,strlen(c.phrase),p); if(rc) break;
        int lists=0;
        for(unsigned v=0;v<p->slots;v++)if(p->lists[v])lists=1;
        unsigned kind=(!p->syntax && !p->slots)?REC_FIXED_PATH:REC_REGEX_PATH;
        if(lists) {
            if(startup&&(scope==DECL_LOCAL||scope==DECL_OFFICIAL)) {
                struct expr_list_bounds bounds={0};const char *key=scope==DECL_LOCAL?"LX06-LOCAL-CHANNEL":"小爱通道唤醒词";
                if(c.line!=(int)i||startup(source_context,t->source,key,(unsigned)i,p,&bounds)){rc=CFG_INVALID;break;}
                kind=REC_REGEX_PATH;
            } else {
            if(!approve||scope!=DECL_CTL){rc=CFG_INVALID;break;}
            const char *raw=decl_ctl_row(t,i);struct cfg_row row;char key[81];
            int n=snprintf(key,sizeof(key),"LX06-%s-CTL",t->app);
            if(!raw||n<0||(size_t)n>=sizeof(key)){rc=CFG_INVALID;break;}
            for(char *q=key;*q;q++)if(*q>='a'&&*q<='z')*q=(char)(*q-'a'+'A');
            rc=cfg_parse_row_scoped(key,raw,&row,approve,source_context);if(rc)break;
            /* This composition has a typed MODEL consumer. Do not claim the
             * legacy deterministic matcher can execute a list-valued row. */
            kind=REC_MODEL_PATH;
            }
        }
        if(kinds) kinds[i]=(unsigned char)kind;
        found|=kind;
    }
    memset(p,0,sizeof(*p)); free(p);
    if(!rc) *available=found;
    return rc;
}
static int rec_inventory_impl(const struct decl_table *t,int scope,unsigned char *kinds,unsigned *available,
        cfg_list_approve approve,void *ctx) {return rec_inventory_startup(t,scope,kinds,available,approve,ctx,NULL);}
static int rec_inventory(const struct decl_table *t,int scope,unsigned char *kinds,unsigned *available) {
    return rec_inventory_impl(t,scope,kinds,available,NULL,NULL);
}
static int rec_ready(rec_model_ready ready, void *ctx, int scope,
                      const struct rec_channel *channel) {
    if(!ready) return 0;
    int rc=ready(ctx,scope,channel);
    return rc==0 || rc==1 ? rc : -1;
}
static int rec_validate_startup(const struct decl_table *t, const struct rec_settings *s,
                 rec_model_ready ready, void *ctx, struct rec_effective *out,cfg_list_approve approve,void *source_context,cfg_startup_approve startup) {
    if(!t || !t->source || !s || !out || !rec_bool(s->fixed_enabled) ||
       !rec_bool(s->regex_enabled) || !rec_bool(s->model_enabled) ||
       !rec_bool(s->channels_present) ||
       !(s->fixed_enabled || s->regex_enabled || s->model_enabled)) return CFG_INVALID;
    struct rec_effective next={0};
    struct rec_channel channels[2]={s->official,s->local};
    if(!s->channels_present) {
        int official=rec_content(cfg_value(t->source,"小爱通道唤醒词"));
        int local=rec_content(cfg_value(t->source,"LX06-LOCAL-CHANNEL"));
        if(official<0 || local<0) return CFG_INVALID;
        channels[0]=(struct rec_channel){.mode=official?REC_REGEX:REC_OFF};
        channels[1]=(struct rec_channel){.mode=local?REC_REGEX:REC_OFF,
                                       .wake_dispatch=local?REC_LISTEN:REC_DIRECT};
    }
    if(channels[1].wake_dispatch!=REC_DIRECT && channels[1].wake_dispatch!=REC_LISTEN)
        return CFG_INVALID;
    if(s->model_enabled && !s->fixed_enabled && !s->regex_enabled &&
       rec_ready(ready,ctx,-1,NULL)!=1) return CFG_INVALID;
    for(int scope=0;scope<DECL_SCOPES;scope++) {
        unsigned inventory=0;
        int rc=rec_inventory_startup(t,scope,NULL,&inventory,approve,source_context,startup); if(rc) return rc;
        unsigned paths=(s->fixed_enabled?REC_FIXED_PATH:0) |
                       (s->regex_enabled?REC_REGEX_PATH:0);
        paths&=inventory;
        if(scope==DECL_OFFICIAL || scope==DECL_LOCAL) {
            struct rec_channel *ch=&channels[scope];
            if(ch->mode<REC_OFF || ch->mode>REC_MODEL) return CFG_INVALID;
            int mode=ch->mode;
            if(mode==REC_REGEX && !inventory) mode=REC_OFF;
            if(mode==REC_MODEL) {
                int connection=rec_content(ch->connection_id), prompt=rec_content(ch->prompt);
                if(connection<0 || prompt<0 || connection!=prompt) return CFG_INVALID;
                if(!connection) mode=REC_OFF;
            }
            if(mode==REC_OFF) paths=0;
            else if(mode==REC_REGEX) {
                if(!paths) return CFG_INVALID;
            } else {
                if(!s->model_enabled || rec_ready(ready,ctx,scope,ch)!=1) return CFG_INVALID;
                paths=(paths&REC_FIXED_PATH)|REC_MODEL_PATH;
            }
            if(scope==DECL_OFFICIAL) next.official_mode=mode;
            else {
                next.local_mode=mode;
                next.wake_dispatch=mode==REC_OFF?REC_DIRECT:REC_LISTEN;
            }
        } else if(scope==DECL_CTL) {
            int model=s->model_enabled?rec_ready(ready,ctx,scope,NULL):0;
            if(model<0||((inventory&REC_MODEL_PATH)&&!model)) return CFG_INVALID;
            if(model) paths|=REC_MODEL_PATH;
            if(inventory && !paths) return CFG_INVALID;
        }
        /* PLAY is a declaration projection used by the channel mapping,
         * not a fifth independently enabled entry or a model intent scope.
         * A template-only PLAY projection must not invalidate only-fixed
         * channels whose explicit fixed connection words are available. */
        next.paths[scope]=paths;
    }
    *out=next; return CFG_OK;
}
static int rec_validate_impl(const struct decl_table *t,const struct rec_settings *s,rec_model_ready ready,
        void *ctx,struct rec_effective *out,cfg_list_approve approve,void *source_context) {
    return rec_validate_startup(t,s,ready,ctx,out,approve,source_context,NULL);
}
int rec_validate(const struct decl_table *t,const struct rec_settings *s,rec_model_ready ready,
        void *ctx,struct rec_effective *out) {
    return rec_validate_impl(t,s,ready,ctx,out,NULL,NULL);
}
static int rec_validate_model_rows(const struct decl_table *t,const struct rec_settings *s,rec_model_ready ready,
        void *ctx,struct rec_effective *out,cfg_list_approve approve,void *source_context) {
    return approve?rec_validate_impl(t,s,ready,ctx,out,approve,source_context):CFG_INVALID;
}
struct rec_startup_match_authority {const struct decl_table *table;int scope;cfg_startup_approve approve;void *ctx;};
static int rec_startup_match_approve(void *context,const struct lx06_match_candidate *candidate,
        const struct expr_program *program,struct expr_list_bounds *bounds) {
    struct rec_startup_match_authority *a=context;
    if(!a||!a->approve||candidate->line<0||
       (a->scope!=DECL_LOCAL&&a->scope!=DECL_OFFICIAL))return -1;
    const char *key=a->scope==DECL_LOCAL?"LX06-LOCAL-CHANNEL":"小爱通道唤醒词";
    return a->approve(a->ctx,a->table->source,key,(unsigned)candidate->line,program,bounds);
}
static int rec_match_startup_impl(const struct decl_table *t, const struct rec_settings *s,
              rec_model_ready ready, void *ctx, enum decl_scope scope,
              const char *script, const char *text, struct rec_result *out,
              rec_model_ready declared,cfg_list_approve approve,void *source_context,cfg_startup_approve startup) {
    (void)ready; /* Used by model opt-in when that fixture is enabled. */
    if(!out || !text || scope<0 || scope>=DECL_SCOPES) return CFG_INVALID;
    /* Keep the original matcher's text budget for ALL draft paths. The new
     * 8192B model envelope is not implemented by this recognition fixture. */
    size_t length=strnlen(text,513);
    if(length>512) return CFG_CAPACITY;
    if(!cfg_utf8_valid(text,length)) return CFG_INVALID;
    struct rec_effective effective;
    int rc=startup?rec_validate_startup(t,s,declared,ctx,&effective,approve,source_context,startup):
        approve?rec_validate_model_rows(t,s,declared,ctx,&effective,approve,source_context):
        rec_validate(t,s,declared,ctx,&effective); if(rc) return rc;
    struct rec_result next={.status=REC_DISABLED,.branch=-1,
                            .match={.index=-1}};
    unsigned paths=effective.paths[scope];
    if(!paths) { *out=next; return CFG_OK; }
    next.status=REC_UNRECOGNIZED;
    struct lx06_match_candidate *candidates=NULL;
    unsigned char kinds[CFG_CANDIDATES]; int indices[CFG_CANDIDATES], count=0;
    if(paths&(REC_FIXED_PATH|REC_REGEX_PATH)) {
        unsigned inventory=0;
        rc=rec_inventory_startup(t,scope,kinds,&inventory,approve,source_context,startup); if(rc) return rc;
        candidates=calloc(CFG_CANDIDATES,sizeof(*candidates));
        if(!candidates) return CFG_CAPACITY;
        for(size_t i=0;i<t->count[scope];i++) if((paths&(REC_FIXED_PATH|REC_REGEX_PATH))&kinds[i]) {
            rc=decl_candidate(t,scope,i,script,&candidates[count]); if(rc) break;
            indices[count++]=(int)i;
        }
        struct rec_startup_match_authority authority={t,scope,startup,source_context};
        (void)authority;
        if(!rc) rc=
#ifdef LX06_STARTUP_LIST_FIXTURE
            startup?lx06_expr_template_match_lists(text,candidates,count,&next.match,
            &next.branch,EXPR_MATCH_STEPS,rec_startup_match_approve,&authority):
#else
            startup?CFG_INVALID:
#endif
            lx06_expr_template_match(text,candidates,count,&next.match,&next.branch,EXPR_MATCH_STEPS);
        free(candidates); if(rc) return rc;
        if(next.match.index>=0) {
            if(next.match.index>=count) return CFG_INVALID;
            next.match.index=indices[next.match.index];
        }
    }
    if(next.match.kind==LX06_MATCH_EXACT || next.match.kind==LX06_MATCH_TEMPLATE)
        next.status=REC_DETERMINISTIC;
    else if(next.match.kind!=LX06_MATCH_NONE) next.status=REC_TERMINAL_REJECT;
    else if(paths&REC_MODEL_PATH) next.status=REC_MODEL_PENDING;
#ifdef LX06_RECOGNITION_MODEL_FIXTURE
    /* Explicit owner opt-in, after the ORIGINAL deterministic arbitration.
     * A model result is distinct from a matcher hit and cannot pass the old
     * delivery path without the same owner preparation/caps validation. */
    if(next.status==REC_MODEL_PENDING&&ready==rm_ready) {
        if(scope==DECL_CTL&&!t->count[DECL_CTL])return CFG_INVALID;
        const struct rec_channel *ch=scope==DECL_OFFICIAL?&s->official:
                                      scope==DECL_LOCAL?&s->local:NULL;
        int hit=rm_recognize(ctx,scope,script,text,ch);
        if(hit<0)return CFG_INVALID;
        next.status=hit?REC_MODEL_RESULT:REC_UNRECOGNIZED;
    }
#endif
    *out=next; return CFG_OK;
}
static int rec_match_impl(const struct decl_table *t,const struct rec_settings *s,rec_model_ready ready,
        void *ctx,enum decl_scope scope,const char *script,const char *text,struct rec_result *out,
        rec_model_ready declared,cfg_list_approve approve,void *source_context) {
    return rec_match_startup_impl(t,s,ready,ctx,scope,script,text,out,declared,approve,source_context,NULL);
}

int rec_match(const struct decl_table *t,const struct rec_settings *s,
        rec_model_ready ready,void *ctx,enum decl_scope scope,
        const char *script,const char *text,struct rec_result *out) {
    return rec_match_impl(t,s,ready,ctx,scope,script,text,out,ready,NULL,NULL);
}
#ifdef LX06_RECOGNITION_MODEL_FIXTURE
/* The original retained source owns this FINAL boundary. Its declaration
 * review remains mandatory before fixed/regex arbitration. Provider freshness
 * is renewed only for the selected model path after the original reader closes. */
static int rec_match_final(const struct decl_table *t,const struct rec_settings *s,
        rec_model_ready ready,void *ctx,enum decl_scope scope,
        const char *script,const char *text,struct rec_result *out) {
    return rec_match_impl(t,s,ready,ctx,scope,script,text,out,
        ready==rm_ready?rm_final_declared:ready,NULL,NULL);
}
#endif

#ifdef LX06_TYPED_CTL_WINDOW_FIXTURE
/* The same deterministic arbitration excludes typed MODEL-only inventory.
 * Only the explicit source owner may use this FINAL list composition. */
static int rec_match_rows_final(const struct decl_table *t,const struct rec_settings *s,
        rec_model_ready ready,void *ctx,const char *script,const char *text,struct rec_result *out,
        cfg_list_approve approve,void *source_context) {
    return ready==rm_ready&&approve?rec_match_impl(t,s,ready,ctx,DECL_CTL,script,text,out,
        rm_final_declared,approve,source_context):CFG_INVALID;
}
#endif

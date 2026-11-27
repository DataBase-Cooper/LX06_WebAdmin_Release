/* Normal native Web composition of the original settings/joint algorithms.
 * Complete cfg/snapshot/catalog and original settings -> wake commits remain.
 * See source-provenance.json for raw input pins and explicit promotion. */
#define _GNU_SOURCE
#define LX06_NATIVE_TARGET_SETTINGS 1
#define LX06_CTL_JSON_ALREADY_INCLUDED 1
#define LX06_APP_EXTENSION_JSON_SHARED 1
#include "app-target-settings.h"
#include "native-settings/settings-helper.inc"
#include "native-settings/declaration-expression.h"
#include "native-settings/recognition-policy.inc"
#include "native-settings/recognition-record.inc"
struct rec_catalog_scope;
static int rdn_catalog_audit(const char *,void *,struct rec_catalog_scope *);
#include "native-settings/uninstall-catalog-projection.generated.h"
#include "native-settings/recognition-target-view.inc"
#include "native-settings/ctl-owner.inc"
#include "native-settings/recognition-owner.inc"
static int ats_policy_field(const struct cfg *,const struct ext_schema *,char [33]);
#include "native-settings/recognition-catalog.inc"
#include "native-settings/ctl-selection.inc"
#include "native-settings/recognition-selection.inc"
#include "native-settings/recognition-declaration-review.inc"
#include "native-settings/recognition-target-view-api.inc"
#include "native-settings/target-view-load.generated.h"
#include "native-settings/target-preview.generated.h"
#include "native-settings/ctl-writer.inc"
#include "native-settings/joint-wake-readers.generated.h"
#include "native-settings/joint-unbind-original.generated.h"
#include "native-settings/ctl-wake-review.inc"
#include "native-settings/recognition-unbind.inc"
#include "native-settings/recognition-readback.inc"
#include "native-settings/target-joint.generated.h"
#include "native-settings/target-settings-joint-api.inc"

/* Actual production roots. Neither reads nor the runtime invent a store,
 * carrier, default script or metadata file. Existing Web/settings bootstrap
 * owns creation; absent/unsafe dependencies refuse this joint operation. */
#ifndef APP_TARGET_PERSIST
#define APP_TARGET_PERSIST "/data/lx06/apps"
#define APP_TARGET_PERSIST_PARENT "/data/lx06"
#define APP_TARGET_UPLOAD "/tmp/lx06-uploads"
#define APP_TARGET_UPLOAD_PARENT "/tmp"
#define APP_TARGET_META "/data/lx06"
#define APP_TARGET_META_PARENT "/data"
#define APP_TARGET_WAKE "/data/wakeword"
#define APP_TARGET_LOCKS "/tmp"
#endif
struct ats_roots {
    struct ctl_selection_root roots[3];
    struct ctl_store_root *ancestors[3];
};
static void ats_roots_close(struct ats_roots *r){
    for(unsigned i=0;i<3;i++){
        if(r->roots[i].dir>=0)close(r->roots[i].dir);
        if(r->roots[i].parent>=0)close(r->roots[i].parent);
        ctl_store_root_release(r->ancestors[i]);
    }
}
static int ats_roots_open(struct ats_roots *r,int scripts,const char *name){
    static const char *const paths[]={APP_TARGET_PERSIST,APP_TARGET_UPLOAD,APP_TARGET_META};
    static const char *const parents[]={APP_TARGET_PERSIST_PARENT,APP_TARGET_UPLOAD_PARENT,APP_TARGET_META_PARENT};
    memset(r,0,sizeof(*r));
    for(unsigned i=0;i<3;i++)r->roots[i].dir=r->roots[i].parent=-1;
    for(unsigned i=0;i<3;i++){
        const char *entry=strrchr(paths[i],'/');if(!entry||!entry[1])goto bad;
        r->roots[i].entry=entry+1;
        r->roots[i].parent=open(parents[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(r->roots[i].parent<0)goto bad;
        r->roots[i].dir=openat(r->roots[i].parent,entry+1,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(r->roots[i].dir<0&&(i==2||errno!=ENOENT))goto bad;
        int actual=r->roots[i].dir>=0?r->roots[i].dir:r->roots[i].parent;
        r->ancestors[i]=ctl_path_root_open(r->roots[i].dir>=0?paths[i]:parents[i],actual);
        if(!r->ancestors[i]||ctl_selection_root_current(&r->roots[i]))goto bad;
    }
    int selected=ctl_selection_target(r->roots,name);struct stat provided,chosen;
    if(selected<0||fstat(scripts,&provided)||fstat(r->roots[selected].dir,&chosen)||
       provided.st_dev!=chosen.st_dev||provided.st_ino!=chosen.st_ino)goto bad;
    return selected;
bad:ats_roots_close(r);return -1;
}
static int ats_source(int scripts,int fd,const char *name,const struct ctl_revision *revision){
    SETTINGS_STAT held,named,after;int fresh=-1,rc=-1;
    if(fd<0||!name||!settings_name(name)||SETTINGS_FSTAT(fd,&held)||!S_ISREG(held.st_mode)||
       held.st_uid!=geteuid()||held.st_nlink!=1||(held.st_mode&0022)||held.st_size<1||
       held.st_size>CTL_SOURCE_SCRIPT_CAP)return -1;
    fresh=ctl_script_open(scripts,name,&named);
    if(fresh<0||!ctl_same_stat(&held,&named))goto done;
    if(revision&&(!ctl_same_stat(&held,&revision->stat)||(size_t)held.st_size!=revision->size))goto done;
    unsigned char a[1024],b[1024];
    for(size_t at=0;at<(size_t)held.st_size;){
        size_t n=(size_t)held.st_size-at;if(n>sizeof a)n=sizeof a;
        if(ctl_pread_exact(fd,(char *)a,n,(off_t)at)||ctl_pread_exact(fresh,(char *)b,n,(off_t)at)||
           memcmp(a,b,n)||(revision&&memcmp(a,revision->bytes+at,n)))goto done;
        at+=n;
    }
    if(SETTINGS_FSTAT(fd,&after)||!ctl_same_stat(&held,&after)||SETTINGS_FSTAT(fresh,&after)||!ctl_same_stat(&held,&after))goto done;
    rc=0;
done:if(fresh>=0)close(fresh);return rc;
}
struct ats_call {
    int scripts,source;const char *name;struct ctl_revision *revision;
    int (*gate)(void *);void *ctx;
};
static int ats_gate(void *ctx){
    struct ats_call *c=ctx;
    return !c||!c->gate||c->gate(c->ctx)||ats_source(c->scripts,c->source,c->name,c->revision)?-1:0;
}
static int ats_policy_field(const struct cfg *cfg,const struct ext_schema *decl,char out[33]){
    const char *field=cfg_value(cfg,"LX06-RECOGNITION-FIELD");
    if(!field||strlen(field)>32||!ext_ident(field)||!strcmp(field,CTL_RECORD_KEY)||
       !strcmp(field,"lx06_extension_intent_v1")||ext_field_index(decl,field)>=0)return -1;
    strcpy(out,field);return 0;
}
static int ats_declaration(int fd,struct cfg *cfg,struct ext_schema *decl,char field[33]){
    SETTINGS_STAT st,after;
    if(fd<0||SETTINGS_FSTAT(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||
       (st.st_mode&0022)||st.st_size<1||st.st_size>CTL_SOURCE_SCRIPT_CAP)return -1;
    char *source=calloc((size_t)st.st_size+1,1);if(!source)return -1;
    int rc=-1;size_t extent=0;struct cfg_metadata metadata;
    if(ctl_pread_exact(fd,source,(size_t)st.st_size,0)||SETTINGS_FSTAT(fd,&after)||!ctl_same_stat(&st,&after)||
       cfg_parse_script(source,(size_t)st.st_size,cfg,&extent)||rts_extensions(source,extent,decl)!=1||
       cfg_metadata_view(cfg,&metadata)||metadata.mode!=CFG_MODE_PHONE_BRIDGE||!metadata.bridge_enabled||
       !metadata.protocol_declared||ats_policy_field(cfg,decl,field))goto done;
    static const char *const keys[]={"host","port","ca_file","spki","token","phone_allowlist","speaker_allowlist"};
    for(unsigned i=0;i<7;i++){
        int index=ext_field_index(decl,keys[i]);
        if(index<0||(!strcmp(keys[i],"token")?strcmp(decl->fields[index].type,"password"):
            !strcmp(decl->fields[index].type,"password")))goto done;
    }
    /* The existing action/schema parser remains the sole action declaration.
     * At least one original action must explicitly include all seven fields. */
    for(int a=0;a<decl->na;a++){
        int fields=xz_json_object_get(&decl->doc,decl->actions[a].token,"fields"),found=0;
        for(int t=fields<0?-1:decl->doc.tokens[fields].first_child;t>=0;t=decl->doc.tokens[t].next_sibling)
            for(unsigned i=0;i<7;i++)if(ext_raw_eq(&decl->doc,t,keys[i]))found++;
        if(found==7){rc=0;break;}
    }
done:rtv_wipe(source,(size_t)st.st_size+1);free(source);return rc;
}
int app_target_settings_supported(int source_fd){
    struct cfg *cfg=calloc(1,sizeof(*cfg));struct ext_schema *decl=calloc(1,sizeof(*decl));char field[33];
    int supported=cfg&&decl&&!ats_declaration(source_fd,cfg,decl,field);
    if(cfg){rtv_wipe(cfg,sizeof(*cfg));free(cfg);}if(decl){rtv_wipe(decl,sizeof(*decl));free(decl);}return supported;
}
/* Connection-only edits cannot enable a model owner. Existing model channels
 * require their real production owner; an unavailable one refuses, never
 * masquerades as ready or falls back to regex. No inference occurs here. */
static int ats_model_ready(void *ctx,int scope,const struct rec_channel *channel){
    (void)ctx;(void)scope;(void)channel;return 0;
}
static int ats_model_prompt(void *ctx,int scope,const char *connection,const char *override,char out[CFG_BYTES+1]){
    (void)ctx;(void)scope;(void)connection;(void)override;(void)out;return -1;
}
#define ATS_AUDIT_PHRASES (2u*CTL_CATALOG_COUNT*CFG_CANDIDATES)
#define ATS_AUDIT_BYTES (2u*1024u*1024u)
struct ats_audit {
    size_t projections,used,count,pairs,protected_checks;const char *target;
    struct {unsigned phrase,script,scope;} entries[ATS_AUDIT_PHRASES];
    char arena[ATS_AUDIT_BYTES];
};
int cfg_static_phrase_hit(const char *,const char *);
static int ats_audit_phrase(struct ats_audit *a,const char *script,unsigned scope,const char *phrase){
    /* Device-side symbolic intersection and expression pronunciation review
     * are not yet available. Classify with the SAME original compiler and
     * refuse every expression/slot/list, including quoted fixed text. Never
     * pass its source spelling to the exact-only audit as a negative proof.
     * Plain legacy literal sequences keep their original exact checks below. */
    struct expr_program *program=calloc(1,sizeof(*program));if(!program)return -1;
    int limited=lx06_expr_compile(phrase,strlen(phrase),program)||program->syntax||
        program->slots||program->branches!=1;
    for(unsigned i=0;!limited&&i<program->count;i++)
        if(program->nodes[i].kind!=EXPR_LITERAL&&program->nodes[i].kind!=EXPR_SEQUENCE)limited=1;
    rtv_wipe(program,sizeof(*program));free(program);if(limited)return -1;
    struct wake_review_source *wake=rec_unbind_active?rec_unbind_active->wake:NULL;
    if(!wake||wake_review_current(wake))return -1;
    if(wake->official_enabled){
        if(++a->protected_checks>4096||cfg_static_phrase_hit(phrase,"小爱同学")!=0)return -1;
    }
    for(size_t i=0;i<wake->count;i++){
        if(++a->protected_checks>4096||cfg_static_phrase_hit(phrase,wake->words[i])!=0)return -1;
    }
    if(scope!=DECL_OFFICIAL&&scope!=DECL_LOCAL)return 0;
    char normalized[EXPR_BYTES+1];
    if(!lx06_phrase_normalize(phrase,normalized,sizeof normalized))return -1;
    for(size_t i=0;i<a->count;i++){
        if(a->entries[i].scope!=scope)continue;
        if(++a->pairs>250000)return -1;
        if(!strcmp(a->arena+a->entries[i].phrase,normalized)){
            if(strcmp(a->arena+a->entries[i].script,script))return -1;
            return 0; /* same script-wide channel repeated in another App */
        }
    }
    size_t name=strlen(script)+1,bytes=strlen(normalized)+1;
    if(a->count==ATS_AUDIT_PHRASES||name+bytes>sizeof a->arena-a->used)return -1;
    a->entries[a->count].scope=scope;
    a->entries[a->count].script=(unsigned)a->used;memcpy(a->arena+a->used,script,name);a->used+=name;
    a->entries[a->count].phrase=(unsigned)a->used;memcpy(a->arena+a->used,normalized,bytes);a->used+=bytes;
    a->count++;return 0;
}
static int ats_audit_session(const char *script,const char *app,enum cfg_session_kind *kind,void *ctx){
    (void)ctx;if(!script||!app||recognition_declaration_current())return -1;
    /* This is a static command settings audit. Actual session/audio permission
     * remains with the later authenticated caps/operation, never inferred here. */
    *kind=CFG_SESSION_COMMAND;return 0;
}
static int ats_audit_consume(const char *script,const struct cfg *cfg,const struct decl_table *table,
        const char *record,size_t bytes,void *ctx){
    struct ats_audit *audit=ctx;
    if(!script||!cfg||!table||table->source!=cfg||!record||bytes!=strlen(record)||bytes>CFG_BYTES||
       !flat_record(record)||recognition_declaration_current())return -1;
    /* Compile/validate every retained declaration from both real directories.
     * No browser-reported row set or synthetic empty peer catalog is accepted. */
    for(unsigned scope=0;scope<DECL_SCOPES;scope++)for(size_t i=0;i<table->count[scope];i++){
        struct lx06_match_candidate candidate;
        if(decl_candidate(table,(enum decl_scope)scope,i,script,&candidate)||
           ats_audit_phrase(audit,script,scope,candidate.phrase))return -1;
    }
    audit->projections++;return recognition_declaration_current();
}
static int ats_audit_finish(void *ctx){
    struct ats_audit *audit=ctx;return !audit->projections||recognition_declaration_current()?-1:0;
}
static int ats_audit(const char *public,void *scope,void *ctx){
    struct ats_audit *audit=calloc(1,sizeof(*audit));struct rdn_summary summary;
    if(!audit)return -1;audit->target=ctx;
    int rc=-1;
    if(public&&scope&&audit->target)rc=recognition_declaration_review(scope,audit->target,
        ats_audit_session,ats_audit_consume,ats_audit_finish,audit,&summary);
    rtv_wipe(audit,sizeof(*audit));free(audit);return rc;
}
static int ats_public_values(const struct ctl_revision *revision,char out[CFG_BYTES+1]){
    if(!revision||!revision->record||!revision->record_ready)return -1;
    xz_json_doc doc;if(xz_json_parse(&doc,revision->record,strlen(revision->record)))return -1;
    struct ctl_buf b={out,0};int count=0;if(ctl_put(&b,"{"))return -1;
    for(int i=0;i<schema.nf;i++){
        if(!strcmp(schema.fields[i].type,"password"))continue;
        int token=xz_json_object_get(&doc,0,schema.fields[i].name);if(token<0)continue;
        char value[2049];if(!ext_text(&doc,0,schema.fields[i].name,value,sizeof value,0)||
           !ext_value(&schema,i,value)||(count++&&ctl_put(&b,","))||
           ctl_quote(&b,schema.fields[i].name)||ctl_put(&b,":")||ctl_quote(&b,value))return -1;
    }
    return ctl_put(&b,"}");
}
int app_target_settings_load(int scripts,int source_fd,const char *name,
        int (*gate)(void *),void *ctx,struct rtv_snapshot **loaded,char public_values[8193]){
    if(!loaded||*loaded||!public_values||!gate||ats_source(scripts,source_fd,name,NULL))return -1;
    struct cfg *cfg=calloc(1,sizeof(*cfg));struct ext_schema *decl=calloc(1,sizeof(*decl));
    struct ats_roots roots;char field[33],values[CFG_BYTES+1];struct rtv_snapshot *snapshot=NULL;
    int selected=-1,rc=-1;
    if(!cfg||!decl||ats_declaration(source_fd,cfg,decl,field)||(selected=ats_roots_open(&roots,scripts,name))<0)goto done;
    struct ats_call call={scripts,source_fd,name,NULL,gate,ctx};struct rec_readback_observation observation;
    struct rec_selection_scope selection={roots.roots,roots.ancestors,NULL};
    const struct rec_catalog_provider provider={rec_selection_acquire,rec_selection_current,rec_selection_scripts,rec_selection_release,&selection};
    char ctl[CFG_BYTES+1],recognition[CFG_BYTES+1];
    rc=recognition_target_joint_read(scripts,roots.roots[selected].parent,roots.roots[selected].entry,name,
        roots.ancestors[selected],field,ats_model_prompt,ats_model_ready,NULL,ats_audit,(void *)name,
        ats_gate,&call,APP_TARGET_WAKE,APP_TARGET_LOCKS,&provider,&snapshot,ctl,recognition,&observation,GSV_BYTES);
    if(!rc){
        call.revision=snapshot->revision;
        if(ats_gate(&call)||ats_public_values(snapshot->revision,values))rc=-1;
    }
    if(!rc){strcpy(public_values,values);*loaded=snapshot;snapshot=NULL;}
    ats_roots_close(&roots);
done:
    recognition_target_view_close(snapshot);
    if(cfg){rtv_wipe(cfg,sizeof(*cfg));free(cfg);}if(decl){rtv_wipe(decl,sizeof(*decl));free(decl);}
    return rc;
}
int app_target_settings_save_connection(int scripts,int source_fd,const char *name,
        struct rtv_snapshot *loaded,const char *connection,size_t length,
        int (*gate)(void *),void *ctx,struct rtv_snapshot **saved,struct rec_unbind_receipt *receipt){
    struct ctl_revision *revision=loaded?loaded->revision:NULL;
    int rc=-1,selected=-1;
    if(!revision||!revision->record_ready)return -1;
    struct cfg *cfg=calloc(1,sizeof(*cfg));struct ext_schema *decl=calloc(1,sizeof(*decl));
    struct ats_roots roots;char field[33],ctl[CFG_BYTES+1],recognition[CFG_BYTES+1];
    if(!saved||*saved||!receipt||!gate||!connection||!length||length>CFG_BYTES||
       strnlen(connection,length+1)!=length||ats_source(scripts,source_fd,name,revision)||
       !cfg||!decl||ats_declaration(source_fd,cfg,decl,field)||(selected=ats_roots_open(&roots,scripts,name))<0)goto done;
    struct ats_call call={scripts,source_fd,name,revision,gate,ctx};
    rc=recognition_target_joint_apply_values(scripts,roots.roots[selected].parent,roots.roots[selected].entry,
        name,loaded,field,NULL,"{}",2,NULL,0,0,0,ats_model_prompt,ats_model_ready,NULL,
        ats_audit,(void *)name,ats_gate,&call,ctl,recognition,roots.roots,roots.ancestors,GSV_BYTES,saved,
        APP_TARGET_WAKE,APP_TARGET_LOCKS,receipt,connection);
    if(!rc){
        /* SAME saved original revision, moved once to the returned complete
         * snapshot. There is no second settings read or reconstructed owner. */
        (*saved)->revision=revision;loaded->revision=NULL;
    }
    ats_roots_close(&roots);
done:
    revision->record_ready=0;if(rc)revision->profile_ready=0;
    if(cfg){rtv_wipe(cfg,sizeof(*cfg));free(cfg);}if(decl){rtv_wipe(decl,sizeof(*decl));free(decl);}
    return rc;
}

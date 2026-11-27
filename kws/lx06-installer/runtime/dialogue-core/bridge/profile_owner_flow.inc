/* Included inside the existing opaque owner: borrow its real profile/session,
 * never cast const getters or instantiate a second transport/session owner. */
static int owner_flow_current(void *context,const struct eo_origin *origin) {
    br_profile_flow *f=context;br_profile_owner *o=f?f->owner:NULL;
    return o&&o->flow==f&&f->source&&f->source->owner&&
        eo_saved_current(f->source,origin)==1&&owner_current(o)&&f->current&&
        f->current(f->context,origin)==1&&owner_current(o)&&eo_saved_current(f->source,origin)==1;
}
static void owner_flow_event(void *context,const br_session *s,uint64_t now,int event) {
    br_profile_flow *f=context;
    if(f&&f->owner&&s==&f->owner->session&&f->run.held&&f->adapter.active)
        (void)br_object_flow_event(&f->run,f->adapter.run,f->adapter.ticket,now,event);
}
static void owner_flow_observe(void *context,uint64_t run,uint64_t ticket,const br_operation *op,int event) {
    /* No extra transition: original br_flow_session delivers to br_flow. */
    (void)context;(void)run;(void)ticket;(void)op;(void)event;
}
static int owner_flow_send(void *context,uint64_t run,uint64_t session_run,uint64_t ticket,
    uint64_t epoch,uint64_t generation,const void *wire,size_t n) {
    br_profile_flow *f=context;br_profile_owner *o=f?f->owner:NULL;
    if(!o||o->flow!=f||!f->run.held||run!=f->adapter.run||ticket!=f->adapter.ticket||
       generation!=f->adapter.generation||!owner_identity(o,session_run,epoch)||
       !owner_flow_current(f,&f->run.origin)||
       !selected_script_flow_held(o->source,f->source->owner,&f->run.origin,session_run,epoch,run,&f->run))return -1;
    return br_profile_submit(o->profile,session_run,epoch,wire,n);
}
int br_profile_owner_flow_init(br_profile_flow *f,br_profile_owner *o,struct eo_saved_source *source) {
    if(!f||f->owner||!o||o->flow||!source||!source->owner||!o->session.live||o->session.owned||
       !owner_current(o)||eo_saved_current(source,&source->owner->origin)!=1)return -1;
    memset(f,0,sizeof(*f));f->owner=o;f->source=source;
    int rc=br_flow_session_init(&f->adapter,&f->flow,&o->session,f->wire,sizeof f->wire,
        (br_flow_session_io){f,owner_flow_send,owner_flow_observe});
    if(rc==1)o->flow=f;else f->owner=NULL;return rc;
}
int br_profile_owner_flow_start(br_profile_flow *f,br_object_flow *object,const char *name,
    br_flow_session_log *log,br_flow_owner_store *store,uint64_t now,br_object_source_current current,void *context,
    br_flow_owner_handoff_current previous,void *previous_context) {
    br_profile_owner *o=f?f->owner:NULL;
    if(!o||o->flow!=f||f->run.held||f->adapter.bound||!f->source||!current||!log||!store||!object||
       !owner_identity(o,log->binding.session_run,log->binding.epoch)||
       strcmp(log->binding.script,o->script)||f->adapter.session!=&o->session||
       strcmp(o->wake_source,log->binding.source==BR_LOG_LOCAL?"local":
           log->binding.source==BR_LOG_OFFICIAL?"official":"")||!owner_current(o))return -1;
    f->current=current;f->context=context;
    if(!owner_flow_current(f,&f->source->owner->origin)||
       br_profile_events(o->profile,o->run,o->epoch,owner_flow_event,f)!=1)return -1;
    int rc=br_object_flow_start_prepared_selected(&f->run,object,f->source->owner,&f->source->owner->origin,
        name,&f->adapter,log,store,now,owner_flow_current,f,previous,previous_context,o->source);
    if(rc!=1&&!f->run.held)br_profile_owner_close(o,o->run,o->epoch);
    return rc;
}
int br_profile_owner_flow_dispatch(br_profile_flow *f,uint64_t run,uint64_t ticket,uint64_t now,const char *command) {
    return f&&f->owner&&f->owner->flow==f?br_object_flow_dispatch(&f->run,run,ticket,now,command):-1;
}
int br_profile_owner_flow_poll(br_profile_flow *f,uint64_t run,uint64_t now,int preempt) {
    if(!f||!f->owner||f->owner->flow!=f||run!=f->adapter.run||(preempt!=0&&preempt!=1))return -1;
    if(!owner_flow_current(f,&f->run.origin)){br_profile_owner_close(f->owner,f->owner->run,f->owner->epoch);preempt=1;}
    return br_object_flow_poll(&f->run,run,now,preempt);
}
int br_profile_owner_flow_result(br_profile_flow *f,br_inbox *inbox,uint64_t wall,int trusted,
    br_result *identity,br_result_progress *progress) {
    return f&&f->owner&&f->owner->flow==f?br_object_flow_result(&f->run,inbox,wall,trusted,identity,progress):-1;
}
int br_profile_owner_flow_retire(br_profile_flow *f,br_flow_session_log_closed closed,void *context) {
    if(!f||!f->owner||f->owner->flow!=f)return -1;
    int rc=br_object_flow_retire(&f->run,closed,context);
    if(rc==1){
        /* Drop both ORIGINAL profile borrows (source guard and event context)
         * before permitting the caller to destroy this completed handle. */
        br_profile_owner *o=f->owner;br_profile_owner_close(o,o->run,o->epoch);
    }
    return rc;
}

int recognition_target_view_load(int scripts,int parent,const char *entry,const char *name,
                   struct ctl_store_root *root,
                   ctl_owner_gate gate,void *gate_ctx,size_t budget,struct rtv_snapshot **out) {
    if(!out||*out||!name||!settings_name(name)||
       parent<0||!root||!gate||budget>GSV_BYTES||!ctl_script_dir(scripts)) return -1;
    if(gate&&gate(gate_ctx)!=0) return -4;
    if(parent>=0&&ctl_directory_current(scripts,parent,entry)) return -2;
    if(root&&ctl_store_root_current(root,scripts)) return -2;
    struct ctl_owner_edit *o=calloc(1,sizeof(*o));
    if(!o) return -1;
    struct rtv_snapshot *view=calloc(1,sizeof(*view));
    if(!view) {free(o);return -1;}
    view->budget=budget;o->full_view=view;
    o->scripts=scripts; o->scripts_parent=parent; o->scripts_entry=entry;
    o->gate=gate; o->gate_ctx=gate_ctx;
    o->scripts_root=root; /* borrowed through synchronous load */
    o->name=name; o->store=cfg_store_open(0);
    o->store_root=o->store<0?NULL:ctl_store_root_open(o->store);
    int lock=!o->store_root?-1:openat(o->store,".lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    o->lock=lock;
    int rc=lock<0?-1:cfg_store_guarded_locked(o->store,lock,name,ctl_owner_load_record,
                                                          gate?ctl_owner_load_guard:ctl_owner_guard,o);
    if(!rc) {view->revision=(struct ctl_revision *)o->revision;*out=view;}
    else {ctl_owner_release((struct ctl_revision *)o->revision);
        recognition_target_view_close(view);}
    if(rc&&gate&&o->refusal) rc=o->refusal;
    if(lock>=0) close(lock);
    if(o->store>=0) close(o->store);
    ctl_store_root_release(o->store_root);
    free(o); return rc;
}
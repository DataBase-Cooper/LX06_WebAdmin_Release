/* SAME original effective table and policy used for load and explicit save.
 * A declaration cannot create an entry, infer a row, or enable a disabled B. */
static int rec_startup_list_program(const struct ext_schema *schema,const struct cfg *cfg,
        const char *key,unsigned row,const struct expr_program *program,struct expr_list_bounds *out) {
    if(!schema||!cfg||!key||!program||!out||!ext_startup_valid((struct ext_schema *)schema))return -1;
    const char *scope=!strcmp(key,"LX06-LOCAL-CHANNEL")?"local":!strcmp(key,"小爱通道唤醒词")?"official":NULL;
    if(!scope)return -1;
    const char *enabled=cfg_value(cfg,!strcmp(scope,"local")?"LX06-LOCALWAKEUP":"LX06-OFFICIALWAKEUP");
    if(!enabled||strcmp(enabled,"on"))return -1;
    const xz_json_doc *d=&schema->doc;int bindings=xz_json_object_get(d,0,"startup_bindings");
    int lists=xz_json_object_get(d,0,"list_slots");if(bindings<0||lists<0)return -1;
    struct expr_list_bounds bounds={0};
    for(unsigned b=0;b<program->branches;b++) {
        struct expr_branch branch;if(lx06_expr_branch(program,b,&branch))return -1;
        if(!branch.list_mask)continue;
        int found=-1;
        for(int t=d->tokens[bindings].first_child;t>=0;t=d->tokens[t].next_sibling) {
            char sc[16];int r,k;
            if(!ext_text(d,t,"scope",sc,sizeof(sc),1)||xz_json_get_int(d,t,"row",&r)||xz_json_get_int(d,t,"branch",&k))return -1;
            if(!strcmp(sc,scope)&&r==(int)row&&k==(int)b){if(found>=0)return -1;found=t;}
        }
        if(found<0)return -1;
        char id[65];if(!ext_text(d,found,"intent",id,sizeof(id),1))return -1;
        int intent=ext_startup_intent(d,id),slots=xz_json_object_get(d,found,"slots");
        int fields=xz_json_object_get(d,intent,"fields");unsigned seen=0,seen_lists=0;
        for(int k=d->tokens[slots].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;char field[17],capture[17];
            if(!ext_model_string(d,k,field,sizeof(field))||!ext_model_string(d,v,capture,sizeof(capture)))return -1;
            unsigned s;for(s=0;s<program->slots;s++)if(!strcmp(capture,program->names[s]))break;
            if(s==program->slots||!(branch.slot_mask&(1U<<s))||(seen&(1U<<s)))return -1;
            seen|=1U<<s;
            if(program->lists[s]) {
                char type[16],binding[33];int decl=xz_json_object_get(d,fields,field),list=xz_json_object_get(d,lists,field);
                int min=1,max=64;
                if(!ext_text(d,decl,"type",type,sizeof(type),1)||strcmp(type,"array")||list<0||
                   !ext_text(d,list,"binding",binding,sizeof(binding),1)||ext_registry_find(schema,"default_bindings",binding)<0)return -1;
                (void)xz_json_get_int(d,list,"minChars",&min);(void)xz_json_get_int(d,list,"maxChars",&max);
                if(min<1||max<min||max>256||(bounds.mask&(1U<<s) &&
                   (bounds.min[s]!=min||bounds.max[s]!=max)))return -1;
                bounds.mask|=1U<<s;bounds.min[s]=(unsigned short)min;bounds.max[s]=(unsigned short)max;seen_lists|=1U<<s;
            }
            k=d->tokens[v].next_sibling;
        }
        if(seen!=branch.slot_mask||seen_lists!=branch.list_mask)return -1;
    }
    unsigned expected=0;for(unsigned s=0;s<program->slots;s++)if(program->lists[s])expected|=1U<<s;
    if(!expected||bounds.mask!=expected)return -1;
    *out=bounds;return 0;
}
static int rec_startup_schema(const struct ext_schema *s,const struct rec_record *r) {
    const xz_json_doc *d=&s->doc;int list=xz_json_object_get(d,0,"startup_bindings");
    if(list<0)return 0;
    if(!ext_startup_valid((struct ext_schema *)s))return -1;
    struct expr_program *program=malloc(sizeof(*program));if(!program)return -1;
    int rc=-1;
    for(int t=d->tokens[list].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char scope[16];int row,b;
        if(!ext_text(d,t,"scope",scope,sizeof(scope),1)||xz_json_get_int(d,t,"row",&row)||
           xz_json_get_int(d,t,"branch",&b))goto done;
        int sc=!strcmp(scope,"official")?DECL_OFFICIAL:DECL_LOCAL;
        const char *enabled=cfg_value(&r->cfg,sc==DECL_OFFICIAL?"LX06-OFFICIALWAKEUP":"LX06-LOCALWAKEUP");
        struct lx06_match_candidate candidate;struct expr_branch branch;
        if(!enabled||strcmp(enabled,"on")||decl_candidate(&r->table,(enum decl_scope)sc,(size_t)row,"startup.sh",&candidate)||
           candidate.line!=row||lx06_expr_compile(candidate.phrase,strlen(candidate.phrase),program))goto done;
        int fixed=!program->syntax&&!program->slots;
        if((fixed?b!=-1:b<0||(unsigned)b>=program->branches)||
           !(r->effective.paths[sc]&(fixed?REC_FIXED_PATH:REC_REGEX_PATH))||
           lx06_expr_branch(program,fixed?0:(unsigned)b,&branch))goto done;
        if(branch.list_mask) {
            struct expr_list_bounds bounds;
            if(rec_startup_list_program(s,&r->cfg,sc==DECL_LOCAL?"LX06-LOCAL-CHANNEL":"小爱通道唤醒词",(unsigned)row,program,&bounds))goto done;
        }
        unsigned mask=0;int slots=xz_json_object_get(d,t,"slots");
        for(int k=d->tokens[slots].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;char capture[17];
            if(!ext_model_string(d,v,capture,sizeof(capture)))goto done;
            unsigned i;for(i=0;i<program->slots;i++)if(!strcmp(capture,program->names[i]))break;
            if(i==program->slots||!(branch.slot_mask&(1U<<i))||(mask&(1U<<i)))goto done;
            mask|=1U<<i;k=d->tokens[v].next_sibling;
        }
        if(mask!=branch.slot_mask)goto done;
    }rc=0;
done:free(program);return rc;
}

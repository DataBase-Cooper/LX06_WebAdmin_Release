/* Optional explicit deterministic entry association. Existing JSON/tokenizer,
 * intent declarations and version remain authoritative; no model permission. */
#ifndef LX06_APP_EXTENSION_STARTUP_H
#define LX06_APP_EXTENSION_STARTUP_H
static int ext_startup_intent(const xz_json_doc *d,const char *id) {
    int list=xz_json_object_get(d,0,"model_intents"),found=-1;
    if(list<0||d->tokens[list].type!=XZ_JSON_ARRAY)return -1;
    for(int t=d->tokens[list].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char name[65];if(!ext_text(d,t,"intent",name,sizeof(name),1))return -1;
        if(!strcmp(name,id)){if(found>=0)return -1;found=t;}
    }return found;
}
static int ext_startup_valid(struct ext_schema *s) {
    const xz_json_doc *d=&s->doc;int list=xz_json_object_get(d,0,"startup_bindings");
    if(list<0)return 1;
    if(d->tokens[list].type!=XZ_JSON_ARRAY)return 0;
    int count=0;
    for(int t=d->tokens[list].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char scope[16],id[65];double row,branch;
        /* Reuse the existing bounded 16-intent association inventory. */
        if(++count>16||!ext_keys(d,t,"|scope||row||branch||intent||slots|")||
           !ext_text(d,t,"scope",scope,sizeof(scope),1)||
           (strcmp(scope,"local")&&strcmp(scope,"official"))||
           !ext_model_int(d,t,"row",&row)||row<0||row>=1024||
           !ext_model_int(d,t,"branch",&branch)||branch< -1||branch>=64||
           !ext_text(d,t,"intent",id,sizeof(id),1))return 0;
        int intent=ext_startup_intent(d,id),slots=xz_json_object_get(d,t,"slots");
        if(intent<0||slots<0||d->tokens[slots].type!=XZ_JSON_OBJECT)return 0;
        int fields=xz_json_object_get(d,intent,"fields"),n=0;
        for(int k=d->tokens[slots].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;char field[17],capture[17];
            if(++n>4||!ext_model_string(d,k,field,sizeof(field))||!ext_model_slot(field)||
               !ext_raw_eq(d,k,field)||xz_json_object_get(d,fields,field)<0||
               !ext_model_string(d,v,capture,sizeof(capture))||!ext_model_slot(capture)||branch<0)return 0;
            for(int p=d->tokens[slots].first_child;p!=k;) {
                int pv=d->tokens[p].next_sibling;char prior[17];
                if(ext_raw_eq(d,p,field)||!ext_model_string(d,pv,prior,sizeof(prior))||!strcmp(prior,capture))return 0;
                p=d->tokens[pv].next_sibling;
            }k=d->tokens[v].next_sibling;
        }
        for(int p=d->tokens[list].first_child;p!=t;p=d->tokens[p].next_sibling) {
            char ps[16];double pr,pb;
            if(!ext_text(d,p,"scope",ps,sizeof(ps),1)||!ext_model_int(d,p,"row",&pr)||
               !ext_model_int(d,p,"branch",&pb))return 0;
            if(!strcmp(ps,scope)&&pr==row&&pb==branch)return 0;
        }
    }return 1;
}
#endif

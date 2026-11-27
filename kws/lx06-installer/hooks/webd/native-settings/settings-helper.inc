/* Provider-neutral, schema-driven settings. Scripts own defaults, controls and
 * environment mappings. C owns bounded JSON, atomic persistence and exec.
 * Password values never leave through show/save output; blank means retain.
 * run overlays saved values onto the existing script environment only at a new
 * session/client launch. No eval, shell sourcing, daemon, or live cancellation. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "T21 helper draft: exclude from production"
#endif
#define _GNU_SOURCE
#define SETTINGS_LEGACY_ABI 1
/* Existing ext_flow_valid has one legacy indentation warning. Scope the
 * exception to that unchanged header; the new draft compiles with Werror. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../app-extension.h"
#pragma GCC diagnostic pop
#include "../../../runtime/common/script-settings-store.h"
#include "../app-extension-options.h"
#include <sys/types.h>
#include "ctl-config.h"

static struct ext_schema schema;
static char values[EXT_FIELDS][2049];
static int present[EXT_FIELDS];
static char output[EXT_EVENT_CAP+1];static size_t used;
static int append(const char*s) {
    size_t n=strlen(s);if(n+used>=sizeof(output))return -1;
    memcpy(output+used,s,n+1);used+=n;return 0;
}
static int quoted(const char*s) {
    if(append("\""))return -1;
    for(;*s;s++) {
        char b[8];unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\'){b[0]='\\';b[1]=c;b[2]=0;}
        else if(c<32)snprintf(b,sizeof(b),"\\u%04x",c);
        else {b[0]=c;b[1]=0;}
        if(append(b))return -1;
    }
    return append("\"");
}
static int object(int public) {
    int count=0;if(append("{"))return -1;
    for(int i=0;i<schema.nf;i++) {
        if(!present[i]||(public&&!strcmp(schema.fields[i].type,"password")))continue;
        if((count++&&append(","))||quoted(schema.fields[i].name)||append(":")||quoted(values[i]))return -1;
    }
    return append("}");
}
static int message(const char*s,int rc) {
    used=0;append("{\"message\":");quoted(s);append("}");puts(output);return rc;
}
static int read_stored(int dir,const char*name) {
    char raw[SETTINGS_CAP+1];int r=cfg_store_read(dir,name,raw);if(r<=0)return r;
    xz_json_doc d;
    if(xz_json_parse(&d,raw,strlen(raw))||d.tokens[0].type!=XZ_JSON_OBJECT)return -1;
    /* Tolerate removed fields after an upgrade, but not duplicates/hidden
     * objects. Revalidate every field retained by the new script schema. */
    for(int k=d.tokens[0].first_child;k>=0;) {
        char key[33],value[SETTINGS_CAP+1];int v=d.tokens[k].next_sibling;
        if(v<0||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||
           !ext_text(&d,0,key,value,sizeof(value),0))return -1;
        for(int p=d.tokens[0].first_child;p!=k;p=d.tokens[d.tokens[p].next_sibling].next_sibling)
            if(ext_raw_eq(&d,p,key))return -1;
        int f=ext_field_index(&schema,key);
        if(f>=0){if(!ext_value(&schema,f,value))return -1;strcpy(values[f],value);present[f]=1;
            if(schema.fields[f].dynamic){strcpy(schema.current[f],value);schema.current_mask|=1U<<f;}}
        k=d.tokens[v].next_sibling;
    }
    return 1;
}
static int defaults(void) {
    for(int i=0;i<schema.nf;i++) {
        if(present[i])continue;
        char env[65];const char *v=NULL;
        if(!ext_text(&schema.doc,schema.fields[i].token,"env",env,sizeof(env),0))return -1;
        if(*env)v=getenv(env);
        if(v) {
            if(!ext_value(&schema,i,v))return -1;
            strcpy(values[i],v);present[i]=1;
        } else if(xz_json_object_get(&schema.doc,schema.fields[i].token,"default")>=0) {
            if(!ext_text(&schema.doc,schema.fields[i].token,"default",values[i],sizeof(values[i]),0))return -1;
            present[i]=1;
        }
    }
    return 0;
}
#include "ctl-helper.inc"
#define LX06_CTL_JSON_ALREADY_INCLUDED 1
#include "ctl-store.c"
static int save(int dir,int lock,const char*name) {
    char raw[EXT_CAP+2],action[33];size_t n=fread(raw,1,sizeof(raw)-1,stdin);raw[n]=0;
    if(n>EXT_CAP||memchr(raw,0,n)||!ext_request(&schema,raw,action)||strcmp(action,"save"))return -1;
    xz_json_doc d;if(xz_json_parse(&d,raw,n))return -1;
    int vs=xz_json_object_get(&d,0,"values");
    for(int i=0;i<schema.nf;i++)if(xz_json_object_get(&d,vs,schema.fields[i].name)>=0) {
        char b[2049];if(!ext_text(&d,vs,schema.fields[i].name,b,sizeof(b),0))return -1;
        if(!*b&&!strcmp(schema.fields[i].type,"password"))continue;
        strcpy(values[i],b);present[i]=1;
    }
    used=0;if(object(0))return -1;
    return cfg_store_transaction_locked(dir,lock,name,ctl_legacy_edit,output);
}

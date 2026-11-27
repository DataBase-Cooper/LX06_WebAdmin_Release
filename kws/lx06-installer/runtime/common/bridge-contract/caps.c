#include "caps.h"
#include <string.h>

_Static_assert(sizeof(br_caps)+sizeof(br_json)<110*1024,"bounded caps + scratch memory");
#define KIND(d,n,k) (br_json_kind((d),(n))==(k))
static unsigned field(const br_json *d,unsigned n,const char *name){return br_json_field(d,n,name);}
static int nonempty(const br_json *d,unsigned n){return KIND(d,n,BR_J_STRING)&&br_json_text(d,n).length>0;}
static int boolean(const br_json *d,unsigned n){return KIND(d,n,BR_J_TRUE)||KIND(d,n,BR_J_FALSE);}
static int positive(const br_json *d,unsigned n,uint64_t *out){return br_json_u64(d,n,out)&&*out>0;}
static int strings(const br_json *d,unsigned n){
    if(!n)return 1; /* optional declaration defaults to [] */
    if(!KIND(d,n,BR_J_ARRAY))return 0;
    for(unsigned i=br_json_child(d,n);i&&i<br_json_next(d,n);i=br_json_next(d,i)){
        if(!nonempty(d,i))return 0;
        for(unsigned j=br_json_child(d,n);j<i;j=br_json_next(d,j))
            if(br_json_equal(d,i,d,j))return 0;
    }
    return 1;
}
static int contains(const br_json *d,unsigned n,const br_json *v,unsigned value){
    if(!KIND(d,n,BR_J_ARRAY))return 0;
    for(unsigned i=br_json_child(d,n);i&&i<br_json_next(d,n);i=br_json_next(d,i))
        if(br_json_equal(d,i,v,value))return 1;
    return 0;
}
static int named(const br_json *d,unsigned n,const char *text){
    if(!KIND(d,n,BR_J_ARRAY))return 0;
    for(unsigned i=br_json_child(d,n);i&&i<br_json_next(d,n);i=br_json_next(d,i))
        if(br_json_is(d,i,text))return 1;
    return 0;
}
static unsigned find(const br_json *d,unsigned list,const char *key,const br_json *v,unsigned name){
    if(!KIND(d,list,BR_J_ARRAY))return 0;
    for(unsigned i=br_json_child(d,list);i&&i<br_json_next(d,list);i=br_json_next(d,i))
        if(br_json_equal(d,field(d,i,key),v,name))return i;
    return 0;
}
static int cmp_unsigned(const br_json *d,unsigned n,uint64_t value){
    uint64_t number;
    if(br_json_u64(d,n,&number))return number<value?-1:number>value?1:0;
    return br_json_text(d,n).data[0]=='-'?-1:1; /* exact overflow sign */
}
static int declarations_at(const br_json *d,unsigned n,int lists){
    if(!KIND(d,n,BR_J_OBJECT))return 0;
    for(unsigned k=br_json_child(d,n);k&&k<br_json_next(d,n);k=br_json_next(d,k+1)){
        unsigned decl=k+1,t=field(d,decl,"type"),a=field(d,decl,"min"),b=field(d,decl,"max");
        unsigned optional=field(d,decl,"optional");
        if(!br_json_text(d,k).length||!KIND(d,decl,BR_J_OBJECT)||
           !(br_json_is(d,t,"string")||br_json_is(d,t,"int")||br_json_is(d,t,"enum")||br_json_is(d,t,"bool")||
             (lists&&br_json_is(d,t,"array")))||
           (optional&&!boolean(d,optional))||(a&&!KIND(d,a,BR_J_INT))||(b&&!KIND(d,b,BR_J_INT)))return 0;
        if(a&&b&&br_json_int_cmp(d,a,d,b)>0)return 0;
        if(br_json_is(d,t,"string")&&b&&cmp_unsigned(d,b,1)<0)return 0;
        if(br_json_is(d,t,"array")){
            uint64_t lo,hi;unsigned item=field(d,decl,"items"),extra=field(d,item,"additionalProperties");
            if(!positive(d,field(d,decl,"minItems"),&lo)||!positive(d,field(d,decl,"maxItems"),&hi)||lo>hi||
               !KIND(d,item,BR_J_OBJECT)||!br_json_is(d,field(d,item,"type"),"object")||
               (extra&&!KIND(d,extra,BR_J_FALSE))||!declarations_at(d,field(d,item,"properties"),0))return 0;
        }
        if(br_json_is(d,t,"enum")){
            unsigned vals=field(d,decl,"values");
            if(!KIND(d,vals,BR_J_ARRAY)||!br_json_child(d,vals))return 0;
            for(unsigned i=br_json_child(d,vals);i<br_json_next(d,vals);i=br_json_next(d,i)){
                if(!KIND(d,i,BR_J_INT)&&!KIND(d,i,BR_J_STRING))return 0;
                for(unsigned j=br_json_child(d,vals);j<i;j=br_json_next(d,j))
                    if(br_json_equal(d,i,d,j))return 0;
            }
        }
    }
    return 1;
}
static int declarations(const br_json *d,unsigned n){return declarations_at(d,n,1);}
static int unique_names(const br_json *d,unsigned list,const char *key){
    if(!KIND(d,list,BR_J_ARRAY))return 0;
    for(unsigned i=br_json_child(d,list);i&&i<br_json_next(d,list);i=br_json_next(d,i)){
        unsigned name=field(d,i,key);
        if(!nonempty(d,name))return 0;
        for(unsigned j=br_json_child(d,list);j<i;j=br_json_next(d,j))
            if(br_json_equal(d,name,d,field(d,j,key)))return 0;
    }
    return 1;
}
static int op_shape(const br_json *d,unsigned op){
    uint64_t timeout;
    unsigned s=field(d,op,"session"),r=field(d,op,"returns");
    if(!positive(d,field(d,op,"timeout_ms"),&timeout)||!declarations(d,field(d,op,"params"))||
       (s&&!br_json_is(d,s,"keep")&&!br_json_is(d,s,"end"))||!strings(d,field(d,op,"fallback")))return 0;
    const char *bools[]={"oneway","async","silent"};
    for(unsigned i=0;i<3;i++){
        unsigned v=field(d,op,bools[i]);if(v&&!boolean(d,v))return 0;
    }
    return !r||(KIND(d,r,BR_J_OBJECT)&&strings(d,field(d,r,"json_fields")));
}
static int shape(const br_json *d){
    if(!KIND(d,1,BR_J_OBJECT))return BR_CAPS_INVALID;
    for(unsigned i=1;i<=d->count;i++)
        if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"authorization")||br_json_key_ci(d,i,"phone_token"))return BR_CAPS_INVALID;
    if(!br_json_is(d,field(d,1,"bridge_version"),LX06_BRIDGE_VERSION))return BR_CAPS_VERSION;
    unsigned apps=field(d,1,"apps");
    if(!br_json_is(d,field(d,1,"kind"),"caps")||!KIND(d,field(d,1,"bridge"),BR_J_TRUE)||
       !unique_names(d,apps,"app")||!strings(d,field(d,1,"features"))||
       !strings(d,field(d,1,"parameter_keys"))||!strings(d,field(d,1,"audio_keys")))return BR_CAPS_INVALID;
    unsigned audio_keys=field(d,1,"audio_keys");
    for(unsigned i=br_json_child(d,audio_keys);i&&i<br_json_next(d,audio_keys);i=br_json_next(d,i))
        if(!br_json_is(d,i,"mic")&&!br_json_is(d,i,"speaker")&&!br_json_is(d,i,"ring"))return BR_CAPS_INVALID;
    for(unsigned a=br_json_child(d,apps);a&&a<br_json_next(d,apps);a=br_json_next(d,a)){
        unsigned proto=field(d,a,"hook_protocol"),audio=field(d,a,"audio"),ops=field(d,a,"ops");
        unsigned compat=field(d,a,"compat"),session=field(d,a,"session"),conn=field(d,a,"connection");
        if(field(d,a,"app_id")||field(d,a,"operations"))return BR_CAPS_INVALID;
        if(!KIND(d,proto,BR_J_OBJECT)||!br_json_is(d,field(d,proto,"min"),"1.0")||
           !br_json_is(d,field(d,proto,"max"),"1.0"))return BR_CAPS_HOOK;
        unsigned proto_keys=0;
        for(unsigned k=br_json_child(d,proto);k&&k<br_json_next(d,proto);k=br_json_next(d,k+1))proto_keys++;
        if(proto_keys!=2)return BR_CAPS_HOOK;
        if(compat&&!br_json_is(d,compat,"ok")&&!br_json_is(d,compat,"degraded")&&
           !br_json_is(d,compat,"absent")&&!br_json_is(d,compat,"out_of_range"))return BR_CAPS_INVALID;
        const char *keys[]={"mic","speaker","ring"},*words[]={"input","output","ringtone"};
        for(unsigned i=0;i<3;i++){
            unsigned v=field(d,audio,keys[i]);
            if(!br_json_is(d,v,words[i])&&!br_json_is(d,v,"none"))return BR_CAPS_INVALID;
        }
        if(!unique_names(d,ops,"name")||!strings(d,field(d,a,"parameter_keys")))return BR_CAPS_INVALID;
        for(unsigned o=br_json_child(d,ops);o&&o<br_json_next(d,ops);o=br_json_next(d,o))
            if(!op_shape(d,o))return BR_CAPS_INVALID;
        if(session){
            unsigned c=field(d,session,"cancel"),end=field(d,session,"end_op");
            if(!KIND(d,session,BR_J_OBJECT)||(!br_json_is(d,c,"pause_hold")&&!br_json_is(d,c,"freeze_only"))||
               (end&&!find(d,ops,"name",d,end)))return BR_CAPS_INVALID;
        }
        if(conn){
            uint64_t timeout;
            if(!KIND(d,conn,BR_J_OBJECT)||!find(d,ops,"name",d,field(d,conn,"op"))||
               !nonempty(d,field(d,conn,"ready_signal"))||!positive(d,field(d,conn,"timeout_ms"),&timeout))return BR_CAPS_INVALID;
        }
    }
    unsigned peers=field(d,1,"peer_ops");
    if(peers){
        if(!unique_names(d,peers,"op"))return BR_CAPS_INVALID;
        for(unsigned o=br_json_child(d,peers);o&&o<br_json_next(d,peers);o=br_json_next(d,o))
            if(!declarations(d,field(d,o,"params")))return BR_CAPS_INVALID;
    }
    return BR_CAPS_OK;
}
int br_caps_validate(br_json *d,const void *data,size_t length){
    if(!d)return BR_CAPS_INVALID;
    int rc=br_json_parse(d,data,length)==1?shape(d):BR_CAPS_INVALID;
    if(rc!=BR_CAPS_OK)memset(d,0,sizeof(*d));
    return rc;
}
/* Official rev13 4.9 / WFP-1-A: the complete bounded row declaration
 * is the online capability. No version/feature/approval flag grants it.
 * Only accept() binds a validated document to an authenticated READY epoch. */
int br_caps_validate_live(br_json *d,const void *data,size_t length){
    return br_caps_validate(d,data,length);
}
void br_caps_forget(br_caps *c){if(c)memset(c,0,sizeof(*c));}
void br_caps_begin(br_caps *c,uint64_t connection){if(c){br_caps_forget(c);c->connection=connection;}}
int br_caps_accept(br_caps *c,uint64_t connection,unsigned frame,const void *data,size_t length){
    if(!c)return BR_CAPS_INVALID;
    c->ready=0;memset(&c->document,0,sizeof(c->document));
    if(!connection||connection!=c->connection)return BR_CAPS_OFFLINE;
    if(frame!=BR_WIRE_READY)return BR_CAPS_INVALID;
    int rc=br_caps_validate_live(&c->document,data,length);
    c->ready=rc==BR_CAPS_OK;return rc;
}
static unsigned property(const br_json *,unsigned,const br_json *,unsigned);
static int schema_declaration_subset(const br_json *,unsigned,const br_json *,unsigned);
static int parameters(const br_json *,unsigned,const br_json *,unsigned);
/* Mapped declarations and constants partition whole parameter names. Only the
 * save-only API supplies mapped; original live admission always supplies 0. */
static int parameter_bindings(const br_json *d,unsigned decls,const br_json *r,unsigned values,unsigned mapped){
    if(values&&!KIND(r,values,BR_J_OBJECT))return 0;
    /* Reject undeclared keys, using decoded key bytes without C string casts. */
    for(unsigned k=br_json_child(r,values);k&&k<br_json_next(r,values);k=br_json_next(r,k+1)){
        int found=0;
        for(unsigned j=br_json_child(d,decls);j&&j<br_json_next(d,decls);j=br_json_next(d,j+1))
            if(br_json_equal(r,k,d,j)){found=1;break;}
        if(!found)return 0;
    }
    for(unsigned k=br_json_child(r,mapped);k&&k<br_json_next(r,mapped);k=br_json_next(r,k+1))
        if(!property(d,decls,r,k)||property(r,values,r,k))return 0;
    for(unsigned k=br_json_child(d,decls);k&&k<br_json_next(d,decls);k=br_json_next(d,k+1)){
        unsigned val=0,decl=k+1,type=field(d,decl,"type");
        for(unsigned j=br_json_child(r,values);j&&j<br_json_next(r,values);j=br_json_next(r,j+1))
            if(br_json_equal(d,k,r,j)){val=j+1;break;}
        if(!val){
            unsigned from=property(r,mapped,d,k);
            if(from){if(!schema_declaration_subset(d,decl,r,from))return 0;}
            else if(!KIND(d,field(d,decl,"optional"),BR_J_TRUE))return 0;
            continue;
        }
        unsigned min=field(d,decl,"min"),max=field(d,decl,"max");
        if(br_json_is(d,type,"array")){
            uint64_t lo,hi,count=0;
            if(!KIND(r,val,BR_J_ARRAY)||!positive(d,field(d,decl,"minItems"),&lo)||
               !positive(d,field(d,decl,"maxItems"),&hi))return 0;
            unsigned props=field(d,field(d,decl,"items"),"properties");
            for(unsigned row=br_json_child(r,val);row&&row<br_json_next(r,val);row=br_json_next(r,row))
                if(++count>hi||!KIND(r,row,BR_J_OBJECT)||!parameters(d,props,r,row))return 0;
            if(count<lo)return 0;
        }else if(br_json_is(d,type,"enum")){if(!contains(d,field(d,decl,"values"),r,val))return 0;}
        else if(br_json_is(d,type,"bool")){if(!boolean(r,val))return 0;}
        else if(br_json_is(d,type,"int")){
            if(!KIND(r,val,BR_J_INT)||(min&&br_json_int_cmp(r,val,d,min)<0)||(max&&br_json_int_cmp(r,val,d,max)>0))return 0;
        }else{
            size_t length=br_json_characters(r,val);
            if(!KIND(r,val,BR_J_STRING)||!length||(min&&cmp_unsigned(d,min,length)>0)||
               (max&&cmp_unsigned(d,max,length)<0))return 0;
        }
    }
    return 1;
}
static int parameters(const br_json *d,unsigned decls,const br_json *r,unsigned values){
    return parameter_bindings(d,decls,r,values,0);
}
static int set_and_audio(const br_json *d,const br_json *r,unsigned app,unsigned *audio){
    unsigned settings=field(r,1,"set"),vector=field(r,1,"audio");
    if((settings&&!KIND(r,settings,BR_J_OBJECT))||(vector&&!KIND(r,vector,BR_J_OBJECT)))return 0;
    unsigned allowed=field(d,1,"parameter_keys");
    for(unsigned k=br_json_child(r,settings);k&&k<br_json_next(r,settings);k=br_json_next(r,k+1)){
        int ok=0;
        for(unsigned j=br_json_child(d,allowed);j&&j<br_json_next(d,allowed);j=br_json_next(d,j)){
            br_text a=br_json_text(r,k),b=br_json_text(d,j);
            if(a.length==b.length&&!memcmp(a.data,b.data,a.length)){ok=1;break;}
        }
        if(!ok)return 0;
    }
    *audio=0;
    const char *keys[]={"mic","speaker","ring"};
    for(unsigned k=br_json_child(r,vector);k&&k<br_json_next(r,vector);k=br_json_next(r,k+1)){
        unsigned i;for(i=0;i<3;i++)if(br_json_is(r,k,keys[i]))break;
        if(i==3||(!br_json_is(r,k+1,"open")&&!br_json_is(r,k+1,"close")))return 0;
        if(br_json_is(r,k+1,"open")){
            if(br_json_is(d,field(d,field(d,app,"audio"),keys[i]),"none"))return 0;
            *audio|=1u<<i;
        }
    }
    return 1;
}
static int uuid7(br_text t){
    if(t.length!=36)return 0;
    for(size_t i=0;i<36;i++){
        unsigned c=t.data[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return 0;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return 0;
    }
    return t.data[14]=='7'&&(t.data[19]=='8'||t.data[19]=='9'||t.data[19]=='a'||
        t.data[19]=='A'||t.data[19]=='b'||t.data[19]=='B');
}
static int envelope(const br_json *r,uint64_t epoch,int mode){
    uint64_t value;unsigned wake=field(r,1,"wake_source"),text=field(r,1,"wake_text");
    unsigned action=field(r,1,"action");
    /* System actions have separate feature/schema/owner admission. A hook's
     * arbitrary app/op declaration cannot capture their namespaces. */
    static const char *const reserved[]={"caps.request","audio.set","system.op","state.query",
        "task.list","task.cancel","hook.describe"};
    for(unsigned i=0;i<sizeof(reserved)/sizeof(reserved[0]);i++)
        if(br_json_is(r,action,reserved[i])&&
           !(mode==1&&br_json_is(r,action,"task.list"))&&
           !(mode==2&&br_json_is(r,action,"system.op"))&&
           !(mode==3&&br_json_is(r,action,"caps.request")))return 0;
    if((mode==1&&!br_json_is(r,action,"task.list"))||
       (mode==2&&!br_json_is(r,action,"system.op"))||
       (mode==3&&!br_json_is(r,action,"caps.request")))return 0;
    br_text name=br_json_text(r,action);
    if(name.length>=5&&!memcmp(name.data,"flow:",5))return 0;
    if(!epoch||!br_json_u64(r,field(r,1,"epoch"),&value)||value!=epoch||
       !nonempty(r,field(r,1,"action"))||!nonempty(r,field(r,1,"script_id"))||
       !KIND(r,field(r,1,"command_id"),BR_J_STRING)||!uuid7(br_json_text(r,field(r,1,"command_id")))||
       (!br_json_is(r,wake,"official")&&!br_json_is(r,wake,"local")&&!br_json_is(r,wake,"scheduled"))||
       (text&&(!KIND(r,text,BR_J_STRING)||br_json_characters(r,text)>128))||
       (br_json_is(r,wake,"scheduled")&&text&&br_json_text(r,text).length))return 0;
    for(unsigned i=1;i<=r->count;i++)
        if(br_json_key_ci(r,i,"token")||br_json_key_ci(r,i,"authorization")||br_json_key_ci(r,i,"phone_token"))return 0;
    return 1;
}
static int action_equal(br_text action,br_text app,br_text op){
    return action.length==app.length+1+op.length&&!memcmp(action.data,app.data,app.length)&&
        action.data[app.length]=='.'&&!memcmp(action.data+app.length+1,op.data,op.length);
}
static int prepare(const br_json *d,const void *data,size_t length,br_json *r,br_caps_plan *out,int mode,uint64_t epoch){
    int peer=mode==1;
    if(!d||!r||d==r||(!peer&&!out))return BR_CAPS_INVALID;
    int rc=BR_CAPS_INVALID;
    if(br_json_parse(r,data,length)!=1||!KIND(r,1,BR_J_OBJECT))goto done;
    unsigned app=0,op_name=field(r,1,"op"),op=0;
    if(mode==2){
        if(!envelope(r,epoch,0))goto done;
        br_text action=br_json_text(r,field(r,1,"action"));unsigned count=0;
        for(unsigned a=br_json_child(d,field(d,1,"apps"));a&&a<br_json_next(d,field(d,1,"apps"));a=br_json_next(d,a)){
            unsigned ops=field(d,a,"ops");
            for(unsigned o=br_json_child(d,ops);o&&o<br_json_next(d,ops);o=br_json_next(d,o)){
                if(action_equal(action,br_json_text(d,field(d,a,"app")),br_json_text(d,field(d,o,"name")))){
                    app=a;op=o;count++;
                }
            }
        }
        if(count!=1||(field(r,1,"target")&&!br_json_equal(r,field(r,1,"target"),d,field(d,app,"app"))))goto done;
    }else{
        if(!nonempty(r,op_name))goto done;
        if(peer){
            op=find(d,field(d,1,"peer_ops"),"op",r,op_name);
            if(op&&parameters(d,field(d,op,"params"),r,field(r,1,"params")))rc=BR_CAPS_OK;
            goto done;
        }
        unsigned app_name=field(r,1,"app");
        if(!nonempty(r,app_name))goto done;
        app=find(d,field(d,1,"apps"),"app",r,app_name);
        if(!app){rc=BR_CAPS_APP_MISSING;goto done;}
        op=find(d,field(d,app,"ops"),"name",r,op_name);
    }
    if(br_json_is(d,field(d,app,"compat"),"absent")&&br_json_is(d,field(d,app,"reason"),"protocol")){
        rc=BR_CAPS_HOOK;goto done;
    }
    if(br_json_is(d,field(d,app,"compat"),"out_of_range")){rc=BR_CAPS_APP_VERSION;goto done;}
    if(!op||!parameters(d,field(d,op,"params"),r,field(r,1,"params")))goto done;
    br_caps_plan p={0};
    if(!set_and_audio(d,r,app,&p.requested_audio))goto done;
    p.app=br_json_text(d,field(d,app,"app"));p.op=br_json_text(d,field(d,op,"name"));
    (void)br_json_u64(d,field(d,op,"timeout_ms"),&p.timeout_ms);
    p.session_end=br_json_is(d,field(d,op,"session"),"end");
    p.oneway=KIND(d,field(d,op,"oneway"),BR_J_TRUE);
    p.asynchronous=KIND(d,field(d,op,"async"),BR_J_TRUE);
    p.silent=KIND(d,field(d,op,"silent"),BR_J_TRUE);
    if(p.silent&&p.requested_audio)goto done;
    p.degraded=br_json_is(d,field(d,app,"compat"),"degraded");
    unsigned session=field(d,app,"session"),conn=field(d,app,"connection");
    p.end_op=br_json_text(d,field(d,session,"end_op"));
    p.cancel_mode=br_json_is(d,field(d,session,"cancel"),"pause_hold")?1:
                  br_json_is(d,field(d,session,"cancel"),"freeze_only")?2:0;
    p.connection_op=br_json_text(d,field(d,conn,"op"));
    p.ready_signal=br_json_text(d,field(d,conn,"ready_signal"));
    if(conn)(void)br_json_u64(d,field(d,conn,"timeout_ms"),&p.connection_timeout_ms);
    *out=p;rc=BR_CAPS_OK;
done:
    memset(r,0,sizeof(*r));return rc;
}
int br_caps_save_operation(const br_json *snapshot,const void *data,size_t length,br_json *r,br_caps_plan *out){
    if(!snapshot)return BR_CAPS_INVALID;
    int rc=shape(snapshot);
    return rc==BR_CAPS_OK?prepare(snapshot,data,length,r,out,0,0):rc;
}
static unsigned property(const br_json *d,unsigned object,const br_json *s,unsigned key){
    for(unsigned k=br_json_child(d,object);k&&k<br_json_next(d,object);k=br_json_next(d,k+1))
        if(br_json_equal(d,k,s,key))return k+1;
    return 0;
}
/* Same-type, conservative inclusion of all admitted values, not one example.
 * Declaration trees are validated before entry; arrays have scalar rows only. */
static int schema_subset(const br_json *,unsigned,const br_json *,unsigned);
static int schema_declaration_subset(const br_json *d,unsigned to,const br_json *s,unsigned from){
    unsigned type=field(s,from,"type");
    if(!to||!br_json_equal(s,type,d,field(d,to,"type"))||
       (KIND(s,field(s,from,"optional"),BR_J_TRUE)&&!KIND(d,field(d,to,"optional"),BR_J_TRUE)))return 0;
    unsigned lo=field(s,from,"min"),hi=field(s,from,"max");
    unsigned tlo=field(d,to,"min"),thi=field(d,to,"max");
    if(br_json_is(s,type,"array")){
        if(br_json_int_cmp(s,field(s,from,"minItems"),d,field(d,to,"minItems"))<0||
           br_json_int_cmp(s,field(s,from,"maxItems"),d,field(d,to,"maxItems"))>0||
           !schema_subset(d,field(d,field(d,to,"items"),"properties"),
                          s,field(s,field(s,from,"items"),"properties")))return 0;
    }else if(br_json_is(s,type,"enum")){
        unsigned values=field(s,from,"values");
        for(unsigned v=br_json_child(s,values);v&&v<br_json_next(s,values);v=br_json_next(s,v))
            if(!contains(d,field(d,to,"values"),s,v))return 0;
    }else if(br_json_is(s,type,"int")){
        if((tlo&&(!lo||br_json_int_cmp(s,lo,d,tlo)<0))||
           (thi&&(!hi||br_json_int_cmp(s,hi,d,thi)>0)))return 0;
    }else if(br_json_is(s,type,"string")){
        /* Empty strings never pass the value validator, regardless of min. */
        if((tlo&&cmp_unsigned(d,tlo,1)>0&&(!lo||br_json_int_cmp(s,lo,d,tlo)<0))||
           (thi&&(!hi||br_json_int_cmp(s,hi,d,thi)>0)))return 0;
    }
    return 1;
}
static int schema_subset(const br_json *d,unsigned target,const br_json *s,unsigned source){
    for(unsigned k=br_json_child(s,source);k&&k<br_json_next(s,source);k=br_json_next(s,k+1))
        if(!schema_declaration_subset(d,property(d,target,s,k),s,k+1))return 0;
    for(unsigned k=br_json_child(d,target);k&&k<br_json_next(d,target);k=br_json_next(d,k+1))
        if(!KIND(d,field(d,k+1,"optional"),BR_J_TRUE)&&!property(s,source,d,k))return 0;
    return 1;
}
static int save_required_overlap(const void *a,size_t an,const void *b,size_t bn){
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return x<=y?y-x<an:x-y<bn;
}
static int save_parameter(const br_json *snapshot,const char *app_name,const char *op_name,
                          const char *parameter,const void *output,size_t output_size,unsigned *declaration){
    if(!snapshot||!app_name||!op_name||!parameter||!output||
       save_required_overlap(snapshot,sizeof(*snapshot),output,output_size))return BR_CAPS_INVALID;
    const char *names[]={app_name,op_name,parameter};
    for(unsigned i=0;i<3;i++){
        size_t n=0;while(n<=LX06_BRIDGE_PAYLOAD_MAX&&names[i][n])n++;
        if(!n||n>LX06_BRIDGE_PAYLOAD_MAX||
           save_required_overlap(names[i],n+1,output,output_size))return BR_CAPS_INVALID;
    }
    int rc=shape(snapshot);if(rc!=BR_CAPS_OK)return rc;
    unsigned apps=field(snapshot,1,"apps"),app=0,op=0;
    for(unsigned a=br_json_child(snapshot,apps);a&&a<br_json_next(snapshot,apps);a=br_json_next(snapshot,a))
        if(br_json_is(snapshot,field(snapshot,a,"app"),app_name)){app=a;break;}
    if(!app)return BR_CAPS_APP_MISSING;
    if(br_json_is(snapshot,field(snapshot,app,"compat"),"absent")&&
       br_json_is(snapshot,field(snapshot,app,"reason"),"protocol"))return BR_CAPS_HOOK;
    if(br_json_is(snapshot,field(snapshot,app,"compat"),"out_of_range"))return BR_CAPS_APP_VERSION;
    unsigned ops=field(snapshot,app,"ops");
    for(unsigned o=br_json_child(snapshot,ops);o&&o<br_json_next(snapshot,ops);o=br_json_next(snapshot,o))
        if(br_json_is(snapshot,field(snapshot,o,"name"),op_name)){op=o;break;}
    if(!op)return BR_CAPS_INVALID;
    unsigned decl=field(snapshot,field(snapshot,op,"params"),parameter);
    if(!decl)return BR_CAPS_INVALID;
    *declaration=decl;
    return BR_CAPS_OK;
}
int br_caps_save_required(const br_json *snapshot,const char *app_name,const char *op_name,
                          const char *parameter,int *required){
    unsigned decl;int rc=save_parameter(snapshot,app_name,op_name,parameter,required,sizeof(*required),&decl);
    if(rc!=BR_CAPS_OK)return rc;
    *required=!KIND(snapshot,field(snapshot,decl,"optional"),BR_J_TRUE);
    return BR_CAPS_OK;
}
int br_caps_save_type_mask(const br_json *snapshot,const char *app_name,const char *op_name,
                          const char *parameter,unsigned *types){
    unsigned decl;int rc=save_parameter(snapshot,app_name,op_name,parameter,types,sizeof(*types),&decl);
    if(rc!=BR_CAPS_OK)return rc;
    unsigned type=field(snapshot,decl,"type"),mask=0;
    if(br_json_is(snapshot,type,"string"))mask=BR_CAPS_PARAM_STRING;
    else if(br_json_is(snapshot,type,"int"))mask=BR_CAPS_PARAM_INT;
    else if(br_json_is(snapshot,type,"bool"))mask=BR_CAPS_PARAM_BOOL;
    else if(br_json_is(snapshot,type,"enum")){
        unsigned values=field(snapshot,decl,"values");
        for(unsigned v=br_json_child(snapshot,values);v&&v<br_json_next(snapshot,values);v=br_json_next(snapshot,v)){
            if(KIND(snapshot,v,BR_J_STRING))mask|=BR_CAPS_PARAM_STRING;
            else if(KIND(snapshot,v,BR_J_INT))mask|=BR_CAPS_PARAM_INT;
            else return BR_CAPS_INVALID; /* Preserve the original enum schema. */
        }
    }
    if(!mask)return BR_CAPS_INVALID;
    *types=mask;return BR_CAPS_OK;
}
/* mode: 0 mapped-only, 1 peer constants, 2 disjoint constants + mapped. */
static int save_request(const br_json *snapshot,const void *data,size_t length,br_json *r,int peer,int mode){
    if(!r||r==snapshot)return BR_CAPS_INVALID;
    int rc=snapshot?shape(snapshot):BR_CAPS_INVALID;
    if(rc!=BR_CAPS_OK)goto done;
    rc=BR_CAPS_INVALID;
    if(br_json_parse(r,data,length)!=1||!KIND(r,1,BR_J_OBJECT))goto done;
    for(unsigned i=1;i<=r->count;i++)
        if(br_json_key_ci(r,i,"token")||br_json_key_ci(r,i,"authorization")||br_json_key_ci(r,i,"phone_token"))goto done;
    for(unsigned k=br_json_child(r,1);k&&k<br_json_next(r,1);k=br_json_next(r,k+1))
        if(!(!peer&&br_json_is(r,k,"app"))&&!br_json_is(r,k,"op")&&!br_json_is(r,k,"params")&&
           !(mode==2&&br_json_is(r,k,"schema")))goto done;
    unsigned name=field(r,1,"app"),op_name=field(r,1,"op"),params=field(r,1,"params"),mapped=field(r,1,"schema");
    if((!peer&&!nonempty(r,name))||!nonempty(r,op_name)||(mode==0&&params&&!declarations(r,params))||
       (mode==2&&mapped&&!declarations(r,mapped)))goto done;
    if(peer){
        unsigned op=find(snapshot,field(snapshot,1,"peer_ops"),"op",r,op_name);
        if(op&&(mode==2?parameter_bindings(snapshot,field(snapshot,op,"params"),r,params,mapped):
                mode==1?parameters(snapshot,field(snapshot,op,"params"),r,params):
                          schema_subset(snapshot,field(snapshot,op,"params"),r,params)))rc=BR_CAPS_OK;
        goto done;
    }
    unsigned app=find(snapshot,field(snapshot,1,"apps"),"app",r,name);
    if(!app){rc=BR_CAPS_APP_MISSING;goto done;}
    if(br_json_is(snapshot,field(snapshot,app,"compat"),"absent")&&br_json_is(snapshot,field(snapshot,app,"reason"),"protocol")){
        rc=BR_CAPS_HOOK;goto done;
    }
    if(br_json_is(snapshot,field(snapshot,app,"compat"),"out_of_range")){rc=BR_CAPS_APP_VERSION;goto done;}
    unsigned op=find(snapshot,field(snapshot,app,"ops"),"name",r,op_name);
    if(op&&(mode==2?parameter_bindings(snapshot,field(snapshot,op,"params"),r,params,mapped):
                   schema_subset(snapshot,field(snapshot,op,"params"),r,params)))rc=BR_CAPS_OK;
done:memset(r,0,sizeof(*r));return rc;
}
int br_caps_save_schema(const br_json *snapshot,const void *data,size_t length,br_json *r){
    return save_request(snapshot,data,length,r,0,0);
}
int br_caps_save_peer_schema(const br_json *snapshot,const void *data,size_t length,br_json *r){
    return save_request(snapshot,data,length,r,1,0);
}
int br_caps_save_peer_operation(const br_json *snapshot,const void *data,size_t length,br_json *r){
    return save_request(snapshot,data,length,r,1,1);
}
int br_caps_save_bindings(const br_json *snapshot,const void *data,size_t length,br_json *r){
    return save_request(snapshot,data,length,r,0,2);
}
int br_caps_save_peer_bindings(const br_json *snapshot,const void *data,size_t length,br_json *r){
    return save_request(snapshot,data,length,r,1,2);
}
static int live(const br_caps *c,uint64_t connection){return c&&c->ready&&connection&&c->connection==connection;}
int br_caps_operation(const br_caps *c,uint64_t connection,const void *data,size_t length,br_json *r,br_caps_plan *out){
    return live(c,connection)?prepare(&c->document,data,length,r,out,0,0):BR_CAPS_OFFLINE;
}
int br_caps_open_operation(const br_caps *c,uint64_t connection,const void *data,size_t length,br_json *r,br_caps_plan *out){
    if(live(c,connection))return prepare(&c->document,data,length,r,out,2,connection);
    if(r&&(!c||r!=&c->document))memset(r,0,sizeof(*r));
    return BR_CAPS_OFFLINE;
}
int br_caps_peer_operation(const br_caps *c,uint64_t connection,const void *data,size_t length,br_json *r){
    return live(c,connection)?prepare(&c->document,data,length,r,NULL,1,0):BR_CAPS_OFFLINE;
}
int br_caps_system_request(const br_caps *c,uint64_t connection,const void *data,size_t length,br_json *r){
    if(!r||(c&&r==&c->document))return BR_CAPS_INVALID;
    int rc=BR_CAPS_INVALID;
    if(!live(c,connection)){rc=BR_CAPS_OFFLINE;goto done;}
    if(br_json_parse(r,data,length)!=1||!KIND(r,1,BR_J_OBJECT)||!envelope(r,connection,2)||
       !br_json_is(r,field(r,1,"target"),"system")||field(r,1,"audio")||field(r,1,"set"))goto done;
    unsigned request=field(r,1,"params"),name=field(r,request,"op");
    if(!KIND(r,request,BR_J_OBJECT)||!nonempty(r,name))goto done;
    for(unsigned k=br_json_child(r,request);k&&k<br_json_next(r,request);k=br_json_next(r,k+1))
        if(!br_json_is(r,k,"op")&&!br_json_is(r,k,"params"))goto done;
    unsigned op=find(&c->document,field(&c->document,1,"peer_ops"),"op",r,name);
    if(op&&parameters(&c->document,field(&c->document,op,"params"),r,field(r,request,"params")))rc=BR_CAPS_OK;
done:memset(r,0,sizeof(*r));return rc;
}
int br_caps_task_list_request(const br_caps *c,uint64_t connection,const void *data,size_t length,br_json *r){
    if(!r||(c&&r==&c->document))return BR_CAPS_INVALID;
    int rc=BR_CAPS_INVALID;
    if(!live(c,connection)){rc=BR_CAPS_OFFLINE;goto done;}
    if(br_json_parse(r,data,length)!=1||!KIND(r,1,BR_J_OBJECT)||!envelope(r,connection,1))goto done;
    unsigned params=field(r,1,"params");
    if(field(r,1,"target")||field(r,1,"audio")||field(r,1,"set")||
       (params&&(!KIND(r,params,BR_J_OBJECT)||br_json_child(r,params))))goto done;
    rc=BR_CAPS_OK;
done:memset(r,0,sizeof(*r));return rc;
}
int br_caps_discovery_request(uint64_t epoch,const void *data,size_t length,br_json *r){
    if(!r)return BR_CAPS_INVALID;
    int rc=BR_CAPS_INVALID;
    if(br_json_parse(r,data,length)!=1||!KIND(r,1,BR_J_OBJECT)||!envelope(r,epoch,3))goto done;
    unsigned target=field(r,1,"target"),params=field(r,1,"params");
    if((target&&!br_json_is(r,target,"system"))||field(r,1,"audio")||field(r,1,"set")||
       (params&&(!KIND(r,params,BR_J_OBJECT)||br_json_child(r,params))))goto done;
    rc=BR_CAPS_OK;
done:memset(r,0,sizeof(*r));return rc;
}
int br_caps_has_feature(const br_caps *c,uint64_t connection,const char *feature){
    return live(c,connection)&&feature&&named(&c->document,field(&c->document,1,"features"),feature);
}
int br_caps_describe_request(const br_caps *c,uint64_t connection,const void *data,size_t length,br_json *r){
    if(!live(c,connection))return BR_CAPS_OFFLINE;
    if(!r||r==&c->document)return BR_CAPS_INVALID;
    int rc=BR_CAPS_INVALID;
    if(!br_caps_has_feature(c,connection,"hook.describe")||br_json_parse(r,data,length)!=1||!KIND(r,1,BR_J_OBJECT))goto done;
    unsigned app=field(r,1,"app");
    if(app&&(!nonempty(r,app)||!find(&c->document,field(&c->document,1,"apps"),"app",r,app)))goto done;
    rc=BR_CAPS_OK;
done:
    memset(r,0,sizeof(*r));return rc;
}
const char *br_caps_error(int rc){
    switch(rc){
    case BR_CAPS_OK:return NULL;
    case BR_CAPS_VERSION:return "bridge_version_mismatch";
    case BR_CAPS_HOOK:return "hook_version_mismatch";
    case BR_CAPS_APP_VERSION:return "app_version_unsupported";
    case BR_CAPS_APP_MISSING:return "app_not_installed";
    case BR_CAPS_OFFLINE:return "bridge_unreachable";
    default:return "unsupported";
    }
}

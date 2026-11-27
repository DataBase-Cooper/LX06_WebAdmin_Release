#include "describe.h"
#include <string.h>
#define KIND(d,n,k) (br_json_kind((d),(n))==(k))
static unsigned field(const br_json *d,unsigned n,const char *key){return br_json_field(d,n,key);}
static int text(const br_json *d,unsigned n,int required){
    return KIND(d,n,BR_J_STRING)&&(!required||br_json_text(d,n).length);
}
static int names(const br_json *d,unsigned list,const char *key){
    if(!KIND(d,list,BR_J_ARRAY))return 0;
    for(unsigned i=br_json_child(d,list);i&&i<br_json_next(d,list);i=br_json_next(d,i)){
        unsigned name=field(d,i,key);
        if(!KIND(d,i,BR_J_OBJECT)||!text(d,name,1))return 0;
        for(unsigned j=br_json_child(d,list);j<i;j=br_json_next(d,j))
            if(br_json_equal(d,name,d,field(d,j,key)))return 0;
    }
    return 1;
}
static int digest(const br_json *d,unsigned n){
    if(!text(d,n,1))return 0;
    br_text t=br_json_text(d,n);if(t.length!=64)return 0;
    for(size_t i=0;i<t.length;i++)
        if(!((t.data[i]>='0'&&t.data[i]<='9')||(t.data[i]>='a'&&t.data[i]<='f')))return 0;
    return 1;
}
static int shape(const br_json *d){
    if(!KIND(d,1,BR_J_OBJECT)||!br_json_is(d,field(d,1,"kind"),"state"))return 0;
    for(unsigned i=1;i<=d->count;i++)
        if(br_json_key_ci(d,i,"token")||br_json_key_ci(d,i,"authorization")||br_json_key_ci(d,i,"phone_token"))return 0;
    unsigned body=field(d,1,"json"),version=field(d,body,"doc_version"),apps=field(d,body,"apps");
    uint64_t v;
    if(!KIND(d,body,BR_J_OBJECT)||!br_json_u64(d,version,&v)||v!=1||!names(d,apps,"app"))return 0;
    for(unsigned a=br_json_child(d,apps);a&&a<br_json_next(d,apps);a=br_json_next(d,a)){
        unsigned ops=field(d,a,"ops"),notes=field(d,a,"notes");
        if(br_json_text(d,a).length>4096||!text(d,field(d,a,"hook_version"),1)||
           !digest(d,field(d,a,"doc_sha256"))||!text(d,field(d,a,"summary"),0)||
           (notes&&!text(d,notes,0))||!names(d,ops,"name"))return 0;
        for(unsigned o=br_json_child(d,ops);o&&o<br_json_next(d,ops);o=br_json_next(d,o)){
            unsigned params=field(d,o,"params"),examples=field(d,o,"examples");
            if(!text(d,field(d,o,"desc"),0)||!text(d,field(d,o,"returns_text"),0)||
               !KIND(d,params,BR_J_OBJECT)||!KIND(d,examples,BR_J_ARRAY))return 0;
            for(unsigned k=br_json_child(d,params);k&&k<br_json_next(d,params);k=br_json_next(d,k+1)){
                unsigned p=k+1;
                if(!br_json_text(d,k).length||!KIND(d,p,BR_J_OBJECT)||
                   !text(d,field(d,p,"desc"),0)||!field(d,p,"example"))return 0;
            }
            for(unsigned e=br_json_child(d,examples);e&&e<br_json_next(d,examples);e=br_json_next(d,e))
                if(!text(d,e,0))return 0;
        }
    }
    return 1;
}
int br_describe_validate(br_json *d,unsigned frame,const void *data,size_t length){
    if(!d)return -1;
    if(frame==BR_WIRE_READY&&br_json_parse(d,data,length)==1&&shape(d))return 1;
    memset(d,0,sizeof(*d));return -1;
}

#define _GNU_SOURCE
#include "caps_json.h"
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SPAN_MASK 0x3fffu
_Static_assert(sizeof(br_json_token)==6,"compact naturally aligned JSON token");
_Static_assert(LX06_BRIDGE_PAYLOAD_MAX<=SPAN_MASK,"JSON spans fit without truncation");
_Static_assert(BR_JSON_TOKENS+1u<=UINT16_MAX,"JSON next sentinel fits");
_Static_assert(BR_J_NULL<16,"JSON kind fits four bits");
static unsigned token_kind(const br_json_token *t){
    return (t->offset_kind>>14)|((t->length_kind>>14)<<2);
}
static void token_length(br_json_token *t,size_t length){
    t->length_kind=(uint16_t)((t->length_kind&~SPAN_MASK)|(unsigned)length);
}

typedef struct { br_json *d; size_t pos; } parser;
static void space(parser *p){
    while(p->pos<p->d->length){
        unsigned char c=p->d->raw[p->pos];
        if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n')break;
        p->pos++;
    }
}
int br_json_kind(const br_json *d,unsigned n){return n&&n<=d->count?(int)token_kind(&d->tokens[n]):0;}
unsigned br_json_next(const br_json *d,unsigned n){return n&&n<=d->count?d->tokens[n].next:0;}
unsigned br_json_child(const br_json *d,unsigned n){
    int k=br_json_kind(d,n);
    return (k==BR_J_ARRAY||k==BR_J_OBJECT)&&n+1<d->tokens[n].next?n+1:0;
}
br_text br_json_text(const br_json *d,unsigned n){
    if(!n||n>d->count)return (br_text){NULL,0};
    const br_json_token *t=&d->tokens[n];
    unsigned kind=token_kind(t);
    return (br_text){(kind==BR_J_STRING||kind==BR_J_KEY?d->strings:d->raw)+
        (t->offset_kind&SPAN_MASK),t->length_kind&SPAN_MASK};
}
int br_json_is(const br_json *d,unsigned n,const char *s){
    br_text t=br_json_text(d,n);
    return (br_json_kind(d,n)==BR_J_STRING||br_json_kind(d,n)==BR_J_KEY)&&
           t.length==strlen(s)&&!memcmp(t.data,s,t.length);
}
int br_json_key_ci(const br_json *d,unsigned n,const char *s){
    br_text t=br_json_text(d,n);
    if(br_json_kind(d,n)!=BR_J_KEY||t.length!=strlen(s))return 0;
    for(size_t i=0;i<t.length;i++){
        unsigned c=t.data[i];if(c>='A'&&c<='Z')c+='a'-'A';
        if(c!=(unsigned char)s[i])return 0;
    }
    return 1;
}
unsigned br_json_field(const br_json *d,unsigned n,const char *key){
    if(br_json_kind(d,n)!=BR_J_OBJECT)return 0;
    for(unsigned k=br_json_child(d,n);k&&k<d->tokens[n].next;k=d->tokens[k+1].next)
        if(br_json_is(d,k,key))return k+1;
    return 0;
}
static unsigned add(parser *p,int kind,size_t offset,size_t length){
    if(p->d->count==BR_JSON_TOKENS)return 0;
    unsigned n=++p->d->count;
    p->d->tokens[n]=(br_json_token){
        (uint16_t)((unsigned)offset|(((unsigned)kind&3u)<<14)),
        (uint16_t)((unsigned)length|(((unsigned)kind>>2)<<14)),(uint16_t)(n+1)};
    return n;
}
static int hex(unsigned char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static int hex4(parser *p,uint32_t *out){
    if(p->pos+4>p->d->length)return 0;
    uint32_t cp=0;
    for(unsigned i=0;i<4;i++){
        int v=hex(p->d->raw[p->pos++]);if(v<0)return 0;
        cp=cp*16+(unsigned)v;
    }
    *out=cp;return 1;
}
static int emit(parser *p,uint32_t cp){
    unsigned char b[4];unsigned n=0;
    if(cp<=0x7f)b[n++]=(unsigned char)cp;
    else if(cp<=0x7ff){b[n++]=(unsigned char)(0xc0|(cp>>6));b[n++]=(unsigned char)(0x80|(cp&63));}
    else if(cp<=0xffff){b[n++]=(unsigned char)(0xe0|(cp>>12));b[n++]=(unsigned char)(0x80|((cp>>6)&63));b[n++]=(unsigned char)(0x80|(cp&63));}
    else{b[n++]=(unsigned char)(0xf0|(cp>>18));b[n++]=(unsigned char)(0x80|((cp>>12)&63));b[n++]=(unsigned char)(0x80|((cp>>6)&63));b[n++]=(unsigned char)(0x80|(cp&63));}
    if(p->d->used+n>LX06_BRIDGE_PAYLOAD_MAX)return 0;
    memcpy(p->d->strings+p->d->used,b,n);p->d->used+=(uint16_t)n;return 1;
}
static unsigned string(parser *p,int key){
    if(p->pos>=p->d->length||p->d->raw[p->pos++]!='"')return 0;
    size_t start=p->d->used;
    while(p->pos<p->d->length){
        uint32_t cp=p->d->raw[p->pos++];
        if(cp=='"')return add(p,key?BR_J_KEY:BR_J_STRING,start,p->d->used-start);
        if(cp<32)return 0;
        if(cp=='\\'){
            if(p->pos==p->d->length)return 0;
            cp=p->d->raw[p->pos++];
            if(cp=='u'){
                if(!hex4(p,&cp))return 0;
                if(cp>=0xd800&&cp<=0xdbff){
                    uint32_t low;
                    if(p->pos+2>p->d->length||p->d->raw[p->pos++]!='\\'||p->d->raw[p->pos++]!='u'||
                       !hex4(p,&low)||low<0xdc00||low>0xdfff)return 0;
                    cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
                }else if(cp>=0xdc00&&cp<=0xdfff)return 0;
            }else if(cp=='b')cp=8;
            else if(cp=='f')cp=12;
            else if(cp=='n')cp=10;
            else if(cp=='r')cp=13;
            else if(cp=='t')cp=9;
            else if(cp!='"'&&cp!='\\'&&cp!='/')return 0;
        }else if(cp>=128){
            unsigned more;uint32_t min;
            if(cp>=0xc2&&cp<=0xdf){more=1;min=0x80;cp&=31;}
            else if(cp>=0xe0&&cp<=0xef){more=2;min=0x800;cp&=15;}
            else if(cp>=0xf0&&cp<=0xf4){more=3;min=0x10000;cp&=7;}
            else return 0;
            if(p->pos+more>p->d->length)return 0;
            while(more--){unsigned c=p->d->raw[p->pos++];if((c&0xc0)!=0x80)return 0;cp=(cp<<6)|(c&63);}
            if(cp<min||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
        }
        if(!emit(p,cp))return 0;
    }
    return 0;
}
static int digit(unsigned char c){return c>='0'&&c<='9';}
static unsigned value(parser *,unsigned);
static unsigned container(parser *p,unsigned depth,int object){
    size_t start=p->pos;
    unsigned n=add(p,object?BR_J_OBJECT:BR_J_ARRAY,start,0);
    if(!n)return 0;
    p->pos++;space(p);
    unsigned char close=object?'}':']';
    if(p->pos<p->d->length&&p->d->raw[p->pos]==close){
        p->pos++;token_length(&p->d->tokens[n],p->pos-start);return n;
    }
    for(;;){
        space(p);
        if(object){
            unsigned k=string(p,1);if(!k)return 0;
            /* Compare decoded keys, so "x" and "\u0078" collide. */
            br_text key=br_json_text(p->d,k);
            for(unsigned prev=n+1;prev<k;prev=p->d->tokens[prev+1].next){
                br_text a=br_json_text(p->d,prev);
                if(a.length==key.length&&!memcmp(a.data,key.data,key.length))return 0;
            }
            space(p);if(p->pos==p->d->length||p->d->raw[p->pos++]!=':')return 0;
        }
        if(!value(p,depth+1))return 0;
        space(p);if(p->pos==p->d->length)return 0;
        unsigned c=p->d->raw[p->pos++];
        if(c==close){
            p->d->tokens[n].next=(uint16_t)(p->d->count+1);
            token_length(&p->d->tokens[n],p->pos-start);return n;
        }
        if(c!=',')return 0;
    }
}
static unsigned value(parser *p,unsigned depth){
    if(depth>BR_JSON_DEPTH)return 0;
    space(p);if(p->pos==p->d->length)return 0;
    unsigned c=p->d->raw[p->pos];size_t start=p->pos;
    if(c=='"')return string(p,0);
    if(c=='{'||c=='[')return container(p,depth,c=='{');
    const char *literal=c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;
    if(literal){
        size_t len=strlen(literal);
        if(p->pos+len>p->d->length||memcmp(p->d->raw+p->pos,literal,len))return 0;
        p->pos+=len;return add(p,c=='t'?BR_J_TRUE:c=='f'?BR_J_FALSE:BR_J_NULL,start,len);
    }
    if(c=='-')p->pos++;
    if(p->pos==p->d->length||!digit(p->d->raw[p->pos]))return 0;
    if(p->d->raw[p->pos]=='0')p->pos++;
    else while(p->pos<p->d->length&&digit(p->d->raw[p->pos]))p->pos++;
    int kind=BR_J_INT;
    if(p->pos<p->d->length&&p->d->raw[p->pos]=='.'){
        kind=BR_J_NUMBER;p->pos++;size_t first=p->pos;
        while(p->pos<p->d->length&&digit(p->d->raw[p->pos]))p->pos++;
        if(first==p->pos)return 0;
    }
    if(p->pos<p->d->length&&(p->d->raw[p->pos]=='e'||p->d->raw[p->pos]=='E')){
        kind=BR_J_NUMBER;p->pos++;
        if(p->pos<p->d->length&&(p->d->raw[p->pos]=='+'||p->d->raw[p->pos]=='-'))p->pos++;
        size_t first=p->pos;
        while(p->pos<p->d->length&&digit(p->d->raw[p->pos]))p->pos++;
        if(first==p->pos)return 0;
    }
    if(kind==BR_J_NUMBER){
        /* Diagnostic extensions may contain finite floats. Never change the
         * process locale or interpret enum/int parameters as floats. */
        locale_t loc=newlocale(LC_NUMERIC_MASK,"C",(locale_t)0);
        if(!loc)return 0;
        unsigned char saved=p->d->raw[p->pos];p->d->raw[p->pos]=0;
        char *number=(char *)p->d->raw+start,*end;
        double parsed=strtod_l(number,&end,loc);
        p->d->raw[p->pos]=saved;
        int ok=isfinite(parsed)&&end==(char *)p->d->raw+p->pos;
        freelocale(loc);if(!ok)return 0;
    }
    return add(p,kind,start,p->pos-start);
}
int br_json_parse(br_json *d,const void *data,size_t length){
    if(!d||!data||!length||length>LX06_BRIDGE_PAYLOAD_MAX)return -1;
    memset(d,0,sizeof(*d));memcpy(d->raw,data,length);d->length=(uint16_t)length;
    parser p={d,0};unsigned root=value(&p,0);space(&p);
    return root==1&&p.pos==length?1:-1;
}
static br_text magnitude(br_text t,int *negative){
    *negative=t.data[0]=='-';if(*negative){t.data++;t.length--;}
    if(t.length==1&&t.data[0]=='0')*negative=0;
    return t;
}
int br_json_int_cmp(const br_json *a,unsigned x,const br_json *b,unsigned y){
    int an,bn; br_text at=magnitude(br_json_text(a,x),&an),bt=magnitude(br_json_text(b,y),&bn);
    if(an!=bn)return an?-1:1;
    int cmp=at.length<bt.length?-1:at.length>bt.length?1:memcmp(at.data,bt.data,at.length);
    cmp=cmp<0?-1:cmp>0?1:0;return an?-cmp:cmp;
}
int br_json_equal(const br_json *a,unsigned x,const br_json *b,unsigned y){
    int kind=br_json_kind(a,x);
    if(!kind||kind!=br_json_kind(b,y))return 0;
    if(kind==BR_J_OBJECT||kind==BR_J_ARRAY)return 0; /* scalar equality only */
    if(kind==BR_J_INT)return br_json_int_cmp(a,x,b,y)==0;
    br_text at=br_json_text(a,x),bt=br_json_text(b,y);
    return at.length==bt.length&&!memcmp(at.data,bt.data,at.length);
}
int br_json_u64(const br_json *d,unsigned n,uint64_t *out){
    if(br_json_kind(d,n)!=BR_J_INT||!out)return 0;
    int negative;br_text t=magnitude(br_json_text(d,n),&negative);if(negative)return 0;
    uint64_t number=0;
    for(size_t i=0;i<t.length;i++){
        unsigned digit_value=t.data[i]-'0';
        if(number>(UINT64_MAX-digit_value)/10)return 0;
        number=number*10+digit_value;
    }
    *out=number;return 1;
}
size_t br_json_characters(const br_json *d,unsigned n){
    br_text t=br_json_text(d,n);size_t count=0;
    for(size_t i=0;i<t.length;i++)if((t.data[i]&0xc0)!=0x80)count++;
    return count;
}

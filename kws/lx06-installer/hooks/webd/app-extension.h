/* Script UI declarations are DATA, never shell-sourced. Shared by Web and the
 * isolated action runner. Keep limits small on the 116 MiB LX06. */
#ifndef LX06_APP_EXTENSION_H
#define LX06_APP_EXTENSION_H
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
/* Reuse the shipped tokenizer without altering existing voice clients. The
 * stricter checks below are local to the extension protocol. */
#ifdef LX06_APP_EXTENSION_JSON_SHARED
#include "../../runtime/doubao/src/json.h"
#define decode_string xz_json_decode_token
#define hex4 xz_json_hex4
#else
#include "../../runtime/doubao/src/json.c"
#endif
#ifdef LX06_REV12_EXPRESSION_FIXTURE
#include "template-expression.h"
#endif
#define EXT_CAP 8192
#define EXT_FIELDS 24
#define EXT_ACTIONS 12
#define EXT_EVENT_CAP 8192
#define EXT_DYNAMIC_OPTIONS 64
#define EXT_OPTION_VALUE 128
#define EXT_OPTION_LABEL 160
struct ext_field { char name[33], type[16]; int token, dynamic; };
struct ext_action { char id[33]; int token; };
/* Optional structured local-model adapter. Ordinary declared string values
 * remain the only carrier; this grants no action, account or execution right. */
struct ext_structured {
    int present, request, prompt, response, invocation, status;
    unsigned actions;
    char status_action[33], logged_in[65];
};
struct ext_schema {
    struct ext_structured structured;
    char json[EXT_CAP+1]; xz_json_doc doc;
    struct ext_field fields[EXT_FIELDS]; struct ext_action actions[EXT_ACTIONS];
    int nf, na, registry_valid;
    /* Saved context belongs to the settings owner, never to POST values. */
    char saved_values[EXT_FIELDS][2049]; unsigned saved_mask;
    /* Only successful, schema-bound script output may populate this runtime
     * catalogue. Public values may retain an old ID; requests cannot invent it. */
    char options_json[EXT_EVENT_CAP+1], current_json[EXT_EVENT_CAP+1];
    xz_json_doc options_doc;
    char current[EXT_FIELDS][EXT_OPTION_VALUE+1]; unsigned current_mask;
};
static int ext_ident(const char *s) {
    size_t n=strlen(s);
    if (!n || n>32 || !((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z'))) return 0;
    for (;*s;s++) if (!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-')) return 0;
    return 1;
}
static int ext_raw_eq(const xz_json_doc *d,int t,const char *s) {
    return t>=0 && d->tokens[t].end-d->tokens[t].start==strlen(s) &&
        !memcmp(d->source+d->tokens[t].start,s,strlen(s));
}
static int ext_buf(char*out,size_t cap,size_t*used,const char*s,size_t n) {
    if(n>=cap-*used)return 0;memcpy(out+*used,s,n);*used+=n;out[*used]=0;return 1;
}
static int ext_quote(char*out,size_t cap,size_t*used,const char*s) {
    if(!ext_buf(out,cap,used,"\"",1))return 0;
    for(;*s;s++) {
        char b[8];unsigned char c=(unsigned char)*s;size_t n=1;b[0]=c;
        if(c=='"'||c=='\\'){b[0]='\\';b[1]=c;n=2;}
        else if(c<32){snprintf(b,sizeof(b),"\\u%04x",c);n=6;}
        if(!ext_buf(out,cap,used,b,n))return 0;
    }return ext_buf(out,cap,used,"\"",1);
}
static int ext_keys(const xz_json_doc *d,int o,const char *allowed) {
    char key[64], tag[68];
    if(o<0 || d->tokens[o].type!=XZ_JSON_OBJECT) return 0;
    for(int k=d->tokens[o].first_child;k>=0;) {
        int v=d->tokens[k].next_sibling;
        if(v<0 || decode_string(d,k,key,sizeof(key))) return 0;
        /* ASCII literal keys only: no escaped aliases, NUL or duplicates. */
        /* Delimiters belong to the allowed-name table, never to a key. An
         * empty key or joined names must not match its double separators. */
        if(!*key||strchr(key,'|')||!ext_raw_eq(d,k,key)) return 0;
        snprintf(tag,sizeof(tag),"|%s|",key);
        if(!strstr(allowed,tag)) return 0;
        for(int p=d->tokens[o].first_child;p!=k;) {
            if(ext_raw_eq(d,p,key)) return 0;
            p=d->tokens[d->tokens[p].next_sibling].next_sibling;
        }
        k=d->tokens[v].next_sibling;
    }
    return 1;
}
static int ext_text(const xz_json_doc*d,int o,const char*k,char*b,size_t n,int required) {
    int t=xz_json_object_get(d,o,k);
    if(t<0) { b[0]=0; return !required; }
    if(xz_json_get_string(d,o,k,b,n)) return 0;
    /* Reject embedded NUL and lone surrogates rather than disagree with JS. */
    const xz_json_token *a=&d->tokens[t];
    for(size_t i=a->start;i+5<a->end;i++) if(d->source[i]=='\\') {
        if(d->source[i+1]=='u') {
            unsigned cp=0; if(hex4(d->source+i+2,&cp)||!cp||(cp>=0xd800&&cp<=0xdfff))return 0;
            i+=5;
        } else i++;
    }
    return !required || b[0];
}
static int ext_field_index(const struct ext_schema*s,const char*n) {
    for(int i=0;i<s->nf;i++) if(!strcmp(s->fields[i].name,n))return i;
    return -1;
}
/* Finite JSON decimal metadata, shared by Web request and settings validation.
 * No locale-dependent text, NaN/Infinity, strings or executable expressions. */
static int ext_decimal(const char*v,double*out) {
    if(!*v||strlen(v)>64)return 0;
    for(const char*p=v;*p;p++)if(!strchr("0123456789+-.eE",*p))return 0;
    char *end;errno=0;double x=strtod(v,&end);
    if(errno||*end||!isfinite(x))return 0;
    *out=x;return 1;
}
static int ext_number(const xz_json_doc*d,int o,const char*k,double*out) {
    int t=xz_json_object_get(d,o,k);char raw[65];
    if(t<0||d->tokens[t].type!=XZ_JSON_PRIMITIVE)return 0;
    size_t n=d->tokens[t].end-d->tokens[t].start;
    if(!n||n>=sizeof(raw))return 0;
    memcpy(raw,d->source+d->tokens[t].start,n);raw[n]=0;return ext_decimal(raw,out);
}
static int ext_rows_value(const struct ext_schema*,int,const char*);
static int ext_effective_value(const struct ext_schema*,int,char[2049]);
/* The shipped tokenizer accepts an object root. Wrap the encoded array in a
 * local object; it is still parsed once by that tokenizer, never by a new one. */
static int ext_rows_parse(const char*value,char wrapped[2060],xz_json_doc*d) {
    if(strlen(value)>2048)return -1;
    snprintf(wrapped,2060,"{\"rows\":%s}",value);
    if(xz_json_parse(d,wrapped,strlen(wrapped)))return -1;
    int a=xz_json_object_get(d,0,"rows");
    return a>=0&&d->tokens[a].type==XZ_JSON_ARRAY?a:-1;
}
static int ext_value(const struct ext_schema*s,int f,const char*v) {
    const struct ext_field *a=&s->fields[f];
    if(strlen(v)>2048)return 0;
    /* Single-line controls must not smuggle CR/LF into HTTP credentials/URLs.
     * Multiline text is explicit and consumers must JSON-escape its contents. */
    if(strcmp(a->type,"textarea"))for(const unsigned char*p=(const unsigned char*)v;*p;p++)if(*p<32||*p==127)return 0;
    int maxbytes;
    /* Large explicitly declared transport carriers never enter ordinary
     * saved values. Only the directional transient paths below admit them. */
    if(!xz_json_get_int(&s->doc,a->token,"maxBytes",&maxbytes)&&
       (maxbytes>2048||strlen(v)>(size_t)maxbytes))return 0;
    if(!strcmp(a->type,"checkbox"))return !strcmp(v,"true")||!strcmp(v,"false");
    if(!strcmp(a->type,"number")) {
        char widget[17];if(!ext_text(&s->doc,a->token,"widget",widget,sizeof(widget),0))return 0;
        if(!*v)return strcmp(widget,"slider")!=0;
        double x,low=0,high,step;
        if(!ext_decimal(v,&x))return 0;
        if(ext_number(&s->doc,a->token,"min",&low)&&x<low)return 0;
        if(ext_number(&s->doc,a->token,"max",&high)&&x>high)return 0;
        if(ext_number(&s->doc,a->token,"step",&step)) {
            double q=(x-low)/step;
            if(!isfinite(q)||fabs(q)>9e12)return 0;
            double nearest=(double)(long long)(q+(q>=0?.5:-.5));
            if(fabs(q-nearest)>1e-6)return 0;
        }
    }
    char format[24];
    if(!ext_text(&s->doc,a->token,"format",format,sizeof(format),0))return 0;
    if(!strcmp(format,"integer")) {
        if(!*v)return 0;
        for(const char*p=v;*p;p++)if(*p<'0'||*p>'9')return 0;
    }
    if(!strcmp(format,"wss") && (strncmp(v,"wss://",6)||!v[6]||strpbrk(v,"@ \t\r\n\\")))return 0;
    if(!strcmp(format,"words")||!strcmp(format,"aliases")) {
        if(strlen(v)>768||strpbrk(v,"\r\n")||(!*v&&!strcmp(format,"words")))return 0;
        const char *p=v;size_t n=0;int meaningful=0;
        for(;*p;p++) {
            if(*p==','||!strncmp(p,"，",3)) {
                if(!meaningful)return 0;
                n=0;meaningful=0;if(*p!=',')p+=2;
            } else {
                if(++n>120||(unsigned char)*p<32)return 0;
                if((unsigned char)*p>=128||isalnum((unsigned char)*p))meaningful=1;
            }
        }
        if(*v&&!meaningful)return 0;
    }
    if(!strcmp(a->type,"select")) {
        if(a->dynamic)return *v&&strlen(v)<=EXT_OPTION_VALUE;
        int o=xz_json_object_get(&s->doc,a->token,"options");
        for(int t=s->doc.tokens[o].first_child;t>=0;t=s->doc.tokens[t].next_sibling) {
            char b[257]; if(!decode_string(&s->doc,t,b,sizeof(b))&&!strcmp(b,v))return 1;
        }
        return 0;
    }
    return !s->registry_valid||ext_rows_value(s,f,v);
}
/* A script owns the workflow; firmware only validates bounded references.
 * These steps are presentation, never an authorization or execution policy. */
static int ext_action_index(const struct ext_schema*s,const char*n) {
    for(int i=0;i<s->na;i++)if(!strcmp(s->actions[i].id,n))return i;
    return -1;
}
static int ext_step_index(const struct ext_schema*s,const char*n) {
    int flow=xz_json_object_get(&s->doc,0,"flow"),steps=xz_json_object_get(&s->doc,flow,"steps");
    if(steps<0)return -1;
    char b[33];int i=0;
    for(int t=s->doc.tokens[steps].first_child;t>=0;t=s->doc.tokens[t].next_sibling,i++)
        if(ext_text(&s->doc,t,"id",b,sizeof(b),1)&&!strcmp(b,n))return i;
    return -1;
}
static int ext_flow_valid(const struct ext_schema*s) {
    const xz_json_doc*d=&s->doc;int flow=xz_json_object_get(d,0,"flow");
    if(flow<0)return 1;
    if(!ext_keys(d,flow,"|initial||steps|"))return 0;
    char b[161];int steps=xz_json_object_get(d,flow,"steps"),count=0;
    if(steps<0||d->tokens[steps].type!=XZ_JSON_ARRAY)return 0;
    for(int t=d->tokens[steps].first_child;t>=0;t=d->tokens[t].next_sibling) {
        if(++count>6||!ext_keys(d,t,"|id||title||description||fields||actions||watch||interval||back|"))return 0;
        if(!ext_text(d,t,"id",b,33,1)||!ext_ident(b)||ext_step_index(s,b)!=count-1||
           !ext_text(d,t,"title",b,161,1)||!ext_text(d,t,"description",b,sizeof(b),0))return 0;
        for(int kind=0;kind<2;kind++) {
            int a=xz_json_object_get(d,t,kind?"actions":"fields"),mask=0;
            if(a<0||d->tokens[a].type!=XZ_JSON_ARRAY)return 0;
            for(int v=d->tokens[a].first_child;v>=0;v=d->tokens[v].next_sibling) {
                if(decode_string(d,v,b,33))return 0;
                int i=kind?ext_action_index(s,b):ext_field_index(s,b);
                if(i<0||(mask&(1<<i)))return 0;mask|=1<<i;
            }
        }
        if(!ext_text(d,t,"watch",b,33,0))return 0;
        if(*b) {
            int i=ext_action_index(s,b),seconds=0;
            if(i<0||d->tokens[xz_json_object_get(d,s->actions[i].token,"fields")].first_child>=0||
               xz_json_get_int(d,t,"interval",&seconds)||seconds<3||seconds>30)return 0;
        } else if(xz_json_object_get(d,t,"interval")>=0)return 0;
        if(!ext_text(d,t,"back",b,33,0)||(*b&&ext_step_index(s,b)<0))return 0;
    }
    return count&&ext_text(d,flow,"initial",b,33,1)&&ext_step_index(s,b)>=0;
}
static int ext_registry_valid(struct ext_schema*);
static int ext_registry_semantics(struct ext_schema*);
static int ext_repeat(const struct ext_schema*,int);
static int ext_structured_valid(struct ext_schema *s) {
    xz_json_doc *d=&s->doc;struct ext_structured *m=&s->structured;
    memset(m,0,sizeof(*m));int o=xz_json_object_get(d,0,"structured");
    if(o<0)return 1;
    if(!ext_keys(d,o,"|version||request_field||prompt_field||response_field||invocation_field||status_action||status_field||logged_in_value||actions|")||
       !ext_raw_eq(d,xz_json_object_get(d,o,"version"),"1")||
       d->tokens[xz_json_object_get(d,o,"version")].type!=XZ_JSON_PRIMITIVE)return 0;
    const char *names[]={"request_field","prompt_field","response_field","invocation_field","status_field"};
    int *fields[]={&m->request,&m->prompt,&m->response,&m->invocation,&m->status};
    unsigned mask=0;char value[65];
    for(int i=0;i<5;i++) {
        if(!ext_text(d,o,names[i],value,33,1))return 0;
        int f=ext_field_index(s,value);
        if(f<0||(mask&(1U<<f))||(strcmp(s->fields[f].type,"text")&&strcmp(s->fields[f].type,"textarea")))return 0;
        /* Transport/status fields cannot silently inherit saved credential or
         * account defaults. Actual success events are the only observations. */
        int token=s->fields[f].token;
        if(xz_json_object_get(d,token,"env")>=0||xz_json_object_get(d,token,"default")>=0)return 0;
        *fields[i]=f;mask|=1U<<f;
    }
    if(!ext_text(d,o,"status_action",m->status_action,sizeof(m->status_action),1)||
       !ext_text(d,o,"logged_in_value",m->logged_in,sizeof(m->logged_in),1)||
       !ext_value(s,m->status,m->logged_in)||!strcmp(m->status_action,"intent")||
       !strcmp(m->status_action,"extract")||!strcmp(m->status_action,"understand"))return 0;
    int ar=xz_json_object_get(d,o,"actions");
    if(ar<0||d->tokens[ar].type!=XZ_JSON_ARRAY)return 0;
    const char *stages[]={"intent","extract","understand"};
    for(int t=d->tokens[ar].first_child;t>=0;t=d->tokens[t].next_sibling) {
        if(decode_string(d,t,value,sizeof(value)))return 0;
        int i=0;while(i<3&&strcmp(value,stages[i]))i++;
        if(i==3||(m->actions&(1U<<i)))return 0;m->actions|=1U<<i;
    }
    if(!m->actions)return 0;
    unsigned found=0;int status=0;
    for(int a=0;a<s->na;a++) {
        unsigned required=0;int stage=-1;
        if(!strcmp(s->actions[a].id,m->status_action)) {required=1U<<m->invocation;status=1;}
        else for(int i=0;i<3;i++)if(!strcmp(s->actions[a].id,stages[i])&&(m->actions&(1U<<i))) {
            required=(1U<<m->invocation)|(1U<<m->request)|(1U<<m->prompt);stage=i;
        }
        if(!required)continue;
        int fs=xz_json_object_get(d,s->actions[a].token,"fields");unsigned got=0;
        for(int t=d->tokens[fs].first_child;t>=0;t=d->tokens[t].next_sibling) {
            if(decode_string(d,t,value,33))return 0;
            int f=ext_field_index(s,value);if(f<0)return 0;got|=1U<<f;
        }
        if(got!=required)return 0;
        if(stage>=0)found|=1U<<stage;
    }
    if(!status||found!=m->actions)return 0;m->present=1;return 1;
}
/* Larger values are opt-in transport carriers, never a larger saved
 * record, repeat row, default, environment variable or dynamic cache. Keep the
 * original public fields and outer request/event byte budgets unchanged. */
static int ext_transport_capacity(const struct ext_schema *s,int f) {
    int cap=0;
    if(!s->structured.present||(f!=s->structured.request&&f!=s->structured.response)||
       xz_json_get_int(&s->doc,s->fields[f].token,"maxBytes",&cap)||cap<=2048)return 0;
    return cap;
}
static int ext_transport_schema_valid(const struct ext_schema *s) {
    for(int f=0;f<s->nf;f++) {
        int cap=0,token=s->fields[f].token;
        if(xz_json_get_int(&s->doc,token,"maxBytes",&cap)||cap<=2048)continue;
        if(cap>EXT_CAP||!ext_transport_capacity(s,f)||strcmp(s->fields[f].type,"textarea")||
           s->fields[f].dynamic||ext_repeat(s,f)>=0||
           xz_json_object_get(&s->doc,token,"format")>=0||
           xz_json_object_get(&s->doc,token,"default")>=0||
           xz_json_object_get(&s->doc,token,"env")>=0)return 0;
    }
    return 1;
}
static int ext_transport_value(const struct ext_schema *s,int f,const char *value) {
    int cap=ext_transport_capacity(s,f);
    return cap?cap<=EXT_CAP&&strnlen(value,EXT_CAP+1)<=(size_t)cap:ext_value(s,f,value);
}
static int ext_transport_text(const xz_json_doc *d,int o,const char *key,const struct ext_schema *s,int f) {
    char value[EXT_CAP+1];
    return ext_text(d,o,key,value,sizeof(value),0)&&ext_transport_value(s,f,value);
}
#include "app-extension-model.h"
#include "app-extension-startup.h"
static int ext_parse(struct ext_schema*s) {
    char b[2049]; int v, ar;
    xz_json_doc*d=&s->doc; s->nf=s->na=s->registry_valid=0;s->saved_mask=0;s->options_json[0]=s->current_json[0]=0;s->current_mask=0;
    if(xz_json_parse(d,s->json,strlen(s->json)) ||
       !ext_keys(d,0,"|version||title||description||fields||actions||load||flow||row_types||repeat_inputs||conditions||visibility||default_bindings||list_slots||structured||model_intents||startup_bindings|") ||
       !ext_raw_eq(d,xz_json_object_get(d,0,"version"),"1") ||
       d->tokens[xz_json_object_get(d,0,"version")].type!=XZ_JSON_PRIMITIVE ||
       !ext_text(d,0,"title",b,161,1)||!ext_text(d,0,"description",b,1025,0))return -1;
    ar=xz_json_object_get(d,0,"fields");
    if(ar<0||d->tokens[ar].type!=XZ_JSON_ARRAY)return -1;
    for(v=d->tokens[ar].first_child;v>=0;v=d->tokens[v].next_sibling) {
        if(s->nf>=EXT_FIELDS||!ext_keys(d,v,"|name||label||type||default||placeholder||options||optionLabels||dynamic||group||env||format||min||max||step||precision||center||unit||widget||maxBytes|"))return -1;
        struct ext_field*f=&s->fields[s->nf]; f->token=v;f->dynamic=0;
        if(!ext_text(d,v,"name",f->name,sizeof(f->name),1)||!ext_ident(f->name)||ext_field_index(s,f->name)>=0||
           !ext_text(d,v,"label",b,161,1)||!ext_text(d,v,"type",f->type,sizeof(f->type),1)||
           !ext_text(d,v,"placeholder",b,257,0)||!ext_text(d,v,"group",b,81,0))return -1;
        if(!ext_text(d,v,"env",b,65,0))return -1;
        if(*b) { for(char*p=b;*p;p++)if(!((*p>='A'&&*p<='Z')||*p=='_'||(p>b&&isdigit((unsigned char)*p))))return -1; }
        if(!ext_text(d,v,"format",b,24,0)||(*b&&strcmp(b,"integer")&&strcmp(b,"words")&&strcmp(b,"aliases")&&strcmp(b,"wss")))return -1;
        double lo=0,hi=0,increment=0,center=0;int precision=0;
        int haslo=xz_json_object_get(d,v,"min")>=0,hashi=xz_json_object_get(d,v,"max")>=0;
        int hasstep=xz_json_object_get(d,v,"step")>=0,hascenter=xz_json_object_get(d,v,"center")>=0;
        if((haslo&&(strcmp(f->type,"number")||!ext_number(d,v,"min",&lo)))||
           (hashi&&(strcmp(f->type,"number")||!ext_number(d,v,"max",&hi)))||(haslo&&hashi&&lo>hi))return -1;
        char widget[17],unit[33];
        if(!ext_text(d,v,"widget",widget,sizeof(widget),0)||(*widget&&strcmp(widget,"slider")&&strcmp(widget,"switch")&&strcmp(widget,"segments"))||
           !ext_text(d,v,"unit",unit,sizeof(unit),0))return -1;
        if(hasstep&&(strcmp(f->type,"number")||!ext_number(d,v,"step",&increment)||increment<=0))return -1;
        if(xz_json_object_get(d,v,"precision")>=0&&(strcmp(f->type,"number")||xz_json_get_int(d,v,"precision",&precision)||precision<0||precision>4))return -1;
        if(hascenter&&(strcmp(widget,"slider")||!ext_number(d,v,"center",&center)||center<lo||center>hi))return -1;
        if(!strcmp(widget,"slider")&&(strcmp(f->type,"number")||!haslo||!hashi||!hasstep||lo>=hi||(hi-lo)/increment>1e7))return -1;
        if(!strcmp(widget,"switch")&&strcmp(f->type,"checkbox")&&strcmp(f->type,"select"))return -1;
        if(!strcmp(widget,"segments")&&strcmp(f->type,"select"))return -1;
        int bytes;
        if(xz_json_object_get(d,v,"maxBytes")>=0&&(xz_json_get_int(d,v,"maxBytes",&bytes)||bytes<1||bytes>EXT_CAP||(bytes>2048&&strcmp(f->type,"textarea"))))return -1;
        if(strcmp(f->type,"text")&&strcmp(f->type,"password")&&strcmp(f->type,"textarea")&&
           strcmp(f->type,"number")&&strcmp(f->type,"checkbox")&&strcmp(f->type,"select"))return -1;
        int o=xz_json_object_get(d,v,"options"), count=0;
        int dynamic=xz_json_object_get(d,v,"dynamic");
        if(dynamic>=0) {
            if(strcmp(f->type,"select")||d->tokens[dynamic].type!=XZ_JSON_PRIMITIVE||
               (!ext_raw_eq(d,dynamic,"true")&&!ext_raw_eq(d,dynamic,"false")))return -1;
            f->dynamic=ext_raw_eq(d,dynamic,"true");
        }
        if(!strcmp(f->type,"select")) {
            if(o<0||d->tokens[o].type!=XZ_JSON_ARRAY)return -1;
            for(int t=d->tokens[o].first_child;t>=0;t=d->tokens[t].next_sibling)
                if(++count>16||decode_string(d,t,b,257)||!b[0]||(f->dynamic&&!ext_value(s,s->nf,b)))return -1;
            if(!count&&!f->dynamic)return -1;
        } else if(o>=0)return -1;
        if((!strcmp(widget,"segments")&&(f->dynamic||count<2||count>5))||
           (!strcmp(widget,"switch")&&!strcmp(f->type,"select")&&(f->dynamic||count!=2)))return -1;
        if(!strcmp(widget,"switch")&&!strcmp(f->type,"select")) {
            int first=d->tokens[o].first_child,second=d->tokens[first].next_sibling;
            if(!ext_raw_eq(d,first,"0")||!ext_raw_eq(d,second,"1"))return -1;
        }
        int labels=xz_json_object_get(d,v,"optionLabels");
        if(labels>=0) {
            if(strcmp(f->type,"select")||f->dynamic||d->tokens[labels].type!=XZ_JSON_ARRAY)return -1;
            int n=0;for(int t=d->tokens[labels].first_child;t>=0;t=d->tokens[t].next_sibling)
                if(++n>count||decode_string(d,t,b,161)||!*b)return -1;
            if(n!=count)return -1;
        }
        if(!ext_text(d,v,"default",b,sizeof(b),0)||
           (xz_json_object_get(d,v,"default")>=0&&!ext_value(s,s->nf,b))||
           (!strcmp(f->type,"password")&&b[0]))return -1;
        s->nf++;
    }
    ar=xz_json_object_get(d,0,"actions");
    if(ar<0||d->tokens[ar].type!=XZ_JSON_ARRAY)return -1;
    for(v=d->tokens[ar].first_child;v>=0;v=d->tokens[v].next_sibling) {
        if(s->na>=EXT_ACTIONS||!ext_keys(d,v,"|id||label||fields|"))return -1;
        struct ext_action*a=&s->actions[s->na]; a->token=v;
        if(!ext_text(d,v,"id",a->id,sizeof(a->id),1)||!ext_ident(a->id)||!ext_text(d,v,"label",b,161,1))return -1;
        for(int i=0;i<s->na;i++)if(!strcmp(a->id,s->actions[i].id))return -1;
        int fs=xz_json_object_get(d,v,"fields"), seen=0;
        if(fs<0||d->tokens[fs].type!=XZ_JSON_ARRAY)return -1;
        for(int t=d->tokens[fs].first_child;t>=0;t=d->tokens[t].next_sibling) {
            int f; if(decode_string(d,t,b,33)||(f=ext_field_index(s,b))<0||(seen&(1<<f)))return -1;
            seen|=1<<f;
        }
        s->na++;
    }
    if(!ext_text(d,0,"load",b,33,0))return -1;
    if(*b) {
        int found=0;
        for(int i=0;i<s->na;i++)if(!strcmp(b,s->actions[i].id)) {
            int fs=xz_json_object_get(d,s->actions[i].token,"fields");
            if(d->tokens[fs].first_child>=0)return -1;
            found=1;
        }
        if(!found)return -1;
    }
    if(!s->na||!ext_flow_valid(s)||!ext_registry_valid(s)||!ext_registry_semantics(s)||!ext_structured_valid(s)||!ext_transport_schema_valid(s)||!ext_model_valid(s)||!ext_startup_valid(s))return -1;
    s->registry_valid=1;return 1;
}
/* Registries share the shipped tokenizer and scalar field validator. There is
 * no alternate script parser or executable expression evaluator. */
#define EXT_ROWS 16
#define EXT_ROW_FIELDS 8
static int ext_registry_find(const struct ext_schema*s,const char*kind,const char*id) {
    int a=xz_json_object_get(&s->doc,0,kind);char b[33];
    if(a<0)return -1;
    for(int t=s->doc.tokens[a].first_child;t>=0;t=s->doc.tokens[t].next_sibling)
        if(ext_text(&s->doc,t,"id",b,sizeof(b),1)&&!strcmp(b,id))return t;
    return -1;
}
static int ext_repeat(const struct ext_schema*s,int f) {
    return ext_registry_find(s,"repeat_inputs",s->fields[f].name);
}
static int ext_effective_value(const struct ext_schema*s,int f,char out[2049]) {
    if(f<0||f>=s->nf)return 0;
    if(s->saved_mask&(1U<<f)){strcpy(out,s->saved_values[f]);return 1;}
    int repeat=ext_repeat(s,f),t=xz_json_object_get(&s->doc,repeat,"default");
    if(t>=0) {
        size_t n=s->doc.tokens[t].end-s->doc.tokens[t].start;
        if(n>2048)return 0;memcpy(out,s->doc.source+s->doc.tokens[t].start,n);out[n]=0;return 1;
    }
    if(xz_json_object_get(&s->doc,s->fields[f].token,"default")<0){out[0]=0;return 0;}
    return ext_text(&s->doc,s->fields[f].token,"default",out,2049,0);
}
static int ext_type_schema(const struct ext_schema*s,int type,struct ext_schema*r) {
    const xz_json_doc*d=&s->doc;int fields=xz_json_object_get(d,type,"fields");
    if(fields<0)return 0;
    /* Project scalar field declarations by original token boundaries. Do not
     * remove comma bytes in-place (adjacent metadata keys can leave bad JSON). */
    size_t used=0;const char*head="{\"version\":1,\"title\":\"row\",\"fields\":[";
    if(!ext_buf(r->json,sizeof(r->json),&used,head,strlen(head)))return 0;
    int count=0;
    for(int f=d->tokens[fields].first_child;f>=0;f=d->tokens[f].next_sibling) {
        if(!ext_keys(d,f,"|name||label||type||default||placeholder||options||optionLabels||group||format||min||max||step||precision||center||maxBytes||widget||unit||required||display_only||conditions|"))return 0;
        if((count++&&!ext_buf(r->json,sizeof(r->json),&used,",",1))||!ext_buf(r->json,sizeof(r->json),&used,"{",1))return 0;
        int members=0;
        for(int k=d->tokens[f].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;
            if(!ext_raw_eq(d,k,"required")&&!ext_raw_eq(d,k,"display_only")&&!ext_raw_eq(d,k,"conditions")) {
                size_t begin=d->tokens[k].start-1,end=d->tokens[v].end+(d->tokens[v].type==XZ_JSON_STRING);
                if((members++&&!ext_buf(r->json,sizeof(r->json),&used,",",1))||!ext_buf(r->json,sizeof(r->json),&used,d->source+begin,end-begin))return 0;
            }k=d->tokens[v].next_sibling;
        }
        if(!ext_buf(r->json,sizeof(r->json),&used,"}",1))return 0;
    }
    const char*tail="],\"actions\":[{\"id\":\"save\",\"label\":\"save\",\"fields\":[]}]}";
    if(!ext_buf(r->json,sizeof(r->json),&used,tail,strlen(tail)))return 0;
    return ext_parse(r)==1&&r->nf<=EXT_ROW_FIELDS;
}
static int ext_row_schema(const struct ext_schema*s,int repeat,struct ext_schema*r) {
    char id[33];if(!ext_text(&s->doc,repeat,"row_type",id,sizeof(id),1))return 0;
    return ext_type_schema(s,ext_registry_find(s,"row_types",id),r);
}
/* Name namespace validation, using the frozen comparison normalization's
 * whitespace/final-punctuation rules. No matching, selection or inference. */
static int ext_label_normalize(const char*in,char out[513],int strip_final) {
    size_t n=0,chars=0;
    while(*in) {
        unsigned c=(unsigned char)*in,cp=c;int k=c<128?1:c>=0xc2&&c<=0xdf?2:c<=0xef&&c>=0xe0?3:c<=0xf4&&c>=0xf0?4:0;
        if(!k||++chars>128)return 0;
        if(k>1){cp&=(1U<<(7-k))-1;for(int j=1;j<k;j++){if(!in[j]||((unsigned char)in[j]&192)!=128)return 0;cp=(cp<<6)|((unsigned char)in[j]&63);}}
        if((k==2&&cp<128)||(k==3&&cp<2048)||(k==4&&cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
        int space=(cp>=9&&cp<=13)||cp==32||cp==0x85||cp==0xa0||cp==0x1680||(cp>=0x2000&&cp<=0x200a)||cp==0x2028||cp==0x2029||cp==0x202f||cp==0x205f||cp==0x3000;
        if(!space){if(c<32||n+k>512)return 0;memcpy(out+n,in,(size_t)k);n+=(size_t)k;}in+=k;
    }out[n]=0;
    while(strip_final&&n){size_t start=n-1;while(start&&((unsigned char)out[start]&192)==128)start--;
        if(!((n-start==1&&strchr(".!?,",out[start]))||(n-start==3&&(!memcmp(out+start,"。",3)||!memcmp(out+start,"！",3)||!memcmp(out+start,"？",3)||!memcmp(out+start,"，",3)))))break;
        out[n=start]=0;
    }return n>0;
}
static int ext_label(const char*in,char out[513]) {return ext_label_normalize(in,out,1);}
static int ext_separator(const xz_json_doc*d,int t,char out[65]) {
    char raw[65],normalized[513];
    if(decode_string(d,t,raw,sizeof(raw)))return 0;
    if(!ext_label_normalize(raw,normalized,0))return 0;size_t n=strlen(normalized);
    if(n>64)return 0;memcpy(out,normalized,n+1);return 1;
}
static const char *ext_condition_comparison(const xz_json_doc *d,int t) {
    int positive=xz_json_object_get(d,t,"equals")>=0;
    int negative=xz_json_object_get(d,t,"notEquals")>=0;
    return positive==negative?NULL:positive?"equals":"notEquals";
}
static int ext_condition(const struct ext_schema*s,const char*id,const xz_json_doc*row,int fields) {
    int t=ext_registry_find(s,"conditions",id);char src[80],expected[2049],value[2049];
    const char *comparison=t<0?NULL:ext_condition_comparison(&s->doc,t);
    if(!comparison||!ext_text(&s->doc,t,"source",src,sizeof(src),1)||!ext_text(&s->doc,t,comparison,expected,sizeof(expected),0))return 0;
    if(!strncmp(src,"settings.",9)) {
        int f=ext_field_index(s,src+9);if(f<0)return 0;
        if(!ext_effective_value(s,f,value))return 0;
    } else if(!strncmp(src,"row.",4)&&row) {
        if(xz_json_object_get(row,fields,src+4)<0||!ext_text(row,fields,src+4,value,sizeof(value),0))return 0;
    } else return 0;
    return (strcmp(value,expected)==0)==(comparison[0]=='e');
}
static int ext_conditions(const struct ext_schema*s,const xz_json_doc*d,int a,const xz_json_doc*row,int fields) {
    if(a<0)return 1;if(d->tokens[a].type!=XZ_JSON_ARRAY)return 0;
    for(int t=d->tokens[a].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char id[33];if(decode_string(d,t,id,sizeof(id))||!ext_condition(s,id,row,fields))return 0;
    }
    return 1;
}
static int ext_visible(const struct ext_schema*s,const char*target) {
    int a=xz_json_object_get(&s->doc,0,"visibility");char b[128];if(a<0)return 1;
    for(int t=s->doc.tokens[a].first_child;t>=0;t=s->doc.tokens[t].next_sibling)
        if(ext_text(&s->doc,t,"target",b,sizeof(b),1)&&!strcmp(b,target)&&
           !ext_conditions(s,&s->doc,xz_json_object_get(&s->doc,t,"conditions"),NULL,-1))return 0;
    return 1;
}
static int ext_rows_value(const struct ext_schema*s,int f,const char*value) {
    int repeat=ext_repeat(s,f);if(repeat<0)return 1;
    xz_json_doc rows;char wrapped[2060];int array=ext_rows_parse(value,wrapped,&rows);
    struct ext_schema*r=calloc(1,sizeof(*r));int ok=0,count=0,min=0,max=0;
    const xz_json_doc*d=&s->doc;char display[33],name[2049],ids[EXT_ROWS][33];
    char (*names)[513]=calloc(EXT_ROWS*(EXT_ROW_FIELDS+1),513);int owners[EXT_ROWS*(EXT_ROW_FIELDS+1)],nn=0;
    if(!r||!names||!ext_row_schema(s,repeat,r)||array<0||
       xz_json_get_int(d,repeat,"maxRows",&max)||!ext_text(d,repeat,"display_field",display,sizeof(display),1))goto done;
    (void)xz_json_get_int(d,repeat,"minRows",&min);
    for(int t=rows.tokens[array].first_child;t>=0;t=rows.tokens[t].next_sibling) {
        char id[33];if(count>=max||count>=EXT_ROWS||!ext_keys(&rows,t,"|id||fields|")||!ext_text(&rows,t,"id",id,sizeof(id),1)||!ext_ident(id))goto done;
        for(int i=0;i<count;i++)if(!strcmp(ids[i],id))goto done;
        strcpy(ids[count],id);int fs=xz_json_object_get(&rows,t,"fields");unsigned seen=0;
        if(fs<0||rows.tokens[fs].type!=XZ_JSON_OBJECT)goto done;
        for(int k=rows.tokens[fs].first_child;k>=0;) {
            char key[33],v[2049];int vtok=rows.tokens[k].next_sibling;
            if(decode_string(&rows,k,key,sizeof(key))||!ext_raw_eq(&rows,k,key))goto done;
            int i=ext_field_index(r,key);
            if(i<0||(seen&(1U<<i))||!ext_text(&rows,fs,key,v,sizeof(v),0)||!ext_value(r,i,v))goto done;
            seen|=1U<<i;k=rows.tokens[vtok].next_sibling;
        }
        /* Required metadata is read from original row declaration. */
        char typ[33];ext_text(d,repeat,"row_type",typ,sizeof(typ),1);
        int rt=ext_registry_find(s,"row_types",typ),rf=xz_json_object_get(d,rt,"fields");
        for(int q=d->tokens[rf].first_child;q>=0;q=d->tokens[q].next_sibling) {
            char key[33],v[2049];ext_text(d,q,"name",key,sizeof(key),1);
            int req=xz_json_object_get(d,q,"required");
            if(req>=0&&ext_raw_eq(d,req,"true")&&
               (!ext_text(&rows,fs,key,v,sizeof(v),1)))goto done;
        }
        int aliases=xz_json_object_get(d,repeat,"alias_fields"),q=-1;
        for(int j=0;;j++) {
            char field[33];if(!j)strcpy(field,display);
            else {q=j==1?(aliases<0?-1:d->tokens[aliases].first_child):d->tokens[q].next_sibling;if(q<0)break;if(decode_string(d,q,field,sizeof(field)))goto done;}
            if(!ext_text(&rows,fs,field,name,sizeof(name),!j))goto done;if(!*name)continue;
            char literal_name[513];
            if(nn>=EXT_ROWS*(EXT_ROW_FIELDS+1)||!ext_label(name,names[nn])||
               !ext_label_normalize(name,literal_name,0))goto done;
            for(int i=0;i<nn;i++)if(owners[i]!=count&&!strcmp(names[i],names[nn]))goto done;
            /* Any declared separator for this list must not occur in a label. */
            int slots=xz_json_object_get(d,0,"list_slots");
            for(int k=slots<0?-1:d->tokens[slots].first_child;k>=0;) {
                int v=d->tokens[k].next_sibling;char binding[33],source[80],repeat_id[33];ext_text(d,repeat,"id",repeat_id,sizeof(repeat_id),1);
                if(!ext_text(d,v,"binding",binding,sizeof(binding),1))goto done;
                int bt=ext_registry_find(s,"default_bindings",binding);if(!ext_text(d,bt,"source",source,sizeof(source),1))goto done;
                if(!strncmp(source,"repeat_inputs.",14)&&!strcmp(source+14,repeat_id)) {
                    int sep=xz_json_object_get(d,v,"separators");if(sep<0)goto done;
                    for(int z=d->tokens[sep].first_child;z>=0;z=d->tokens[z].next_sibling){char literal[65];if(!ext_separator(d,z,literal)||strstr(literal_name,literal))goto done;}
                }k=d->tokens[v].next_sibling;
            }
            owners[nn++]=count;
        }count++;
    }
    ok=count>=min;
done:free(names);free(r);return ok;
}
/* Save normalization is deliberately separate from load validation. Only a
 * NEW, entirely empty row may disappear; existing rows require explicit remove. */
static int ext_rows_edit(const struct ext_schema*s,int f,const char*value,char out[2049]) {
    int rep=ext_repeat(s,f);if(rep<0){strcpy(out,value);return ext_value(s,f,out);}
    char wrapped[2060],oldwrapped[2060],old[2049];xz_json_doc d,previous;
    int a=ext_rows_parse(value,wrapped,&d),pa=-1;
    if(ext_effective_value(s,f,old))pa=ext_rows_parse(old,oldwrapped,&previous);
    struct ext_schema*r=calloc(1,sizeof(*r));int ok=0,count=0;size_t used=0;
    if(a<0||!r||!ext_row_schema(s,rep,r)||!ext_buf(out,2049,&used,"[",1))goto done;
    char ids[EXT_ROWS][33];int seen=0;
    for(int t=d.tokens[a].first_child;t>=0;t=d.tokens[t].next_sibling) {
        char id[33],fields[EXT_ROW_FIELDS][2049];unsigned supplied=0;int any=0,exists=0;
        if(seen>=EXT_ROWS||!ext_keys(&d,t,"|id||fields|")||!ext_text(&d,t,"id",id,sizeof(id),1)||!ext_ident(id))goto done;
        for(int i=0;i<seen;i++)if(!strcmp(ids[i],id))goto done;strcpy(ids[seen++],id);
        int fs=xz_json_object_get(&d,t,"fields");if(fs<0||d.tokens[fs].type!=XZ_JSON_OBJECT)goto done;
        for(int k=d.tokens[fs].first_child;k>=0;) {
            char key[33];if(decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key))goto done;
            int j=ext_field_index(r,key);if(j<0||(supplied&(1U<<j)))goto done;supplied|=1U<<j;k=d.tokens[d.tokens[k].next_sibling].next_sibling;
        }
        for(int i=0;i<r->nf;i++) {
            if(!ext_text(&d,fs,r->fields[i].name,fields[i],sizeof(fields[i]),0))goto done;
            if(!(supplied&(1U<<i)))ext_effective_value(r,i,fields[i]);
            if(fields[i][0])any=1;
        }
        for(int p=pa<0?-1:previous.tokens[pa].first_child;p>=0;p=previous.tokens[p].next_sibling) {
            char prior[33];if(ext_text(&previous,p,"id",prior,sizeof(prior),1)&&!strcmp(id,prior))exists=1;
        }
        if(!any&&!exists)continue;
        if((count++&&!ext_buf(out,2049,&used,",",1))||!ext_buf(out,2049,&used,"{\"id\":",6)||!ext_quote(out,2049,&used,id)||!ext_buf(out,2049,&used,",\"fields\":{",11))goto done;
        int n=0;for(int i=0;i<r->nf;i++)if((supplied&(1U<<i))||fields[i][0]) {
            if((n++&&!ext_buf(out,2049,&used,",",1))||!ext_quote(out,2049,&used,r->fields[i].name)||!ext_buf(out,2049,&used,":",1)||!ext_quote(out,2049,&used,fields[i]))goto done;
        }
        if(!ext_buf(out,2049,&used,"}}",2))goto done;
    }
    ok=ext_buf(out,2049,&used,"]",1)&&ext_value(s,f,out);
done:free(r);return ok;
}
static int ext_refs_valid(const struct ext_schema*s,int a) {
    const xz_json_doc*d=&s->doc;if(a<0)return 1;if(d->tokens[a].type!=XZ_JSON_ARRAY)return 0;
    int n=0;
    for(int t=d->tokens[a].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char id[33];if(++n>24||decode_string(d,t,id,sizeof(id)))return 0;
        int c=ext_registry_find(s,"conditions",id);if(c<0)return 0;
        for(int p=d->tokens[a].first_child;p!=t;p=d->tokens[p].next_sibling)
            if(ext_raw_eq(d,p,id))return 0;
    }return 1;
}
static int ext_registry_valid(struct ext_schema*s) {
    const xz_json_doc*d=&s->doc;char id[33],b[2049];
    const char*registries[]={"conditions","row_types","repeat_inputs","visibility","default_bindings"};
    const char*keys[]={"|id||source||equals||notEquals|","|id||fields|","|id||row_type||minRows||maxRows||display_field||alias_fields||reorder||default|","|target||conditions|","|id||source||enabled_conditions||role||prompt||selection||map|"};
    for(int kind=0;kind<5;kind++) {
        int a=xz_json_object_get(d,0,registries[kind]),count=0;if(a<0)continue;
        if(d->tokens[a].type!=XZ_JSON_ARRAY)return 0;
        for(int t=d->tokens[a].first_child;t>=0;t=d->tokens[t].next_sibling) {
            if(++count>16||!ext_keys(d,t,keys[kind]))return 0;
            if(kind!=3&&(!ext_text(d,t,"id",id,sizeof(id),1)||!ext_ident(id)||ext_registry_find(s,registries[kind],id)!=t))return 0;
            if(kind==0) {
                const char *comparison=ext_condition_comparison(d,t);if(!comparison)return 0;
                if(!ext_text(d,t,"source",b,sizeof(b),1))return 0;
                if(!strncmp(b,"settings.",9)) {int f=ext_field_index(s,b+9);if(f<0||!ext_text(d,t,comparison,b,sizeof(b),0)||!ext_value(s,f,b))return 0;}
                else if(strncmp(b,"row.",4)||!ext_ident(b+4)||!ext_text(d,t,comparison,b,sizeof(b),0))return 0;
            }
            if(kind==1) {
                struct ext_schema*r=calloc(1,sizeof(*r));int valid=r&&ext_type_schema(s,t,r);free(r);if(!valid)return 0;
                int fields=xz_json_object_get(d,t,"fields"),n=0;if(fields<0||d->tokens[fields].type!=XZ_JSON_ARRAY)return 0;
                for(int q=d->tokens[fields].first_child;q>=0;q=d->tokens[q].next_sibling) {
                    if(++n>EXT_ROW_FIELDS||!ext_refs_valid(s,xz_json_object_get(d,q,"conditions")))return 0;
                    const char*flags[]={"required","display_only"};for(int k=0;k<2;k++) {int v=xz_json_object_get(d,q,flags[k]);if(v>=0&&!ext_raw_eq(d,v,"true")&&!ext_raw_eq(d,v,"false"))return 0;}
                }if(!n)return 0;
            }
            if(kind==2) {
                int f=ext_field_index(s,id),min=0,max=0;struct ext_schema*r=calloc(1,sizeof(*r));
                int good=r&&f>=0&&!strcmp(s->fields[f].type,"textarea")&&ext_row_schema(s,t,r)&&
                    !xz_json_get_int(d,t,"maxRows",&max)&&max>0&&max<=EXT_ROWS;
                if(xz_json_object_get(d,t,"minRows")>=0&&(xz_json_get_int(d,t,"minRows",&min)||min<0||min>max))good=0;
                if(!ext_text(d,t,"display_field",b,33,1)||!r||ext_field_index(r,b)<0)good=0;
                int aliases=xz_json_object_get(d,t,"alias_fields");if(aliases>=0) {
                    if(d->tokens[aliases].type!=XZ_JSON_ARRAY)good=0;
                    for(int q=d->tokens[aliases].first_child;q>=0;q=d->tokens[q].next_sibling)
                        if(decode_string(d,q,b,33)||!r||ext_field_index(r,b)<0)good=0;
                }
                free(r);if(!good)return 0;
                int reorder=xz_json_object_get(d,t,"reorder");if(reorder>=0&&!ext_raw_eq(d,reorder,"true")&&!ext_raw_eq(d,reorder,"false"))return 0;
                int def=xz_json_object_get(d,t,"default");if(def>=0) {
                    size_t n=d->tokens[def].end-d->tokens[def].start;if(n>=sizeof(b))return 0;
                    memcpy(b,d->source+d->tokens[def].start,n);b[n]=0;if(!ext_rows_value(s,f,b))return 0;
                }
                if(xz_json_object_get(d,s->fields[f].token,"default")>=0&&(!ext_text(d,s->fields[f].token,"default",b,sizeof(b),0)||!ext_rows_value(s,f,b)))return 0;
            }
            if(kind==3) {
                if(!ext_text(d,t,"target",b,128,1)||!ext_refs_valid(s,xz_json_object_get(d,t,"conditions")))return 0;
                if(!strncmp(b,"actions.",8)) {if(ext_action_index(s,b+8)<0)return 0;}
                else if(!strncmp(b,"fields.",7)) {if(ext_field_index(s,b+7)<0)return 0;}
                else if(!strncmp(b,"repeat_inputs.",14)) {if(ext_registry_find(s,"repeat_inputs",b+14)<0)return 0;}
                else if(!strncmp(b,"groups.",7)) {
                    int found=0;for(int f=0;f<s->nf;f++){char group[81];if(ext_text(d,s->fields[f].token,"group",group,sizeof(group),0)&&*group&&!strcmp(group,b+7))found=1;}if(!found)return 0;
                }
                else if(!strncmp(b,"row_types.",10)) {
                    char*part=strstr(b+10,".fields.");int group=0;if(!part){part=strstr(b+10,".groups.");group=1;}if(!part)return 0;*part=0;
                    int rt=ext_registry_find(s,"row_types",b+10),fields=xz_json_object_get(d,rt,"fields"),found=0;
                    if(fields<0)return 0;
                    for(int q=d->tokens[fields].first_child;q>=0;q=d->tokens[q].next_sibling) {char key[81];if(ext_text(d,q,group?"group":"name",key,sizeof(key),0)&&*key&&!strcmp(key,part+8))found=1;}
                    if(!found)return 0;
                }else return 0;
            }
            if(kind==4) {
                if(!ext_text(d,t,"prompt",b,257,0)||!ext_text(d,t,"role",b,33,0)||(*b&&!ext_ident(b)))return 0;
                if(!ext_text(d,t,"source",b,128,1)||!ext_refs_valid(s,xz_json_object_get(d,t,"enabled_conditions")))return 0;
                int repeat=-1;
                if(!strncmp(b,"settings.",9)) {if(ext_field_index(s,b+9)<0)return 0;}
                else if(!strncmp(b,"repeat_inputs.",14)) {repeat=ext_registry_find(s,"repeat_inputs",b+14);if(repeat<0)return 0;}
                else return 0;
                int selection=xz_json_object_get(d,t,"selection");
                if(selection>=0) {
                    char mode[16];int min=1,max=1,rows=0;
                    if(repeat<0||!ext_keys(d,selection,"|mode||minSelected||maxSelected||answer_patterns||all_patterns|")||!ext_text(d,selection,"mode",mode,sizeof(mode),1))return 0;
                    if(xz_json_object_get(d,selection,"minSelected")>=0&&xz_json_get_int(d,selection,"minSelected",&min))return 0;
                    if(xz_json_object_get(d,selection,"maxSelected")>=0&&xz_json_get_int(d,selection,"maxSelected",&max))return 0;
                    if(!strcmp(mode,"multiple")&&xz_json_object_get(d,selection,"maxSelected")<0)return 0;
                    if(strcmp(mode,"single")&&strcmp(mode,"multiple"))return 0;
                    if(xz_json_get_int(d,repeat,"maxRows",&rows)||min<1||min>max||max>rows||(!strcmp(mode,"single")&&max!=1))return 0;
                }
                int map=xz_json_object_get(d,t,"map"),n=0;if(map<0||d->tokens[map].type!=XZ_JSON_ARRAY)return 0;
                for(int q=d->tokens[map].first_child;q>=0;q=d->tokens[q].next_sibling) {
                    if(++n>EXT_ROW_FIELDS||!ext_keys(d,q,"|field||target|"))return 0;
                    int target=xz_json_object_get(d,q,"target");char type[24],name[81];
                    if(!ext_keys(d,target,"|kind||name||app||op||flow||step||item_field|")||!ext_text(d,target,"kind",type,sizeof(type),1)||!ext_text(d,target,"name",name,sizeof(name),1)||!ext_ident(name))return 0;
                    if(strcmp(type,"slot")&&strcmp(type,"op_param")&&strcmp(type,"flow_input")&&strcmp(type,"step_param"))return 0;
                    if(!ext_keys(d,target,!strcmp(type,"slot")?"|kind||name|":
                        !strcmp(type,"op_param")?"|kind||name||app||op||item_field|":
                        !strcmp(type,"flow_input")?"|kind||name||flow||item_field|":"|kind||name||flow||step||item_field|"))return 0;
                    if(!strcmp(type,"op_param")&&(!ext_text(d,target,"app",b,81,1)||!ext_text(d,target,"op",b,81,1)))return 0;
                    if((!strcmp(type,"flow_input")||!strcmp(type,"step_param"))&&!ext_text(d,target,"flow",b,81,1))return 0;
                    if(!strcmp(type,"step_param")&&!ext_text(d,target,"step",b,81,1))return 0;
                    if(repeat>=0) {struct ext_schema*r=calloc(1,sizeof(*r));int good=r&&ext_row_schema(s,repeat,r)&&ext_text(d,q,"field",b,33,1)&&ext_field_index(r,b)>=0;free(r);if(!good)return 0;}
                    else if(xz_json_object_get(d,q,"field")>=0)return 0;
                }if(!n)return 0;
            }
        }
    }
    int slots=xz_json_object_get(d,0,"list_slots");if(slots>=0) {
        if(d->tokens[slots].type!=XZ_JSON_OBJECT)return 0;
        for(int k=d->tokens[slots].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;if(!ext_keys(d,v,"|binding||separators||minChars||maxChars||itemMaxChars|")||decode_string(d,k,id,sizeof(id))||!ext_ident(id)||!ext_text(d,v,"binding",b,33,1)||ext_registry_find(s,"default_bindings",b)<0)return 0;
            int a=xz_json_object_get(d,v,"separators"),n=0;if(a<0||d->tokens[a].type!=XZ_JSON_ARRAY)return 0;
            for(int q=d->tokens[a].first_child;q>=0;q=d->tokens[q].next_sibling) {if(++n>16||!ext_separator(d,q,b))return 0;}
            if(!n)return 0;k=d->tokens[v].next_sibling;
        }
    }
    return 1;
}
/* Check references in their actual scope. A row condition never reads another
 * row, and a hidden control cannot disable format/required validation. */
static int ext_condition_scope(const struct ext_schema*s,int refs,const struct ext_schema*row,int target,unsigned*edges) {
    const xz_json_doc*d=&s->doc;if(refs<0)return 1;
    for(int t=d->tokens[refs].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char id[33],src[80],v[2049];if(decode_string(d,t,id,sizeof(id)))return 0;
        int c=ext_registry_find(s,"conditions",id);
        const char *comparison=c<0?NULL:ext_condition_comparison(d,c);
        if(!comparison||!ext_text(d,c,"source",src,sizeof(src),1)||!ext_text(d,c,comparison,v,sizeof(v),0))return 0;
        if(!strncmp(src,"row.",4)) {
            int f=row?ext_field_index(row,src+4):-1;if(f<0||!ext_value(row,f,v))return 0;
            if(edges&&target>=0)edges[target]|=1U<<f;
        } else {
            int f=ext_field_index(s,src+9);
            if(f<0||ext_repeat(s,f)>=0)return 0;
            if(!row&&edges&&target>=0)edges[target]|=1U<<f;
        }
    }return 1;
}
static int ext_acyclic(unsigned *edges,int n) {
    for(int k=0;k<n;k++)for(int i=0;i<n;i++)if(edges[i]&(1U<<k))edges[i]|=edges[k];
    for(int i=0;i<n;i++)if(edges[i]&(1U<<i))return 0;return 1;
}
static int ext_target_equal(const xz_json_doc*d,int a,int b,int item) {
    const char*keys[]={"kind","name","app","op","flow","step","item_field"};
    for(int i=0;i<(item?7:6);i++){char x[81],y[81];if(!ext_text(d,a,keys[i],x,sizeof(x),0)||!ext_text(d,b,keys[i],y,sizeof(y),0)||strcmp(x,y))return 0;}
    return 1;
}
/* The original expression compiler owns pattern syntax. A consumer built
 * without that component must reject pattern-bearing declarations before it
 * can read, save or run them; it cannot certify strings by shape alone. */
static int ext_selection_pattern(const char*pattern,const char*slot,int all) {
#ifdef LX06_REV12_EXPRESSION_FIXTURE
    struct expr_program*p=calloc(1,sizeof(*p));int ok=0;
    if(!p||lx06_expr_compile(pattern,strlen(pattern),p))goto done;
    if(all){ok=!p->slots;goto done;}
    if(p->slots!=1||!p->lists[0]||strcmp(p->names[0],slot))goto done;
    for(unsigned b=0;b<p->branches;b++) {
        struct expr_branch branch;
        if(lx06_expr_branch(p,b,&branch)||branch.slot_mask!=1||branch.list_mask!=1)goto done;
    }ok=1;
done:free(p);return ok;
#else
    (void)pattern;(void)slot;(void)all;return 0;
#endif
}
static int ext_registry_semantics(struct ext_schema*s) {
    const xz_json_doc*d=&s->doc;unsigned edges[EXT_FIELDS]={0};
    int visibility=xz_json_object_get(d,0,"visibility");
    for(int t=visibility<0?-1:d->tokens[visibility].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char target[128],other[128];if(!ext_text(d,t,"target",target,sizeof(target),1))return 0;
        for(int q=d->tokens[visibility].first_child;q!=t;q=d->tokens[q].next_sibling)
            if(ext_text(d,q,"target",other,sizeof(other),1)&&!strcmp(other,target))return 0;
        int refs=xz_json_object_get(d,t,"conditions");
        if(strncmp(target,"row_types.",10)) {
            int f=!strncmp(target,"fields.",7)?ext_field_index(s,target+7):!strncmp(target,"repeat_inputs.",14)?ext_field_index(s,target+14):-1;
            if(!ext_condition_scope(s,refs,NULL,f,edges))return 0;
            if(!strncmp(target,"groups.",7))for(int i=0;i<s->nf;i++) {
                char group[81];ext_text(d,s->fields[i].token,"group",group,sizeof(group),0);
                if(!strcmp(group,target+7)&&!ext_condition_scope(s,refs,NULL,i,edges))return 0;
            }
        }
    }
    if(!ext_acyclic(edges,s->nf))return 0;
    int repeats=xz_json_object_get(d,0,"repeat_inputs");
    for(int t=repeats<0?-1:d->tokens[repeats].first_child;t>=0;t=d->tokens[t].next_sibling) {
        struct ext_schema*r=calloc(1,sizeof(*r));if(!r)return 0;
        int ok=ext_row_schema(s,t,r);unsigned re[EXT_ROW_FIELDS]={0};char type[33],id[33];
        ext_text(d,t,"row_type",type,sizeof(type),1);ext_text(d,t,"id",id,sizeof(id),1);
        int rt=ext_registry_find(s,"row_types",type),fs=xz_json_object_get(d,rt,"fields"),i=0;
        for(int f=d->tokens[fs].first_child;ok&&f>=0;f=d->tokens[f].next_sibling,i++)
            ok=ext_condition_scope(s,xz_json_object_get(d,f,"conditions"),r,i,re);
        for(int v=visibility<0?-1:d->tokens[visibility].first_child;ok&&v>=0;v=d->tokens[v].next_sibling) {
            char target[128],prefix[64];ext_text(d,v,"target",target,sizeof(target),1);snprintf(prefix,sizeof(prefix),"row_types.%s.fields.",type);
            if(!strncmp(target,prefix,strlen(prefix)))ok=ext_condition_scope(s,xz_json_object_get(d,v,"conditions"),r,ext_field_index(r,target+strlen(prefix)),re);
            snprintf(prefix,sizeof(prefix),"row_types.%s.groups.",type);
            if(!strncmp(target,prefix,strlen(prefix)))for(int f=0;ok&&f<r->nf;f++) {
                char group[81];ext_text(&r->doc,r->fields[f].token,"group",group,sizeof(group),0);
                if(!strcmp(group,target+strlen(prefix)))ok=ext_condition_scope(s,xz_json_object_get(d,v,"conditions"),r,f,re);
            }
        }
        ok=ok&&ext_acyclic(re,r->nf);free(r);if(!ok)return 0;
        /* A repeat's default and the legacy flat field default cannot disagree. */
        int f=ext_field_index(s,id),def=xz_json_object_get(d,t,"default");char value[2049];
        if(def>=0&&xz_json_object_get(d,s->fields[f].token,"default")>=0) {
            if(!ext_text(d,s->fields[f].token,"default",value,sizeof(value),0))return 0;
            char canonical[2049],canonical_default[2049],repeat_default[2049];
            if(!ext_effective_value(s,f,repeat_default)||!ext_rows_edit(s,f,value,canonical)||
               !ext_rows_edit(s,f,repeat_default,canonical_default)||strcmp(canonical,canonical_default))return 0;
        }
    }
    int bindings=xz_json_object_get(d,0,"default_bindings"),targets[128],owners[128],nt=0;
    for(int t=bindings<0?-1:d->tokens[bindings].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char source[80],type[33];ext_text(d,t,"source",source,sizeof(source),1);
        int rep=!strncmp(source,"repeat_inputs.",14)?ext_registry_find(s,"repeat_inputs",source+14):-1;
        struct ext_schema*r=rep<0?NULL:calloc(1,sizeof(*r));
        if(rep>=0&&(!r||!ext_row_schema(s,rep,r))){free(r);return 0;}
        if(!ext_condition_scope(s,xz_json_object_get(d,t,"enabled_conditions"),r,-1,NULL)){free(r);return 0;}
        int selection=xz_json_object_get(d,t,"selection");char mode[16]="single";
        if(selection>=0)ext_text(d,selection,"mode",mode,sizeof(mode),1);
        const char*patterns[]={"answer_patterns","all_patterns"};
        for(int kind=0;kind<2;kind++) {
            int a=xz_json_object_get(d,selection,patterns[kind]),n=0;if(a<0)continue;
            if(d->tokens[a].type!=XZ_JSON_ARRAY){free(r);return 0;}
            for(int q=d->tokens[a].first_child;q>=0;q=d->tokens[q].next_sibling) {
                char pattern[2049],slot[33];int result=xz_json_object_get(d,q,"result");
                if(++n>16||!ext_keys(d,q,kind?"|pattern||result||expected_count|":"|pattern||result|")||!ext_text(d,q,"pattern",pattern,sizeof(pattern),1)){free(r);return 0;}
                if(kind) {
                    int expected;if(!ext_text(d,q,"result",slot,sizeof(slot),1)||strcmp(slot,"all")||
                        (xz_json_object_get(d,q,"expected_count")>=0&&(xz_json_get_int(d,q,"expected_count",&expected)||expected<1||expected>EXT_ROWS))){free(r);return 0;}
                } else {
                    int slots=xz_json_object_get(d,0,"list_slots");char binding[33],id[33];ext_text(d,t,"id",id,sizeof(id),1);
                    if(!ext_keys(d,result,"|slot|")||!ext_text(d,result,"slot",slot,sizeof(slot),1)||
                       !ext_text(d,xz_json_object_get(d,slots,slot),"binding",binding,sizeof(binding),1)||strcmp(binding,id)){free(r);return 0;}
                }
                if(!ext_selection_pattern(pattern,slot,kind)){free(r);return 0;}
            }
        }
        unsigned mapped=0;int first=-1,array=-1,map=xz_json_object_get(d,t,"map");
        for(int q=d->tokens[map].first_child;q>=0;q=d->tokens[q].next_sibling) {
            int target=xz_json_object_get(d,q,"target");char field[33],item[33];
            ext_text(d,target,"item_field",item,sizeof(item),0);
            if(*item&&!ext_ident(item)){free(r);return 0;}
            if((*item&&rep<0)||(array>=0&&array!=!!*item)){free(r);return 0;}array=!!*item;
            if(!strcmp(mode,"multiple")&&!*item){free(r);return 0;}
            if(*item&&first>=0&&!ext_target_equal(d,first,target,0)){free(r);return 0;}
            if(first<0)first=target;
            if(r){ext_text(d,q,"field",field,sizeof(field),1);int f=ext_field_index(r,field);if(f<0){free(r);return 0;}mapped|=1U<<f;}
            for(int k=0;k<nt;k++)if(ext_target_equal(d,targets[k],target,0)&&
                (owners[k]!=t||ext_target_equal(d,targets[k],target,1))){free(r);return 0;}
            if(nt>=128){free(r);return 0;}targets[nt]=target;owners[nt++]=t;
        }
        if(r) {
            ext_text(d,rep,"row_type",type,sizeof(type),1);int rt=ext_registry_find(s,"row_types",type),fs=xz_json_object_get(d,rt,"fields"),i=0;
            for(int q=d->tokens[fs].first_child;q>=0;q=d->tokens[q].next_sibling,i++)
                if(!(mapped&(1U<<i))&&!ext_raw_eq(d,xz_json_object_get(d,q,"display_only"),"true")){free(r);return 0;}
        }free(r);
    }
    int slots=xz_json_object_get(d,0,"list_slots");
    for(int k=slots<0?-1:d->tokens[slots].first_child;k>=0;) {
        char key[33],binding[33],source[80];int v=d->tokens[k].next_sibling;
        if(decode_string(d,k,key,sizeof(key))||strlen(key)>16)return 0;
        for(char*p=key;*p;p++)if((*p<'a'||*p>'z')&&*p!='_')return 0;
        for(int q=d->tokens[slots].first_child;q!=k;q=d->tokens[d->tokens[q].next_sibling].next_sibling)if(ext_raw_eq(d,q,key))return 0;
        ext_text(d,v,"binding",binding,sizeof(binding),1);int b=ext_registry_find(s,"default_bindings",binding);
        if(!ext_text(d,b,"source",source,sizeof(source),1)||strncmp(source,"repeat_inputs.",14))return 0;
        const char*keys[]={"minChars","maxChars","itemMaxChars"};int limits[]={1,64,64};
        for(int i=0;i<3;i++)if(xz_json_object_get(d,v,keys[i])>=0&&(xz_json_get_int(d,v,keys[i],&limits[i])||limits[i]<1||limits[i]>128))return 0;
        if(limits[0]>limits[1])return 0;k=d->tokens[v].next_sibling;
    }
    return 1;
}
static int ext_dynamic_allowed(const struct ext_schema*s,int f,const char*v) {
    if(!s->fields[f].dynamic)return 1;
    if((s->current_mask&(1U<<f))&&!strcmp(s->current[f],v))return 1;
    char initial[EXT_OPTION_VALUE+1];
    if(ext_text(&s->doc,s->fields[f].token,"default",initial,sizeof(initial),0)&&*initial&&!strcmp(initial,v))return 1;
    int seed=xz_json_object_get(&s->doc,s->fields[f].token,"options");
    for(int t=s->doc.tokens[seed].first_child;t>=0;t=s->doc.tokens[t].next_sibling) {
        char b[257];if(!decode_string(&s->doc,t,b,sizeof(b))&&!strcmp(b,v))return 1;
    }
    if(!s->options_json[0])return 0;
    const xz_json_doc*d=&s->options_doc;
    int o=xz_json_object_get(d,0,"options"),a=xz_json_object_get(d,o,s->fields[f].name);
    if(a<0)return 0;
    for(int t=d->tokens[a].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char b[EXT_OPTION_VALUE+1];if(ext_text(d,t,"value",b,sizeof(b),1)&&!strcmp(b,v))return 1;
    }
    return 0;
}
/* Only a bounded, leading comment block can declare UI. Never discover UI by
 * running an uploaded file; a binary payload cannot become a declaration. */
static int ext_read(int fd,struct ext_schema*s) {
    char head[16385]; ssize_t n=pread(fd,head,sizeof(head)-1,0); int active=0;
    size_t used=0; if(n<=0)return 0; head[n]=0; s->json[0]=0;
    char*p=head;
    while(*p) {
        char*end=strchr(p,'\n'); if(!end)return active?-1:0;
        *end=0; if(end>p&&end[-1]=='\r')end[-1]=0;
        while(*p==' '||*p=='\t')p++;
        if(!strcmp(p,"# LX06-EXTENSION-BEGIN")) { if(active)return -1; active=1; }
        else if(!strcmp(p,"# LX06-EXTENSION-END")) return active?ext_parse(s):-1;
        else if(active) {
            if(*p!='#')return -1;
            p++; if(*p==' ')p++;
            size_t z=strlen(p); if(used+z+1>EXT_CAP)return -1;
            memcpy(s->json+used,p,z); used+=z; s->json[used++]='\n'; s->json[used]=0;
        } else if(*p&&*p!='#')return 0;
        p=end+1;
    }
    return active?-1:0;
}
static int ext_greeting_field(const struct ext_schema *s) {
    for (int i = 0; i < s->nf; i++) {
        char env[65];
        if (strcmp(s->fields[i].type, "text") && strcmp(s->fields[i].type, "textarea")) continue;
        if (!ext_text(&s->doc, s->fields[i].token, "env", env, sizeof env, 0)) continue;
        if (!strcmp(env, "MODEL_GREETING") || !strcmp(env, "DB_GREETING") ||
            !strcmp(env, "XZ_WAKE_TEXT")) return i;
    }
    return -1;
}

/* Values deliberately use strings (including numbers and checkbox true/false)
 * for portable /bin/sh + jsonfilter. No values are interpolated into commands. */
static int ext_request(const struct ext_schema*s,const char*body,char*action) {
    xz_json_doc d; char b[2049], key[33]; int a=-1, mask=0, got=0;
    if(strlen(body)>EXT_CAP||xz_json_parse(&d,body,strlen(body))||
       !ext_keys(&d,0,"|name||action||values|")||!ext_text(&d,0,"action",action,33,1))return 0;
    if(!strcmp(action,"regenerate-greeting")) {
        int vs=xz_json_object_get(&d,0,"values");
        return ext_greeting_field(s)>=0&&vs>=0&&d.tokens[vs].type==XZ_JSON_OBJECT&&d.tokens[vs].first_child<0;
    }
    for(int i=0;i<s->na;i++)if(!strcmp(s->actions[i].id,action))a=i;
    if(a<0)return 0;
    char target[48];snprintf(target,sizeof(target),"actions.%s",action);
    if(!ext_visible(s,target))return 0;
    int fs=xz_json_object_get(&s->doc,s->actions[a].token,"fields");
    for(int t=s->doc.tokens[fs].first_child;t>=0;t=s->doc.tokens[t].next_sibling) {
        if(decode_string(&s->doc,t,key,sizeof(key)))return 0;
        mask|=1<<ext_field_index(s,key);
    }
    int vs=xz_json_object_get(&d,0,"values");
    if(vs<0||d.tokens[vs].type!=XZ_JSON_OBJECT)return 0;
    for(int k=d.tokens[vs].first_child;k>=0;) {
        int f,v=d.tokens[k].next_sibling;
        if(decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||
           (f=ext_field_index(s,key))<0||!(mask&(1<<f))||(got&(1<<f)))return 0;
        if(ext_transport_capacity(s,f)) {
            if(f!=s->structured.request||!ext_transport_text(&d,vs,key,s,f))return 0;
        } else {
            char cleaned[2049];
            if(!ext_text(&d,vs,key,b,sizeof(b),0)||!ext_rows_edit(s,f,b,cleaned)||
               !ext_dynamic_allowed(s,f,cleaned))return 0;
        }
        got|=1<<f; k=d.tokens[v].next_sibling;
    }
    return got==mask;
}
/* Only explicitly public UI fields survive. stderr and arbitrary script output
 * never become browser output. Scripts must not emit tokens as public fields. */
static int ext_public_schema(const char*line,const struct ext_schema*s) {
    xz_json_doc d; char b[2049];
    if(strlen(line)>EXT_EVENT_CAP||xz_json_parse(&d,line,strlen(line))||
       !ext_keys(&d,0,"|message||link||linkLabel||code||values||options||step|"))return 0;
    if(!ext_text(&d,0,"step",b,33,0)||(*b&&(!s||ext_step_index(s,b)<0))||
       !ext_text(&d,0,"linkLabel",b,161,0))return 0;
    if(!ext_text(&d,0,"message",b,sizeof(b),0)||!ext_text(&d,0,"code",b,129,0)||
       !ext_text(&d,0,"link",b,2049,0))return 0;
    if(b[0]) {
        if(strncmp(b,"https://",8))return 0;
        if(!b[8]||b[8]=='/'||strpbrk(b,"\r\n\t <>\"")||strchr(b,'\\'))return 0;
    }
    int vs=xz_json_object_get(&d,0,"values"),seen=0;
    if(vs>=0) {
        if(!s||d.tokens[vs].type!=XZ_JSON_OBJECT)return 0;
        for(int k=d.tokens[vs].first_child;k>=0;) {
            char key[33];int f,v=d.tokens[k].next_sibling;
            if(v<0||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||
               (f=ext_field_index(s,key))<0||(seen&(1<<f))||!strcmp(s->fields[f].type,"password"))return 0;
            if(ext_transport_capacity(s,f)) {
                if(f!=s->structured.response||!ext_transport_text(&d,vs,key,s,f))return 0;
            } else if(!ext_text(&d,vs,key,b,sizeof(b),0)||!ext_value(s,f,b))return 0;
            seen|=1<<f;k=d.tokens[v].next_sibling;
        }
    }
    int options=xz_json_object_get(&d,0,"options"),total=0;
    if(options>=0) {
        if(!s||d.tokens[options].type!=XZ_JSON_OBJECT)return 0;
        for(int k=d.tokens[options].first_child;k>=0;) {
            char key[33];int f,a=d.tokens[k].next_sibling,count=0;
            if(a<0||decode_string(&d,k,key,sizeof(key))||!ext_raw_eq(&d,k,key)||
               (f=ext_field_index(s,key))<0||!s->fields[f].dynamic||d.tokens[a].type!=XZ_JSON_ARRAY)return 0;
            for(int prev=d.tokens[options].first_child;prev!=k;prev=d.tokens[d.tokens[prev].next_sibling].next_sibling)
                if(ext_raw_eq(&d,prev,key))return 0;
            for(int t=d.tokens[a].first_child;t>=0;t=d.tokens[t].next_sibling) {
                char value[EXT_OPTION_VALUE+1],label[EXT_OPTION_LABEL+1];
                if(++count>EXT_DYNAMIC_OPTIONS||++total>EXT_DYNAMIC_OPTIONS||
                   !ext_keys(&d,t,"|value||label|")||!ext_text(&d,t,"value",value,sizeof(value),1)||
                   !ext_text(&d,t,"label",label,sizeof(label),1)||!ext_value(s,f,value))return 0;
                for(const unsigned char*p=(const unsigned char*)label;*p;p++)if(*p<32||*p==127)return 0;
                for(int prev=d.tokens[a].first_child;prev!=t;prev=d.tokens[prev].next_sibling) {
                    char old[EXT_OPTION_VALUE+1];if(!ext_text(&d,prev,"value",old,sizeof(old),1)||!strcmp(old,value))return 0;
                }
            }
            k=d.tokens[a].next_sibling;
        }
    }
    return 1;
}
static int ext_public(const char*line) {return ext_public_schema(line,NULL);}
#endif

/* Same extension parser/tokenizer. Optional model declaration metadata only;
 * this never supplies a connection, caps authority, or executable mapping. */
#ifndef LX06_APP_EXTENSION_MODEL_H
#define LX06_APP_EXTENSION_MODEL_H
/* Match the existing private preparation budget. This is not an approved
 * production resource limit and never enlarges the regex capture carrier. */
#define EXT_MODEL_FIELDS 8
/* Same bounded UTF-8 predicate as the original cfg reader. This validates
 * bytes around the shipped tokenizer; it does not parse JSON or headers. */
static int ext_model_utf8(const char*s,size_t n) {
    for(size_t i=0;i<n;) {
        unsigned char a=(unsigned char)s[i++];
        if(a<128){if(!a||(a<32&&a!='\n'&&a!='\r'&&a!='\t'))return 0;continue;}
        unsigned need,cp,minimum;
        if(a>=0xc2&&a<=0xdf){need=1;cp=a&31;minimum=128;}
        else if(a>=0xe0&&a<=0xef){need=2;cp=a&15;minimum=2048;}
        else if(a>=0xf0&&a<=0xf4){need=3;cp=a&7;minimum=65536;}
        else return 0;
        if(i+need>n)return 0;
        while(need--){unsigned char b=(unsigned char)s[i++];if((b&0xc0)!=0x80)return 0;cp=(cp<<6)|(b&63);}
        if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;
    }return 1;
}
static int ext_model_id(const char*s) {
    size_t n=strlen(s);return n&&n<=64&&s[0]>='a'&&s[0]<='z'&&strcmp(s,"other")&&
        strspn(s,"abcdefghijklmnopqrstuvwxyz0123456789_.-")==n;
}
static int ext_model_slot(const char*s) {
    size_t n=strlen(s);return n&&n<=16&&strspn(s,"abcdefghijklmnopqrstuvwxyz_")==n;
}
static int ext_model_bool(const xz_json_doc*d,int o,const char*key) {
    int t=xz_json_object_get(d,o,key);return t<0||(d->tokens[t].type==XZ_JSON_PRIMITIVE&&
        (ext_raw_eq(d,t,"true")||ext_raw_eq(d,t,"false")));
}
static int ext_model_int(const xz_json_doc*d,int o,const char*key,double*out) {
    int t=xz_json_object_get(d,o,key);char raw[65];
    if(t<0||d->tokens[t].type!=XZ_JSON_PRIMITIVE)return 0;
    size_t n=d->tokens[t].end-d->tokens[t].start;if(!n||n>=sizeof(raw))return 0;
    memcpy(raw,d->source+d->tokens[t].start,n);raw[n]=0;
    const char*p=raw;if(*p=='-')p++;
    if(*p=='0')p++;else {if(*p<'1'||*p>'9')return 0;while(isdigit((unsigned char)*p))p++;}
    return !*p&&ext_decimal(raw,out);
}
static int ext_model_string(const xz_json_doc*d,int t,char*out,size_t cap) {
    if(t<0||decode_string(d,t,out,cap))return 0;
    for(size_t i=d->tokens[t].start;i<d->tokens[t].end;i++)if(d->source[i]=='\\') {
        if(d->source[i+1]=='u') {unsigned cp;
            if(hex4(d->source+i+2,&cp)||!cp||(cp>=0xd800&&cp<=0xdfff))return 0;i+=5;
        }else i++;
    }return ext_model_utf8(out,strlen(out));
}
static int ext_model_refs(const xz_json_doc*d,int o,const char*key) {
    int a=xz_json_object_get(d,o,key);if(a<0)return 1;
    if(d->tokens[a].type!=XZ_JSON_ARRAY)return 0;int count=0;
    for(int t=d->tokens[a].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char id[65],prior[65];
        if(++count>16||!ext_model_string(d,t,id,sizeof(id))||!ext_model_id(id))return 0;
        for(int p=d->tokens[a].first_child;p!=t;p=d->tokens[p].next_sibling)
            if(!ext_model_string(d,p,prior,sizeof(prior))||!strcmp(id,prior))return 0;
    }return 1;
}
static int ext_model_field(const xz_json_doc*d,int t) {
    char type[16],normalization[32];double min,max;
    if(!ext_keys(d,t,"|type||min||max||required||normalization||values||minItems||maxItems||items|")||
       !ext_text(d,t,"type",type,sizeof(type),1)||!ext_model_bool(d,t,"required")||
       !ext_text(d,t,"normalization",normalization,sizeof(normalization),0)||
       (*normalization&&strcmp(normalization,"none")&&strcmp(normalization,"date_iso8601")))return 0;
    int date=!strcmp(normalization,"date_iso8601");
    if(date&&strcmp(type,"string"))return 0;
    int values=xz_json_object_get(d,t,"values");
    if(strcmp(type,"enum")&&values>=0)return 0;
    if(strcmp(type,"array")&&(xz_json_object_get(d,t,"items")>=0||
       xz_json_object_get(d,t,"minItems")>=0||xz_json_object_get(d,t,"maxItems")>=0))return 0;
    if(!strcmp(type,"string"))return xz_json_object_get(d,t,"min")<0&&
        ext_model_int(d,t,"max",&max)&&max>=1&&max<=256&&(!date||max==10);
    if(!strcmp(type,"int"))return ext_model_int(d,t,"min",&min)&&ext_model_int(d,t,"max",&max)&&
        min<=max&&min>=-2147483648.0&&max<=2147483647.0;
    if(xz_json_object_get(d,t,"min")>=0||xz_json_object_get(d,t,"max")>=0)return 0;
    if(!strcmp(type,"bool"))return 1;
    if(!strcmp(type,"array")) {
        int items=xz_json_object_get(d,t,"items");char item_type[16];double item_max;
        return ext_keys(d,items,"|type||max|")&&ext_text(d,items,"type",item_type,sizeof(item_type),1)&&
            !strcmp(item_type,"string")&&ext_model_int(d,items,"max",&item_max)&&item_max>=1&&item_max<=256&&
            ext_model_int(d,t,"minItems",&min)&&ext_model_int(d,t,"maxItems",&max)&&min>=1&&min<=max&&max<=16;
    }
    if(strcmp(type,"enum")||values<0||d->tokens[values].type!=XZ_JSON_ARRAY||d->tokens[values].first_child<0)return 0;
    int kind=0;
    for(int v=d->tokens[values].first_child;v>=0;v=d->tokens[v].next_sibling) {
        char value[257],prior[257];int next=d->tokens[v].type;
        if(kind&&next!=kind)return 0;kind=next;
        if(kind==XZ_JSON_STRING) {if(!ext_model_string(d,v,value,sizeof(value))||!*value)return 0;}
        else {
            /* Reuse the bounded integer checker with this same token. */
            size_t n=d->tokens[v].end-d->tokens[v].start;
            if(kind!=XZ_JSON_PRIMITIVE||!n||n>64)return 0;
            memcpy(value,d->source+d->tokens[v].start,n);value[n]=0;
            const char*p=value;if(*p=='-')p++;
            if(*p=='0')p++;else{if(*p<'1'||*p>'9')return 0;while(isdigit((unsigned char)*p))p++;}
            if(*p||!ext_decimal(value,&max)||max<-2147483648.0||max>2147483647.0)return 0;
            snprintf(value,sizeof(value),"%.0f",max==0?0:max);
        }
        for(int p=d->tokens[values].first_child;p!=v;p=d->tokens[p].next_sibling) {
            if(kind==XZ_JSON_STRING){if(!ext_model_string(d,p,prior,sizeof(prior)))return 0;}
            else {size_t n=d->tokens[p].end-d->tokens[p].start;memcpy(prior,d->source+d->tokens[p].start,n);prior[n]=0;
                if(!ext_decimal(prior,&max))return 0;snprintf(prior,sizeof(prior),"%.0f",max==0?0:max);}
            if(!strcmp(value,prior))return 0;
        }
    }return 1;
}
static int ext_model_valid(struct ext_schema*s) {
    const xz_json_doc*d=&s->doc;int list=xz_json_object_get(d,0,"model_intents");
    if(list<0)return 1;
    if(d->tokens[list].type!=XZ_JSON_ARRAY||!ext_model_utf8(d->source+d->tokens[list].start,
        d->tokens[list].end-d->tokens[list].start))return 0;int count=0;
    for(int t=d->tokens[list].first_child;t>=0;t=d->tokens[t].next_sibling) {
        char id[65],prior[65],name[81];
        if(++count>16||!ext_keys(d,t,"|intent||examples||fields||allow_model||delegate_model||intent_connections||extract_connections||mapping|")||
           !ext_text(d,t,"intent",id,sizeof(id),1)||!ext_model_id(id)||
           !ext_model_bool(d,t,"allow_model")||!ext_model_bool(d,t,"delegate_model")||
           !ext_model_refs(d,t,"intent_connections")||!ext_model_refs(d,t,"extract_connections"))return 0;
        for(int p=d->tokens[list].first_child;p!=t;p=d->tokens[p].next_sibling)
            if(!ext_text(d,p,"intent",prior,sizeof(prior),1)||!strcmp(id,prior))return 0;
        int examples=xz_json_object_get(d,t,"examples"),fields=xz_json_object_get(d,t,"fields");
        if(examples<0||d->tokens[examples].type!=XZ_JSON_ARRAY||d->tokens[examples].first_child<0||
           fields<0||d->tokens[fields].type!=XZ_JSON_OBJECT)return 0;
        for(int e=d->tokens[examples].first_child;e>=0;e=d->tokens[e].next_sibling) {
            char text[513];if(!ext_model_string(d,e,text,sizeof(text)))return 0;
            if(strspn(text," \t\r\n")==strlen(text))return 0;
        }
        int field_count=0;
        for(int k=d->tokens[fields].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;
            if(++field_count>EXT_MODEL_FIELDS||!ext_model_string(d,k,name,17)||!ext_model_slot(name)||!ext_raw_eq(d,k,name)||
               !ext_model_field(d,v))return 0;
            for(int p=d->tokens[fields].first_child;p!=k;p=d->tokens[d->tokens[p].next_sibling].next_sibling)
                if(ext_raw_eq(d,p,name))return 0;
            k=d->tokens[v].next_sibling;
        }
        int map=xz_json_object_get(d,t,"mapping");char kind[16],app[81],op[81];
        if(!ext_text(d,map,"kind",kind,sizeof(kind),1))return 0;
        int params;
        if(!strcmp(kind,"op")) {
            if(!ext_keys(d,map,"|kind||app||op||params|")||
               !ext_text(d,map,"app",app,sizeof(app),1)||!ext_text(d,map,"op",op,sizeof(op),1))return 0;
            params=xz_json_object_get(d,map,"params");
        } else if(!strcmp(kind,"flow")) {
            char flow[129];
            if(!ext_keys(d,map,"|kind||name||inputs|")||!ext_text(d,map,"name",flow,sizeof(flow),1))return 0;
            params=xz_json_object_get(d,map,"inputs");
        } else return 0;
        if(params<0||d->tokens[params].type!=XZ_JSON_OBJECT)return 0;
        int args=0;
        for(int k=d->tokens[params].first_child;k>=0;) {
            int v=d->tokens[k].next_sibling;char slot[17],constant[257];
            if(++args>32||!ext_model_string(d,k,name,sizeof(name))||!*name||!ext_raw_eq(d,k,name))return 0;
            for(int p=d->tokens[params].first_child;p!=k;p=d->tokens[d->tokens[p].next_sibling].next_sibling)
                if(ext_raw_eq(d,p,name))return 0;
            if(d->tokens[v].type==XZ_JSON_STRING){if(!ext_model_string(d,v,constant,sizeof(constant))||!*constant)return 0;}
            else if(!ext_keys(d,v,"|slot|")||!ext_text(d,v,"slot",slot,sizeof(slot),1)||
                    xz_json_object_get(d,fields,slot)<0)return 0;
            k=d->tokens[v].next_sibling;
        }
    }return 1;
}
#endif

/* Small provider-neutral primitives for uploadable account adapters.
 * OAuth endpoints, client registration, scopes, refresh policy and inference
 * mapping belong in scripts. Secrets enter through stdin/private files, never
 * shell-expanded command lines. Uses stock OpenSSL 1.0 ABI for crypto only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../../hooks/robust/keeper_client.h"
/* Only this standalone helper needs larger model-catalog documents. Existing
 * Doubao/Xiaozhi builds retain the JSON parser's default 1024-token limit. */
#define XZ_JSON_MAX_TOKENS 8192
#include "../doubao/src/json.c"

typedef struct rsa_st RSA;
extern int RAND_bytes(unsigned char*,int);
extern unsigned char*SHA256(const unsigned char*,size_t,unsigned char*);
extern RSA*d2i_RSAPublicKey(RSA**,const unsigned char**,long);
extern int RSA_verify(int,const unsigned char*,unsigned int,const unsigned char*,unsigned int,RSA*);
extern void RSA_free(RSA*);
#define NID_SHA256 672
#define CAP 32768
static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static int read_data(const char*path,char*out,size_t cap) {
    int fd=path?open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK):0;
    if(fd<0)return -1;
    size_t used=0;ssize_t n;
    while(used<cap-1&&(n=read(fd,out+used,cap-1-used))>0)used+=(size_t)n;
    char extra;int overflow=read(fd,&extra,1)>0;
    if(path)close(fd);
    if(n<0||overflow)return -1;
    out[used]=0;return (int)used;
}
static void quote_to(FILE *f,const char*s,size_t n) {
    fputc('"',f);
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c=='"'||c=='\\'){fputc('\\',f);fputc(c,f);}
        else if(c<32)fprintf(f,"\\u%04x",c);
        else fputc(c,f);
    }
    fputc('"',f);
}
static void quote(const char*s,size_t n) { quote_to(stdout,s,n); }
static void b64(const unsigned char*s,size_t n,char*out) {
    size_t j=0;unsigned acc=0,bits=0;
    for(size_t i=0;i<n;i++){acc=(acc<<8)|s[i];bits+=8;while(bits>=6){bits-=6;out[j++]=alphabet[(acc>>bits)&63];}}
    if(bits)out[j++]=alphabet[(acc<<(6-bits))&63];out[j]=0;
}
static int unb64(const char*s,unsigned char*out,size_t cap) {
    unsigned acc=0,bits=0;size_t used=0;
    for(;*s;s++) {
        const char*p=strchr(alphabet,*s);if(!p)return -1;
        acc=(acc<<6)|(unsigned)(p-alphabet);bits+=6;
        if(bits>=8){bits-=8;if(used>=cap)return -1;out[used++]=(unsigned char)(acc>>bits);}
    }
    if(bits>=6||(bits&&(acc&((1U<<bits)-1))))return -1;
    return (int)used;
}
static int string(const xz_json_doc*d,int o,const char*k,char*out,size_t cap) {
    return xz_json_get_string(d,o,k,out,cap)==0;
}
static int unique(const xz_json_doc*d) {
    /* Reject duplicate JSON keys and decoded NUL. OAuth data must have one
     * interpretation across helper, shell and browser. */
    for(int o=0;o<d->token_count;o++)if(d->tokens[o].type==XZ_JSON_OBJECT) {
        for(int k=d->tokens[o].first_child;k>=0;) {
            char a[256],b[256];int v=d->tokens[k].next_sibling;
            if(v<0||decode_string(d,k,a,sizeof(a)))return 0;
            for(int p=d->tokens[o].first_child;p!=k;) {
                if(decode_string(d,p,b,sizeof(b))||!strcmp(a,b))return 0;
                p=d->tokens[d->tokens[p].next_sibling].next_sibling;
            }
            k=d->tokens[v].next_sibling;
        }
    }
    return 1;
}
static int parse(xz_json_doc*d,const char*s) {
    if(xz_json_parse(d,s,strlen(s))||!unique(d))return 0;
    for(int t=0;t<d->token_count;t++)if(d->tokens[t].type==XZ_JSON_STRING)
        for(size_t i=d->tokens[t].start;i<d->tokens[t].end;i++)if(s[i]=='\\') {
            if(s[++i]=='u') {unsigned cp;if(hex4(s+i+1,&cp)||!cp||(cp>=0xd800&&cp<=0xdfff))return 0;i+=4;}
        }
    return 1;
}
static void encode(const char*s,size_t n) {
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr("-._~",c))putchar(c);
        else printf("%%%02X",c);
    }
}
static int hexval(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int decode(char*s) {
    char*w=s;
    while(*s) {
        unsigned char c=(unsigned char)*s++;
        if(c=='%') {if(!s[0]||!s[1]||hexval(s[0])<0||hexval(s[1])<0)return 0;c=(unsigned char)((hexval(s[0])<<4)|hexval(s[1]));s+=2;}
        else if(c=='+')c=' ';
        if(c<32||c==127)return 0;*w++=(char)c;
    }
    *w=0;return 1;
}
static int same(const char*a,const char*b) {
    size_t n=strlen(a);if(n!=strlen(b))return 0;unsigned x=0;
    for(size_t i=0;i<n;i++)x|=(unsigned char)a[i]^(unsigned char)b[i];return x==0;
}
/* Bind before creating the ready file. A temporary SSH local forward can
 * deliver a desktop/phone loopback callback; no runtime proxy is required. */
static int callback(int port,const char*path,const char*statefile,const char*ready) {
    char state[257];int n=read_data(statefile,state,sizeof(state));if(n<16||port<1024||port>65535||path[0]!='/')return 2;
    state[strcspn(state,"\r\n")]=0;
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener<0)return 3;
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);addr.sin_port=htons((unsigned short)port);
    int one=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if(bind(listener,(struct sockaddr*)&addr,sizeof(addr))||listen(listener,2)){close(listener);return 3;}
    int f=open(ready,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(f<0){close(listener);return 3;}close(f);
    int64_t end=lk_now_ms()+240000;int success=0;
    while(lk_now_ms()<end&&!success) {
        struct pollfd p={listener,POLLIN,0};if(poll(&p,1,250)<=0)continue;
        int peer=accept4(listener,NULL,NULL,SOCK_CLOEXEC|SOCK_NONBLOCK);if(peer<0)continue;
        char req[8193]={0};size_t used=0;int64_t until=lk_now_ms()+3000;
        while(used<sizeof(req)-1&&lk_now_ms()<until) {
            struct pollfd r={peer,POLLIN,0};if(poll(&r,1,100)<=0)continue;
            ssize_t got=read(peer,req+used,sizeof(req)-1-used);if(got<=0)break;used+=(size_t)got;req[used]=0;
            if(strstr(req,"\r\n\r\n"))break;
        }
        req[used]=0;char*uri=req+4,*space=strchr(uri,' '),*q=NULL;
        char*keys[16],*values[16];int count=0,valid=space&&!strncmp(req,"GET ",4);
        if(valid){*space=0;q=strchr(uri,'?');if(!q)valid=0;else{*q++=0;if(strcmp(uri,path))valid=0;}}
        if(valid) {
            char*save=NULL;
            for(char*t=strtok_r(q,"&",&save);t;t=strtok_r(NULL,"&",&save)) {
                char*value=strchr(t,'=');if(!value||count>=16){valid=0;break;}*value++=0;
                if(!decode(t)||!decode(value)){valid=0;break;}
                for(int i=0;i<count;i++)if(!strcmp(keys[i],t))valid=0;
                keys[count]=t;values[count++]=value;
            }
            int matched=0;for(int i=0;i<count;i++)if(!strcmp(keys[i],"state")&&same(values[i],state))matched=1;
            if(!matched)valid=0;
        }
        const char*reply=valid?"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\nAuthorization received. Return to XiaoAI WebAdmin.\n":
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid authorization callback.\n";
        send(peer,reply,strlen(reply),MSG_NOSIGNAL);close(peer);
        if(valid) {
            putchar('{');for(int i=0;i<count;i++){if(i)putchar(',');quote(keys[i],strlen(keys[i]));putchar(':');quote(values[i],strlen(values[i]));}puts("}");success=1;
        }
    }
    close(listener);unlink(ready);return success?0:4;
}
static size_t der_len(unsigned char*out,size_t n) {
    if(n<128){out[0]=(unsigned char)n;return 1;}
    if(n<256){out[0]=0x81;out[1]=(unsigned char)n;return 2;}
    out[0]=0x82;out[1]=(unsigned char)(n>>8);out[2]=(unsigned char)n;return 3;
}
static size_t der_int(unsigned char*out,const unsigned char*s,size_t n) {
    size_t z=0;out[z++]=2;int pad=s[0]&128;z+=der_len(out+z,n+(pad?1:0));if(pad)out[z++]=0;memcpy(out+z,s,n);return z+n;
}
/* RS256 verification is a general primitive, not a platform login flow.
 * Reject unsupported algorithms instead of accepting an unverified ID token. */
static int verify_jwt(const char*jwtfile,const char*jwksfile,const char*issuer,const char*audience,const char*noncefile) {
    char jwt[CAP],jwks[CAP],nonce[257],header[4097],payload[16385],sigtext[2049];
    if(read_data(jwtfile,jwt,sizeof(jwt))<0||read_data(jwksfile,jwks,sizeof(jwks))<0||read_data(noncefile,nonce,sizeof(nonce))<0)return 2;
    jwt[strcspn(jwt,"\r\n")]=0;nonce[strcspn(nonce,"\r\n")]=0;
    char*dot=strchr(jwt,'.'),*dot2=dot?strchr(dot+1,'.'):NULL;
    if(!dot||!dot2||strchr(dot2+1,'.')||strlen(dot2+1)>=sizeof(sigtext))return 3;
    strcpy(sigtext,dot2+1);unsigned char digest[32],signature[1536];SHA256((unsigned char*)jwt,(size_t)(dot2-jwt),digest);
    *dot=*dot2=0;
    int hn=unb64(jwt,(unsigned char*)header,sizeof(header)-1),pn=unb64(dot+1,(unsigned char*)payload,sizeof(payload)-1),sn=unb64(sigtext,signature,sizeof(signature));
    if(hn<1||pn<1||sn<1)return 3;header[hn]=0;payload[pn]=0;
    xz_json_doc h,p,j;if(!parse(&h,header)||!parse(&p,payload)||!parse(&j,jwks))return 3;
    char alg[32],kid[257],s[2049];
    if(!string(&h,0,"alg",alg,sizeof(alg))||strcmp(alg,"RS256")||!string(&h,0,"kid",kid,sizeof(kid)))return 3;
    int keys=xz_json_object_get(&j,0,"keys"),match=-1;
    if(keys<0||j.tokens[keys].type!=XZ_JSON_ARRAY)return 3;
    for(int t=j.tokens[keys].first_child;t>=0;t=j.tokens[t].next_sibling)
        if(string(&j,t,"kid",s,sizeof(s))&&!strcmp(s,kid)){if(match>=0)return 3;match=t;}
    if(match<0||!string(&j,match,"kty",s,sizeof(s))||strcmp(s,"RSA"))return 3;
    if(string(&j,match,"alg",s,sizeof(s))&&strcmp(s,"RS256"))return 3;
    if(string(&j,match,"use",s,sizeof(s))&&strcmp(s,"sig"))return 3;
    unsigned char modulus[1024],exponent[16],inner[1100],der[1110];int mn,en;
    if(!string(&j,match,"n",s,sizeof(s))||(mn=unb64(s,modulus,sizeof(modulus)))<256||
       !string(&j,match,"e",s,sizeof(s))||(en=unb64(s,exponent,sizeof(exponent)))<1)return 3;
    size_t il=der_int(inner,modulus,(size_t)mn);il+=der_int(inner+il,exponent,(size_t)en);
    der[0]=0x30;size_t dl=1+der_len(der+1,il);memcpy(der+dl,inner,il);dl+=il;
    const unsigned char*dp=der;RSA*rsa=d2i_RSAPublicKey(NULL,&dp,(long)dl);if(!rsa)return 3;
    int verified=RSA_verify(NID_SHA256,digest,32,signature,(unsigned)sn,rsa);RSA_free(rsa);if(verified!=1)return 3;
    if(!string(&p,0,"iss",s,sizeof(s))||strcmp(s,issuer)||!string(&p,0,"nonce",s,sizeof(s))||!same(s,nonce))return 3;
    int aud=xz_json_object_get(&p,0,"aud"),audok=0,multiple=0;
    if(aud>=0&&p.tokens[aud].type==XZ_JSON_ARRAY) {
        int count=0;for(int t=p.tokens[aud].first_child;t>=0;t=p.tokens[t].next_sibling){count++;if(!decode_string(&p,t,s,sizeof(s))&&!strcmp(s,audience))audok=1;}
        multiple=count>1;
    } else if(string(&p,0,"aud",s,sizeof(s))&&!strcmp(s,audience))audok=1;
    if(!audok||(multiple&&(!string(&p,0,"azp",s,sizeof(s))||strcmp(s,audience))))return 3;
    int exp,nbf;time_t now=time(NULL);
    if(xz_json_get_int(&p,0,"exp",&exp)||exp<=now)return 3;
    if(xz_json_object_get(&p,0,"nbf")>=0&&(xz_json_get_int(&p,0,"nbf",&nbf)||nbf>now+60))return 3;
    if(!string(&p,0,"sub",s,sizeof(s))||!s[0])return 3;
    puts(payload);return 0;
}
/* SSE framing and field/event names are supplied by the script. A premature
 * EOF is failure, not a complete model answer. Text accumulation is bounded. */
static int sse(const char*typekey,const char*delta,const char*textkey,const char*done,const char*fail,int streaming) {
    char line[CAP],event[CAP],answer[16385],type[256],piece[CAP];size_t used=0,al=0,total=0;
    int completed=0;
    while(fgets(line,sizeof(line),stdin)) {
        size_t z=strlen(line);total+=z;if(total>1024*1024||(!strchr(line,'\n')&&!feof(stdin)))return 3;
        if(!strcmp(line,"\n")||!strcmp(line,"\r\n")) {
            if(!used)continue;event[used]=0;xz_json_doc d;
            if(!strcmp(event,"[DONE]")){used=0;continue;}
            if(!parse(&d,event)||!string(&d,0,typekey,type,sizeof(type)))return 3;
            if(!strcmp(type,delta)) {
                if(completed||!string(&d,0,textkey,piece,sizeof(piece)))return 3;
                size_t n=strlen(piece);if(al+n>=sizeof(answer))return 3;memcpy(answer+al,piece,n);al+=n;
                /* Flush bounded NDJSON deltas as soon as a complete SSE event
                 * arrives. The shell emits done only after HTTP status and
                 * parser exit both succeed; never infer completion from EOF. */
                if(streaming)for(size_t at=0;at<n;){
                    size_t z=n-at;if(z>512){z=512;while(z&&((unsigned char)piece[at+z]&0xc0)==0x80)z--;}
                    if(!z)return 3;fputs("{\"delta\":",stdout);quote(piece+at,z);puts("}");fflush(stdout);if(ferror(stdout))return 3;at+=z;
                }
            } else if(!strcmp(type,done))completed=1;
            else {
                char failures[1024],*save=NULL;if(strlen(fail)>=sizeof(failures))return 2;strcpy(failures,fail);
                for(char*t=strtok_r(failures,",",&save);t;t=strtok_r(NULL,",",&save))if(!strcmp(t,type))return 3;
            }
            used=0;continue;
        }
        if(!strncmp(line,"data:",5)) {
            char*p=line+5;if(*p==' ')p++;z=strcspn(p,"\r\n");
            if(used+z+1>=sizeof(event))return 3;if(used)event[used++]='\n';memcpy(event+used,p,z);used+=z;
        }
    }
    if(ferror(stdin)||!completed||used||!al)return 3;
    if(!streaming){fputs("{\"text\":",stdout);quote(answer,al);puts("}");}return 0;
}
static int model_list(int argc,char**argv) {
    /* Catalogs contain long instructions unrelated to the UI. Allocate the
     * bounded 1 MiB body only for this short-lived command, not for secrets,
     * SSE events or the always-running audio engine. Never execute metadata. */
    size_t cap=1024*1024+1;
    char *body=malloc(cap);xz_json_doc *d=malloc(sizeof(*d));int result=3;
    if(!body||!d)goto done;
    int length=read_data(NULL,body,cap);
    if(length<0||memchr(body,0,(size_t)length)||!parse(d,body))goto done;
    int items=xz_json_object_get(d,0,argv[2]);
    if(items<0||d->tokens[items].type!=XZ_JSON_ARRAY)goto done;
    char id[257],label[513],filter[129];int count=0,shown=0;
    for(int t=d->tokens[items].first_child;t>=0;t=d->tokens[t].next_sibling) {
        if(++count>64||d->tokens[t].type!=XZ_JSON_OBJECT)goto done;
        if(argc==7&&(!string(d,t,argv[5],filter,sizeof(filter))||strcmp(filter,argv[6])))continue;
        if(!string(d,t,argv[3],id,sizeof(id))||!id[0])goto done;
        if(!string(d,t,argv[4],label,sizeof(label)))label[0]=0;
        if(strpbrk(id,"\r\n\t")||strpbrk(label,"\r\n\t"))goto done;
        printf("%s%s%s\n",id,label[0]?" · ":"",label);shown++;
    }
    result=shown?0:3;
done:
    free(d);free(body);return result;
}
/* Transform only public catalog IDs/labels, never provider instructions, into
 * one bounded extension event. Validate the entire response before publishing
 * anything so an oversized/invalid refresh cannot replace a working menu. */
static int model_options(int argc,char**argv) {
    const char *field=argv[2];size_t fieldlen=strlen(field);
    if(!fieldlen||fieldlen>32||!((*field>='a'&&*field<='z')||(*field>='A'&&*field<='Z')))return 2;
    for(const char*p=field;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))return 2;
    char *body=malloc(1024*1024+1),*event=NULL;
    xz_json_doc *d=malloc(sizeof(*d));int rc=3,count=0,seen=0;
    char ids[64][129],labels[64][161],filter[129];size_t used=0;FILE *out=NULL;
    if(!body||!d)goto done;
    int length=read_data(NULL,body,1024*1024+1);
    if(length<0||memchr(body,0,(size_t)length)||!parse(d,body))goto done;
    int mappings=0;
    if(argc==6||argc==8)mappings=1;
    else if(argc>=13&&(argc-3)%5==0)mappings=(argc-3)/5;
    else goto done;
    for(int m=0;m<mappings;m++) {
        int base=3+m*5;
        const char *root,*idkey,*labelkey,*filterkey=NULL,*filtervalue=NULL;
        if(mappings==1&&(argc==6||argc==8)) {
            root=argv[3];idkey=argv[4];labelkey=argv[5];
            if(argc==8){filterkey=argv[6];filtervalue=argv[7];}
        } else {
            root=argv[base];idkey=argv[base+1];labelkey=argv[base+2];
            if(strcmp(argv[base+3],"-")){filterkey=argv[base+3];filtervalue=argv[base+4];}
        }
        int items=xz_json_object_get(d,0,root),base_count=count;
        if(items<0||d->tokens[items].type!=XZ_JSON_ARRAY)continue;
        for(int t=d->tokens[items].first_child;t>=0;t=d->tokens[t].next_sibling) {
            /* Several mappings may inspect the same array (id/name, id/display
             * name, slug). Bound work separately from the 64 unique options;
             * counting revisits as new models rejects a valid 17-model list. */
            if(++seen>64*6||d->tokens[t].type!=XZ_JSON_OBJECT)goto done;
            if(filterkey&&(!string(d,t,filterkey,filter,sizeof(filter))||strcmp(filter,filtervalue)))continue;
            char id[129],label[161];
            if(!string(d,t,idkey,id,sizeof(id))||!id[0]){if(m==0)goto done;continue;}
            if(!string(d,t,labelkey,label,sizeof(label)))label[0]=0;
            if(!label[0])strcpy(label,id);
            for(const unsigned char*p=(const unsigned char*)id;*p;p++)if(*p<32||*p==127)goto done;
            for(const unsigned char*p=(const unsigned char*)label;*p;p++)if(*p<32||*p==127)goto done;
            int duplicate=0;for(int i=0;i<count;i++)if(!strcmp(ids[i],id)){if(i>=base_count)goto done;duplicate=1;break;}
            if(duplicate)continue;
            if(count>=64)goto done;
            strcpy(ids[count],id);strcpy(labels[count],label);count++;
        }
    }
    if(!count)goto done;
    out=open_memstream(&event,&used);if(!out)goto done;
    fputs("{\"options\":{",out);quote_to(out,field,fieldlen);fputs(":[",out);
    for(int i=0;i<count;i++) {
        if(i)fputc(',',out);
        fputs("{\"value\":",out);quote_to(out,ids[i],strlen(ids[i]));
        fputs(",\"label\":",out);quote_to(out,labels[i],strlen(labels[i]));fputc('}',out);
    }
    fprintf(out,"]},\"message\":\"已获取 %d 个可用选项，请展开下拉选择后保存。\"}\n",count);
    int bad=ferror(out);if(fclose(out))bad=1;out=NULL;
    if(bad||used>8192)goto done;
    if(fwrite(event,1,used,stdout)!=used)goto done;
    rc=0;
done:
    if(out)fclose(out);free(event);free(d);free(body);return rc;
}
int main(int argc,char**argv) {
    if(argc<2)return 2;char buf[CAP],out[CAP];int n;
    if(!strcmp(argv[1],"random")&&argc==2) {
        unsigned char b[32];if(RAND_bytes(b,sizeof(b))!=1)return 3;b64(b,sizeof(b),out);puts(out);return 0;
    }
    if(!strcmp(argv[1],"callback")&&argc==6)return callback(atoi(argv[2]),argv[3],argv[4],argv[5]);
    if(!strcmp(argv[1],"jwt-verify")&&argc==7)return verify_jwt(argv[2],argv[3],argv[4],argv[5],argv[6]);
    if((!strcmp(argv[1],"sse")||!strcmp(argv[1],"sse-stream"))&&argc==7){
        int streaming=!strcmp(argv[1],"sse-stream");
        int rc=sse(argv[2],argv[3],argv[4],argv[5],argv[6],streaming);
        /* The FIFO holder may outlive a failed parser while curl is writing.
         * Notify the core immediately so it cancels the whole request group,
         * rather than waiting for the HTTP timeout. No raw provider error. */
        if(rc&&streaming){puts("{\"error\":true}");fflush(stdout);}
        return rc;
    }
    if(!strcmp(argv[1],"list")&&(argc==5||argc==7))return model_list(argc,argv);
    if(!strcmp(argv[1],"list-options")&&(argc==6||argc==8||(argc>=13&&argc<=33&&(argc-3)%5==0)))return model_options(argc,argv);
    if(!strcmp(argv[1],"object")||!strcmp(argv[1],"form")) {
        int form=!strcmp(argv[1],"form");if(!form)putchar('{');
        if(argc>34||argc%2)return 2;
        for(int i=2;i<argc;i+=2) {
            n=read_data(argv[i+1],buf,sizeof(buf));if(n<0)return 3;
            if(i>2)putchar(form?'&':',');
            if(form){encode(argv[i],strlen(argv[i]));putchar('=');encode(buf,(size_t)n);}
            else {quote(argv[i],strlen(argv[i]));putchar(':');quote(buf,(size_t)n);}
        }
        if(!form)puts("}");return 0;
    }
    n=read_data(NULL,buf,sizeof(buf));if(n<0)return 3;
    if(!strcmp(argv[1],"quote")&&argc==2){quote(buf,(size_t)n);putchar('\n');return 0;}
    if(!strcmp(argv[1],"urlencode")&&argc==2){encode(buf,(size_t)n);return 0;}
    if(!strcmp(argv[1],"clip")&&argc==3) {
        int limit=atoi(argv[2]);if(limit<0||limit>CAP-1)return 2;
        if(n>limit){n=limit;while(n>0&&((unsigned char)buf[n]&0xc0)==0x80)n--;}
        fwrite(buf,1,(size_t)n,stdout);return 0;
    }
    if(!strcmp(argv[1],"pkce")&&argc==2) {
        unsigned char b[32];buf[strcspn(buf,"\r\n")]=0;
        if(strlen(buf)<43||strlen(buf)>128)return 2;
        SHA256((unsigned char*)buf,strlen(buf),b);b64(b,sizeof(b),out);puts(out);return 0;
    }
    if(!strcmp(argv[1],"get")&&argc==3) {
        xz_json_doc d;if(!parse(&d,buf))return 3;int t=xz_json_object_get(&d,0,argv[2]);if(t<0)return 4;
        if(d.tokens[t].type==XZ_JSON_STRING) {if(decode_string(&d,t,out,sizeof(out)))return 3;fputs(out,stdout);}
        else fwrite(buf+d.tokens[t].start,1,d.tokens[t].end-d.tokens[t].start,stdout);
        return 0;
    }
    return 2;
}

#define _GNU_SOURCE
#include "phone.h"
#include "../protocol.h"
#include "../io/compat.h"
#include "support/xiaozhi/ws_frame.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Compile with modern headers, import only the stock OpenSSL 1.0 public ABI.
 * No implementation addresses, device identity, credential or TLS bypass. */
#ifdef XZ_OPENSSL_1_0_COMPAT
#undef SSL_get_peer_certificate
#undef SSL_library_init
#undef SSL_load_error_strings
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
/* Old target headers already provide _STACK and typed SAN accessors. */
#undef sk_num
#undef sk_value
#undef sk_GENERAL_NAME_num
#undef sk_GENERAL_NAME_value
extern X509 *SSL_get_peer_certificate(const SSL *);
extern int sk_num(const OPENSSL_STACK *);
extern void *sk_value(const OPENSSL_STACK *,int);
#define sk_GENERAL_NAME_num(s) sk_num((const OPENSSL_STACK *)(s))
#define sk_GENERAL_NAME_value(s,i) ((GENERAL_NAME *)sk_value((const OPENSSL_STACK *)(s),(i)))
#endif
extern unsigned char *phone_asn1_data(ASN1_STRING *) __asm__("ASN1_STRING_data");
static const unsigned char *asn1_data(const ASN1_STRING *s){return phone_asn1_data((ASN1_STRING *)s);}
static void tls_init(void){extern int SSL_library_init(void);extern void SSL_load_error_strings(void);(void)SSL_library_init();SSL_load_error_strings();}
#else
static const unsigned char *asn1_data(const ASN1_STRING *s){return ASN1_STRING_get0_data(s);}
static void tls_init(void){}
#endif

#define PHONE_PATH "/lx06/phone-audio/v1"
#define QCAP 8u
#define WCAP (sizeof(dc_wire_header)+DC_WIRE_CAP)
#define FCAP (WCAP+8u)
#define JSON_CAP 4096u
#define PHASE_MS 5000u
#define WRITE_MS 2000u
#define COMMAND_MS 30000u
#define MIN_WALL_MS UINT64_C(1704067200000)
#define MAX_WALL_MS UINT64_C(4102444800000)

/* Small, bounded strict JSON tree. Object keys are decoded before duplicate
 * comparison; UTF-8, surrogate pairs, numbers, depths and entry counts are
 * validated, unlike the intentionally permissive legacy adapter tokenizer. */
enum jt {JOBJ,JARR,JSTR,JNUM,JTRUE,JFALSE,JNULL};
struct jnode {enum jt type;int first,next;const char *str;};
struct json {const unsigned char *s;size_t n,at,used;unsigned count;struct jnode v[128];char arena[8192];};
static void space(struct json *j){while(j->at<j->n&&j->s[j->at]&&strchr(" \r\n\t",j->s[j->at]))j->at++;}
static int hex(unsigned c){return c>='0'&&c<='9'?(int)c-'0':c>='a'&&c<='f'?(int)c-'a'+10:c>='A'&&c<='F'?(int)c-'A'+10:-1;}
static int utf8(const unsigned char *s,size_t n,size_t *used,uint32_t *cp){
    if(!n)return -1;
    unsigned c=s[0],k;uint32_t v,min;
    if(c<0x80){*used=1;*cp=c;return 0;}
    if(c>=0xc2&&c<=0xdf){k=2;v=c&31;min=0x80;}
    else if(c>=0xe0&&c<=0xef){k=3;v=c&15;min=0x800;}
    else if(c>=0xf0&&c<=0xf4){k=4;v=c&7;min=0x10000;}else return -1;
    if(n<k)return -1;
    for(unsigned i=1;i<k;i++){if((s[i]&0xc0)!=0x80)return -1;v=(v<<6)|(s[i]&63);}
    if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return -1;
    *used=k;*cp=v;return 0;
}
static int putcp(struct json *j,uint32_t c){
    unsigned char b[4];size_t n;
    if(c<0x80){b[0]=(unsigned char)c;n=1;}
    else if(c<0x800){b[0]=0xc0|(c>>6);b[1]=0x80|(c&63);n=2;}
    else if(c<0x10000){b[0]=0xe0|(c>>12);b[1]=0x80|((c>>6)&63);b[2]=0x80|(c&63);n=3;}
    else{b[0]=0xf0|(c>>18);b[1]=0x80|((c>>12)&63);b[2]=0x80|((c>>6)&63);b[3]=0x80|(c&63);n=4;}
    if(j->used+n>=sizeof j->arena)return -1;
    memcpy(j->arena+j->used,b,n);j->used+=n;return 0;
}
static int u4(struct json *j,uint32_t *v){
    if(j->n-j->at<4)return -1;*v=0;
    for(unsigned i=0;i<4;i++){int h=hex(j->s[j->at++]);if(h<0)return -1;*v=(*v<<4)|(unsigned)h;}return 0;
}
static const char *quoted(struct json *j){
    if(j->at>=j->n||j->s[j->at++]!='"')return NULL;
    size_t start=j->used;unsigned units=0;
    while(j->at<j->n){
        uint32_t cp;size_t take;unsigned c=j->s[j->at++];
        if(c=='"'){if(j->used>=sizeof j->arena)return NULL;j->arena[j->used++]=0;return j->arena+start;}
        if(c<32)return NULL;
        if(c=='\\'){
            if(j->at==j->n)return NULL;c=j->s[j->at++];
            if(c=='u'){
                if(u4(j,&cp))return NULL;
                if(cp>=0xd800&&cp<=0xdbff){uint32_t lo;if(j->n-j->at<6||j->s[j->at++]!='\\'||j->s[j->at++]!='u'||u4(j,&lo)||lo<0xdc00||lo>0xdfff)return NULL;cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);}
                else if(cp>=0xdc00&&cp<=0xdfff)return NULL;
            }else if(c=='b')cp=8;else if(c=='f')cp=12;else if(c=='n')cp=10;else if(c=='r')cp=13;else if(c=='t')cp=9;
            else if(c=='"'||c=='\\'||c=='/')cp=c;else return NULL;
        }else{j->at--;if(utf8(j->s+j->at,j->n-j->at,&take,&cp))return NULL;j->at+=take;}
        /* NUL has no legitimate use in this protocol and cannot be represented
         * by the following C schema checks without ambiguity. */
        if(!cp||++units>512||putcp(j,cp))return NULL;
    }return NULL;
}
static int value(struct json *j,unsigned depth){
    space(j);if(depth>4||j->count==128||j->at==j->n)return -1;
    int ix=(int)j->count++;struct jnode *v=&j->v[ix];v->first=v->next=-1;v->str=NULL;
    unsigned c=j->s[j->at];
    if(c=='{'||c=='['){
        int obj=c=='{',last=-1;unsigned entries=0;v->type=obj?JOBJ:JARR;j->at++;space(j);
        if(j->at<j->n&&j->s[j->at]==(obj?'}':']')){j->at++;return ix;}
        for(;;){
            if(++entries>32)return -1;
            if(obj){space(j);const char *key=quoted(j);if(!key)return -1;
                for(int p=v->first;p>=0;p=j->v[j->v[p].next].next)if(!strcmp(j->v[p].str,key))return -1;
                if(j->count==128)return -1;int k=(int)j->count++;j->v[k]=(struct jnode){JSTR,-1,-1,key};
                if(last<0)v->first=k;else j->v[last].next=k;last=k;
                space(j);if(j->at==j->n||j->s[j->at++]!=':')return -1;
            }
            int child=value(j,depth+1);if(child<0)return -1;
            if(last<0)v->first=child;else j->v[last].next=child;last=child;
            space(j);if(j->at==j->n)return -1;c=j->s[j->at++];if(c==(unsigned)(obj?'}':']'))break;if(c!=',')return -1;
        }return ix;
    }
    if(c=='"'){v->type=JSTR;v->str=quoted(j);return v->str?ix:-1;}
    const char *literal=c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;
    if(literal){size_t n=strlen(literal);if(j->n-j->at<n||memcmp(j->s+j->at,literal,n))return -1;j->at+=n;v->type=c=='t'?JTRUE:c=='f'?JFALSE:JNULL;return ix;}
    size_t start=j->at;if(c=='-')j->at++;
    if(j->at==j->n)return -1;
    if(j->s[j->at]=='0')j->at++;
    else{if(j->s[j->at]<'1'||j->s[j->at]>'9')return -1;while(j->at<j->n&&isdigit(j->s[j->at]))j->at++;}
    if(j->at<j->n&&j->s[j->at]=='.'){j->at++;size_t b=j->at;while(j->at<j->n&&isdigit(j->s[j->at]))j->at++;if(b==j->at)return -1;}
    if(j->at<j->n&&(j->s[j->at]=='e'||j->s[j->at]=='E')){j->at++;if(j->at<j->n&&(j->s[j->at]=='+'||j->s[j->at]=='-'))j->at++;size_t b=j->at;while(j->at<j->n&&isdigit(j->s[j->at]))j->at++;if(b==j->at)return -1;}
    size_t n=j->at-start;if(n>64||j->used+n+1>sizeof j->arena)return -1;
    v->type=JNUM;v->str=j->arena+j->used;memcpy(j->arena+j->used,j->s+start,n);j->used+=n;j->arena[j->used++]=0;return ix;
}
static int parse(struct json *j,const void *s,size_t n){memset(j,0,sizeof *j);j->s=s;j->n=n;if(!n||(size_t)n>JSON_CAP||value(j,0)!=0||j->v[0].type!=JOBJ)return -1;space(j);return j->at==n?0:-1;}
static int get(const struct json *j,int obj,const char *key){
    if(obj<0||j->v[obj].type!=JOBJ)return -1;
    for(int p=j->v[obj].first;p>=0;p=j->v[j->v[p].next].next)if(!strcmp(j->v[p].str,key))return j->v[p].next;return -1;
}
static const char *strget(const struct json *j,const char *key){int k=get(j,0,key);return k>=0&&j->v[k].type==JSTR?j->v[k].str:NULL;}
static int truth(const struct json *j,int obj,const char *key,int yes){int k=get(j,obj,key);return k>=0&&j->v[k].type==(yes?JTRUE:JFALSE);}
static int fields(const struct json *j,const char *const *keys,unsigned count){unsigned n=0;for(int p=j->v[0].first;p>=0;p=j->v[j->v[p].next].next)n++;if(n!=count)return -1;for(unsigned i=0;i<count;i++)if(get(j,0,keys[i])<0)return -1;return 0;}
static int ident(const char *s,size_t min,size_t max,int dots){size_t n=s?strlen(s):0;if(n<min||n>max)return 0;for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='_'&&s[i]!='-'&&!(dots&&s[i]=='.'))return 0;return 1;}
static int decimal64(const char *s,uint64_t *value){if(!s||!*s||s[0]=='0'||strlen(s)>19)return 0;uint64_t v=0;for(;*s;s++){if(*s<'0'||*s>'9'||v>((uint64_t)INT64_MAX-(unsigned)(*s-'0'))/10)return 0;v=v*10+(unsigned)(*s-'0');}*value=v;return 1;}
static int epoch_string(const char *s,uint64_t want){uint64_t v;return decimal64(s,&v)&&v==want;}

struct config {char host[16],token[129],ca[PATH_MAX];unsigned port;unsigned char pin[32];};
static int trusted_path(const char *path,int secret){
    if(!path||path[0]!='/'||strlen(path)>=PATH_MAX)return -1;
    char p[PATH_MAX];strcpy(p,path);
    for(char *c=p+1;;c++)if(*c=='/'||!*c){char saved=*c;*c=0;dc_io_stat st;
        if(dc_io_fstatat(AT_FDCWD,p,&st,AT_SYMLINK_NOFOLLOW)||st.st_uid||S_ISLNK(st.st_mode))return -1;
        if(saved){if(!S_ISDIR(st.st_mode)||((st.st_mode&022)&&!((st.st_mode&S_ISVTX)&&!strcmp(p,"/tmp"))))return -1;}
        else{if(!S_ISREG(st.st_mode)||st.st_nlink!=1||(secret?(st.st_mode&0777)!=0600:(st.st_mode&022)!=0))return -1;return 0;}*c=saved;}
}
static int load_config(struct config *c,const char *path,struct json *j){
    if(trusted_path(path,1))return -1;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    dc_io_stat st;unsigned char b[JSON_CAP+1];ssize_t n;
    if(dc_io_fstat(fd,&st)||st.st_uid||!S_ISREG(st.st_mode)||st.st_nlink!=1||(st.st_mode&0777)!=0600){close(fd);return -1;}
    n=read(fd,b,sizeof b);close(fd);if(n<=0||(size_t)n>JSON_CAP||parse(j,b,(size_t)n))return -1;
    const char *const keys[]={"endpoint","token","ca_file","spki_sha256"};
    const char *ep=strget(j,"endpoint"),*tok=strget(j,"token"),*ca=strget(j,"ca_file"),*pin=strget(j,"spki_sha256");
    if(fields(j,keys,4)||!ep||!ident(tok,32,128,0)||!ca||strlen(ca)>=sizeof c->ca||trusted_path(ca,0)||!pin||strlen(pin)!=64)return -1;
    if(strncmp(ep,"wss://",6))return -1;const char *colon=strchr(ep+6,':');if(!colon||colon-ep-6<1||colon-ep-6>15)return -1;
    memcpy(c->host,ep+6,(size_t)(colon-ep-6));c->host[colon-ep-6]=0;
    struct in_addr ip;if(inet_pton(AF_INET,c->host,&ip)!=1)return -1;uint32_t addr=ntohl(ip.s_addr);
    if((addr>>24)!=10&&(addr>>24)!=127&&(addr>>20)!=0xac1&&(addr>>16)!=0xc0a8)return -1;
    const char *p=colon+1;unsigned port=0;if(*p<'1'||*p>'9')return -1;
    while(isdigit((unsigned char)*p)){port=port*10+(unsigned)(*p++-'0');if(port>65535)return -1;}if(!port||strcmp(p,PHONE_PATH))return -1;
    c->port=port;strcpy(c->token,tok);strcpy(c->ca,ca);
    for(unsigned i=0;i<32;i++){int a=hex(pin[i*2]),b2=hex(pin[i*2+1]);if(a<0||b2<0)return -1;c->pin[i]=(unsigned char)((a<<4)|b2);}
    memset(b,0,sizeof b);memset(j,0,sizeof *j);return 0;
}

struct frame {size_t n,at;unsigned type;uint64_t started;unsigned char b[FCAP];};
struct queue {unsigned count;struct frame f[QCAP];};
struct local_queue {unsigned count;dc_wire_packet p[QCAP];};
struct phone {
    int core,socket,state,opened,ready,completed,ack_state,diagnostic,closing,frozen,cancel_sent,command_pending,replay_only,remote_closed;
    uint64_t epoch,generation,reply,last_reply,mic_sequence,deadline,ping_ms,command_deadline,cancel_sequence;
    unsigned char command_hash[32];char ack_status[16],ack_code[65];
    char command[65],accept[64];struct config config;SSL_CTX *ctx;SSL *ssl;
    struct queue net;struct frame http;struct local_queue local;unsigned char rx[FCAP+8192];size_t rxn;
    short tls_want,read_want,write_want;struct json json;
};
enum {WAIT_OPEN,CONNECTING,TLS_CONNECT,HTTP_WRITE,HTTP_READ,WEBSOCKET};
static uint64_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static uint64_t wall_ms(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t)||t.tv_sec<0)return 0;return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static void shift_net(struct queue *q){memmove(q->f,q->f+1,(--q->count)*sizeof q->f[0]);}
static int local_push(struct phone *w,const dc_wire_header *h,const void *p){if(w->local.count==QCAP||(h->length&&!p))return -1;dc_wire_packet *o=&w->local.p[w->local.count++];o->header=*h;if(p&&h->length)memcpy(o->payload,p,h->length);return 0;}
static int local_flush(struct phone *w){while(w->local.count){dc_wire_packet *p=&w->local.p[0];if(dc_wire_send(w->core,&p->header,p->payload)){if(errno==EAGAIN||errno==EWOULDBLOCK)return 0;return -1;}memmove(w->local.p,w->local.p+1,(--w->local.count)*sizeof w->local.p[0]);}return 0;}
static int error_event(struct phone *w,const char *code){dc_wire_header h=dc_wire_make(DC_EVT_ERROR,w->epoch,w->generation,0,strlen(code));return local_push(w,&h,code);}
static int command_ack(struct phone *w,uint64_t epoch,const char *id,const char *status,const char *code,int replayed){
    char b[320];int n=snprintf(b,sizeof b,"{\"v\":2,\"kind\":\"command_ack\",\"epoch\":\"%llu\",\"command_id\":\"%s\",\"status\":\"%s\",\"code\":\"%s\",\"replayed\":%s}",(unsigned long long)epoch,id,status,code,replayed?"true":"false");
    if(n<0||(size_t)n>=sizeof b)return -1;dc_wire_header h=dc_wire_make(DC_EVT_TEXT,epoch,0,0,(size_t)n);return local_push(w,&h,b);
}
static int cache_ack(struct phone *w,const char *status,const char *code){strcpy(w->ack_status,status);strcpy(w->ack_code,code);return command_ack(w,w->epoch,w->command,status,code,0);}
static int enqueue_frame(struct phone *w,unsigned opcode,unsigned type,const void *p,size_t n){
    if(w->net.count==QCAP)return -1;struct frame *f=&w->net.f[w->net.count];unsigned char mask[4];
    if(RAND_bytes(mask,4)!=1||dc_xz_ws_build_client_frame((int)opcode,p,n,mask,f->b,sizeof f->b,&f->n))return -1;
    f->at=0;f->started=0;f->type=type;w->net.count++;return 0;
}
static int net_packet(struct phone *w,const dc_wire_header *h,const void *p){unsigned char b[WCAP];if(h->length&&!p)return -1;memcpy(b,h,sizeof *h);if(h->length)memcpy(b+sizeof *h,p,h->length);return enqueue_frame(w,2,h->type,b,sizeof *h+h->length);}
struct command {uint64_t deadline;char id[65];unsigned char digest[32];};
static int open_envelope(struct phone *w,const dc_wire_packet *p,struct command *command){
    const dc_wire_header *h=&p->header;struct json *j=&w->json;
    if(!h->epoch||h->epoch>INT64_MAX||h->generation||h->sequence||h->flags||h->length>4096||parse(j,p->payload,h->length))return -1;
    const char *const keys[]={"v","epoch","command_id","script_id","wake_source","wake_text","action","target","params","deadline_unix_ms"};
    int v=get(j,0,"v"),params=get(j,0,"params");const char *id=strget(j,"command_id"),*script=strget(j,"script_id"),*source=strget(j,"wake_source"),*wake=strget(j,"wake_text"),*action=strget(j,"action"),*target=strget(j,"target");
    if(fields(j,keys,10)||v<0||j->v[v].type!=JNUM||strcmp(j->v[v].str,"2")||!epoch_string(strget(j,"epoch"),h->epoch)||!decimal64(strget(j,"deadline_unix_ms"),&command->deadline)||!ident(id,8,64,0)||!ident(script,4,64,1)||(!isalnum((unsigned char)script[0])&&script[0]!='_')||strcmp(script+strlen(script)-3,".sh")||strstr(script,"..")||!source||!wake||!action||!target||params<0||j->v[params].type!=JOBJ)return -1;
    if(strcmp(source,"official_asr")&&strcmp(source,"local_wake")&&strcmp(source,"manual"))return -1;
    unsigned cps=0;for(const unsigned char *s=(const unsigned char *)wake;*s;){size_t n;uint32_t cp;if(utf8(s,strlen((const char *)s),&n,&cp)||cp<32||(cp>=127&&cp<=159)||++cps>64)return -1;s+=n;}
    /* Only known registry operations are transportable. Adding a provider must
     * explicitly extend the paired app registry; script strings are not code. */
    if(strcmp(target,"doubao")||(strcmp(action,"probe.attach")&&strcmp(action,"call.start"))||j->v[params].first>=0)return -1;
    strcpy(command->id,id);if(!SHA256(p->payload,h->length,command->digest))return -1;memset(j,0,sizeof *j);return 0;
}
static int connect_start(struct phone *w){
    w->socket=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(w->socket<0)return -1;
    int size=32768;(void)setsockopt(w->socket,SOL_SOCKET,SO_SNDBUF,&size,sizeof size);
    struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons((uint16_t)w->config.port);if(inet_pton(AF_INET,w->config.host,&a.sin_addr)!=1)return -1;
    int r=connect(w->socket,(struct sockaddr *)&a,sizeof a);if(r&&errno!=EINPROGRESS)return -1;
    w->state=CONNECTING;w->deadline=now_ms()+PHASE_MS;return 0;
}
static int peer_verified(struct phone *w){
    if(SSL_get_verify_result(w->ssl)!=X509_V_OK)return 0;X509 *cert=SSL_get_peer_certificate(w->ssl);if(!cert)return 0;
    unsigned char ip[4];(void)inet_pton(AF_INET,w->config.host,ip);int match=0,pinned=0;
    GENERAL_NAMES *sans=X509_get_ext_d2i(cert,NID_subject_alt_name,NULL,NULL);
    if(sans){for(int i=0;i<sk_GENERAL_NAME_num(sans);i++){const GENERAL_NAME *n=sk_GENERAL_NAME_value(sans,i);if(n->type==GEN_IPADD&&ASN1_STRING_length(n->d.iPAddress)==4&&!memcmp(asn1_data(n->d.iPAddress),ip,4))match=1;}GENERAL_NAMES_free(sans);}
    EVP_PKEY *key=X509_get_pubkey(cert);if(key){int n=i2d_PUBKEY(key,NULL);if(n>0&&n<16384){unsigned char *der=malloc((size_t)n);if(der){unsigned char *cursor=der,digest[SHA256_DIGEST_LENGTH];if(i2d_PUBKEY(key,&cursor)==n&&SHA256(der,(size_t)n,digest)){unsigned diff=0;for(unsigned i=0;i<32;i++)diff|=digest[i]^w->config.pin[i];pinned=!diff;}free(der);}}EVP_PKEY_free(key);}X509_free(cert);return match&&pinned;
}
static int tls_start(struct phone *w){
    tls_init();w->ctx=SSL_CTX_new(TLSv1_2_client_method());if(!w->ctx)return -1;
    SSL_CTX_set_verify(w->ctx,SSL_VERIFY_PEER,NULL);if(SSL_CTX_load_verify_locations(w->ctx,w->config.ca,NULL)!=1)return -1;
    w->ssl=SSL_new(w->ctx);if(!w->ssl||SSL_set_fd(w->ssl,w->socket)!=1)return -1;SSL_set_connect_state(w->ssl);w->state=TLS_CONNECT;w->tls_want=POLLOUT;return 0;
}
static int ssl_result(struct phone *w,int n,short *want){int e=SSL_get_error(w->ssl,n);if(e==SSL_ERROR_WANT_READ){*want=POLLIN;return 0;}if(e==SSL_ERROR_WANT_WRITE){*want=POLLOUT;return 0;}return -1;}
static int make_upgrade(struct phone *w){
    unsigned char random[16],hash[20];char key[25],source[128];
    if(RAND_bytes(random,sizeof random)!=1||EVP_EncodeBlock((unsigned char *)key,random,sizeof random)!=24)return -1;
    int n=snprintf(source,sizeof source,"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);if(n<0||n>=(int)sizeof source||!SHA1((unsigned char *)source,(size_t)n,hash))return -1;
    if(EVP_EncodeBlock((unsigned char *)w->accept,hash,sizeof hash)!=28)return -1;
    struct frame *f=&w->http;n=snprintf((char *)f->b,sizeof f->b,"GET " PHONE_PATH " HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer %s\r\n\r\n",w->config.host,w->config.port,key,w->config.token);
    if(n<0||n>=(int)sizeof f->b)return -1;f->n=(size_t)n;f->at=0;f->started=0;f->type=0;w->state=HTTP_WRITE;return 0;
}
static int upgrade_ok(struct phone *w,unsigned char *b,size_t n){
    if(n<4||memchr(b,0,n))return -1;b[n]=0;char *line=(char *)b,*end=strstr(line,"\r\n");if(!end)return -1;*end=0;
    if(strcmp(line,"HTTP/1.1 101 Switching Protocols"))return -1;
    unsigned upgrade=0,connection=0,accept=0;char *seen[32];unsigned count=0;line=end+2;
    while(*line){end=strstr(line,"\r\n");if(!end)return -1;*end=0;if(!*line)break;if(*line==' '||*line=='\t'||count==32)return -1;
        char *colon=strchr(line,':');if(!colon)return -1;*colon++=0;for(char *p=line;*p;p++)if(!isalnum((unsigned char)*p)&&*p!='-')return -1;
        for(unsigned i=0;i<count;i++)if(!strcasecmp(line,seen[i]))return -1;seen[count++]=line;
        while(*colon==' '||*colon=='\t')colon++;char *tail=colon+strlen(colon);while(tail>colon&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
        if(!strcasecmp(line,"Upgrade"))upgrade=!strcasecmp(colon,"websocket");
        else if(!strcasecmp(line,"Connection"))connection=!strcasecmp(colon,"Upgrade");
        else if(!strcasecmp(line,"Sec-WebSocket-Accept"))accept=!strcmp(colon,w->accept);
        else if(!strcasecmp(line,"Sec-WebSocket-Extensions")||!strcasecmp(line,"Sec-WebSocket-Protocol")||!strcasecmp(line,"Content-Length")||!strcasecmp(line,"Transfer-Encoding"))return -1;
        line=end+2;
    }return upgrade&&connection&&accept?0:-1;
}
static int ack(struct phone *w,const dc_wire_packet *p){
    struct json *j=&w->json;const char *const keys[]={"v","kind","epoch","command_id","status","code","replayed"};
    if(p->header.generation||p->header.sequence||p->header.flags||parse(j,p->payload,p->header.length)||fields(j,keys,7))return -1;
    int v=get(j,0,"v");const char *kind=strget(j,"kind"),*id=strget(j,"command_id"),*status=strget(j,"status"),*code=strget(j,"code");
    if(v<0||j->v[v].type!=JNUM||strcmp(j->v[v].str,"2")||!kind||strcmp(kind,"command_ack")||!id||strcmp(id,w->command)||!epoch_string(strget(j,"epoch"),w->epoch)||!status||!ident(code,1,64,0)||(!truth(j,0,"replayed",1)&&!truth(j,0,"replayed",0)))return -1;
    if(strcmp(status,"started")&&strcmp(status,"completed")&&strcmp(status,"failed")&&strcmp(status,"cancelled"))return -1;
    int state=!strcmp(status,"started")?1:!strcmp(status,"completed")?2:!strcmp(status,"failed")?3:4;
    /* A delayed replay of an earlier started result cannot reverse a genuine
     * completion. Drop it before the terminal contradiction check and never
     * forward it as a new state transition to the core. */
    if(w->ack_state==2&&!w->replay_only&&state==1&&truth(j,0,"replayed",1))return 0;
    /* Terminal command results cannot be reopened by delayed contradictory
     * ACKs for the same command_id. Exact duplicate ACKs are idempotent. */
    if(w->ack_state>=2&&(w->ack_state!=state||strcmp(w->ack_code,code)))return -1;
    /* A historical replay must never establish ownership, but it also cannot
     * revoke a genuine completion already observed on this connection. */
    if(truth(j,0,"replayed",1)&&!(w->ack_state==2&&!w->replay_only))w->replay_only=1;
    w->ack_state=state;w->completed=state==2&&!w->replay_only;
    if(state>=2)w->command_pending=0;
    strcpy(w->ack_status,status);strcpy(w->ack_code,code);
    return local_push(w,&p->header,p->payload);
}
static int maybe_ready(struct phone *w){
    if(w->ready||!w->completed||w->replay_only||!w->diagnostic||w->frozen)return 0;
    w->ready=1;dc_wire_header h=dc_wire_make(DC_EVT_READY,w->epoch,0,0,0);return local_push(w,&h,NULL);
}
static int status_event(struct phone *w,const dc_wire_packet *p){
    struct json *j=&w->json;if(p->header.generation||p->header.sequence||p->header.flags||parse(j,p->payload,p->header.length))return -1;
    int cap=get(j,0,"capabilities");if(cap<0||j->v[cap].type!=JOBJ||(!truth(j,0,"probe_only",1)&&!truth(j,0,"probe_only",0))||(!truth(j,0,"call_ready",1)&&!truth(j,0,"call_ready",0)))return -1;
    const char *names[]={"virtual_mic","rtc_capture","call_lifecycle","reply_lifecycle","turn_fence","automatic_ui"};
    for(unsigned i=0;i<6;i++)if(!truth(j,cap,names[i],1)&&!truth(j,cap,names[i],0))return -1;
    w->diagnostic=truth(j,0,"probe_only",0)&&truth(j,0,"call_ready",1)&&truth(j,cap,"virtual_mic",1)&&truth(j,cap,"rtc_capture",1)&&truth(j,cap,"call_lifecycle",1)&&truth(j,cap,"reply_lifecycle",1);
    if(w->ready&&!w->diagnostic)return -1;
    /* Only fixed booleans cross the diagnostic plane, never arbitrary app
     * response strings. In particular this cannot stop a connection cue. */
    const char *msg=w->diagnostic?"{\"v\":1,\"kind\":\"phone_status\",\"production_capable\":true}":"{\"v\":1,\"kind\":\"phone_status\",\"production_capable\":false}";
    dc_wire_header h=dc_wire_make(DC_EVT_TEXT,w->epoch,0,0,strlen(msg));if(local_push(w,&h,msg))return -1;return maybe_ready(w);
}
static int wire_valid(const dc_wire_packet *p,size_t n){
    const dc_wire_header *h=&p->header;
    if(n<sizeof *h||h->magic!=DC_WIRE_MAGIC||h->version!=1||h->length>DC_WIRE_CAP||h->reserved||n!=sizeof *h+h->length||(h->flags&~7u)||h->type<DC_EVT_READY||h->type>DC_EVT_RESUMED)return -1;
    if(h->type==DC_EVT_AUDIO){if(!(h->flags&DC_WIRE_PCM_S16LE)||!h->length||(h->channels!=1&&h->channels!=2)||h->length%(2u*h->channels)||h->rate<8000||h->rate>48000)return -1;}
    else if(h->rate||h->channels)return -1;return 0;
}
static int expire_command(struct phone *w);
static int remote_packet(struct phone *w,const unsigned char *b,size_t n){
    dc_wire_packet p;memset(&p,0,sizeof p);if(n>sizeof p||n<sizeof p.header)return -1;memcpy(&p,b,n);if(wire_valid(&p,n))return -1;
    dc_wire_header *h=&p.header;if(h->epoch!=w->epoch||w->remote_closed)return 0;
    if(expire_command(w))return -1;if(w->remote_closed)return 0;
    if(h->type==DC_EVT_TEXT){if(ack(w,&p))return -1;return maybe_ready(w);}
    if(h->type==DC_EVT_READY)return status_event(w,&p);
    if(h->type==DC_EVT_ERROR){/* Fixed local reason, no unchecked server text. */return error_event(w,"phone_remote_error");}
    if(h->type==DC_EVT_CANCEL_ACK){if(!w->frozen||!w->cancel_sent||h->generation!=w->generation||h->sequence!=w->cancel_sequence||h->length||h->flags)return -1;return local_push(w,h,NULL);}
    if(h->type==DC_EVT_RESUMED)return -1; /* No verifiable turn-fence event exists in v1. */
    if(!w->ready||w->frozen)return 0;
    if(h->type==DC_EVT_REPLY_BEGIN){
        if(h->length||h->flags||h->generation||!h->sequence||h->sequence<=w->last_reply||w->reply)return -1;
        w->reply=w->last_reply=h->sequence;w->generation=0;return local_push(w,h,NULL);
    }
    if(h->type==DC_EVT_AUDIO||h->type==DC_EVT_REPLY_END){
        if(!w->reply||h->sequence!=w->reply||!w->generation||h->generation!=w->generation)return -1;
        if(h->type==DC_EVT_AUDIO){/* No untested resampler or channel conversion. */if(h->channels!=1||(h->rate!=8000&&h->rate!=16000&&h->rate!=24000&&h->rate!=48000))return -1;}
        else{if(h->length||h->flags)return -1;w->reply=0;}
        return local_push(w,h,p.payload);
    }return -1;
}
static void freeze(struct phone *w){
    w->frozen=1;
    for(unsigned i=0;i<w->local.count;){unsigned t=w->local.p[i].header.type;if(t==DC_EVT_AUDIO||t==DC_EVT_REPLY_BEGIN||t==DC_EVT_REPLY_END){memmove(w->local.p+i,w->local.p+i+1,(--w->local.count-i)*sizeof w->local.p[0]);}else i++;}
    /* A partially SSL-written frame must finish byte-identically before CANCEL;
     * removing half a WebSocket frame would corrupt the authenticated stream. */
    for(unsigned i=0;i<w->net.count;){if(w->net.f[i].type==DC_CMD_MIC&&!w->net.f[i].at&&!w->net.f[i].started){memmove(w->net.f+i,w->net.f+i+1,(--w->net.count-i)*sizeof w->net.f[0]);}else i++;}
}
static int expire_command(struct phone *w){
    if(!w->command_pending)return 0;uint64_t now=now_ms();if(now&&now<w->command_deadline)return 0;
    freeze(w);w->command_pending=0;w->completed=0;w->ack_state=3;w->remote_closed=1;
    /* Closing the connection aborts pending remote dispatch. Never wait for a
     * remote ACK or preserve queued OPEN after the action deadline. */
    if(w->socket>=0){shutdown(w->socket,SHUT_RDWR);close(w->socket);w->socket=-1;}
    if(w->ssl){SSL_free(w->ssl);w->ssl=NULL;}
    w->state=WAIT_OPEN;memset(&w->net,0,sizeof w->net);memset(&w->http,0,sizeof w->http);
    return cache_ack(w,"failed",now?"deadline_exceeded":"clock_untrusted");
}
static int commands(struct phone *w){
    for(unsigned batch=0;batch<16;batch++){
        dc_wire_packet p;int got=dc_wire_receive(w->core,&p);if(!got)return 1;if(got<0)return errno==EAGAIN||errno==EWOULDBLOCK?0:-1;
        dc_wire_header *h=&p.header;
        if(h->type==DC_CMD_OPEN){
            struct command cmd;if(open_envelope(w,&p,&cmd))return -1;
            if(w->opened){
                if(h->epoch!=w->epoch||memcmp(cmd.digest,w->command_hash,sizeof cmd.digest)){
                    if(command_ack(w,h->epoch,cmd.id,"failed","command_conflict",0))return -1;
                }else if(command_ack(w,w->epoch,w->command,*w->ack_status?w->ack_status:"started",*w->ack_code?w->ack_code:"accepted",1))return -1;
                continue;
            }
            w->opened=1;w->epoch=h->epoch;strcpy(w->command,cmd.id);memcpy(w->command_hash,cmd.digest,sizeof cmd.digest);
            uint64_t wall=wall_ms(),mono=now_ms();const char *refusal=!mono||wall<MIN_WALL_MS||wall>=MAX_WALL_MS?"clock_untrusted":cmd.deadline<=wall?"deadline_exceeded":cmd.deadline-wall>COMMAND_MS?"deadline_out_of_range":NULL;
            if(refusal){w->frozen=1;w->remote_closed=1;w->ack_state=3;if(cache_ack(w,"failed",refusal))return -1;continue;}
            w->command_pending=1;w->command_deadline=mono+cmd.deadline-wall;
            /* Keep OPEN private in the bounded frame queue until upgrade. */
            if(net_packet(w,h,p.payload)||connect_start(w))return -1;continue;
        }
        if(!w->opened||h->epoch!=w->epoch||h->type<DC_CMD_MIC||h->type>DC_CMD_GRANT)return -1;
        if(h->type==DC_CMD_CLOSE){if(h->length||h->flags)return -1;freeze(w);w->closing=1;w->deadline=now_ms()+WRITE_MS;if(w->state==WEBSOCKET&&net_packet(w,h,NULL))return -1;return 0;}
        if(w->remote_closed)continue;
        if(h->type==DC_CMD_CANCEL){
            if(h->length||h->flags)return -1;
            /* An old queued cancellation must not freeze a newer granted
             * reply. Ignore its stale identity without changing current state. */
            if(h->generation!=w->generation||(h->sequence&&h->sequence!=w->reply))continue;
            freeze(w);if(!w->cancel_sent){if(net_packet(w,h,NULL))return -1;w->cancel_sequence=h->sequence;w->cancel_sent=1;}continue;
        }
        if(h->type==DC_CMD_RESUME){if(error_event(w,"turn_fence_unavailable"))return -1;continue;}
        if(h->type==DC_CMD_MIC){
            if(h->rate!=16000||h->channels!=1||h->flags!=DC_WIRE_PCM_S16LE||h->sequence<=w->mic_sequence)return -1;
            w->mic_sequence=h->sequence;if(!w->ready||w->frozen)continue;
            if(h->generation!=w->generation||net_packet(w,h,p.payload))return -1;continue;
        }
        if(h->type==DC_CMD_GRANT){
            if(h->length||h->flags)return -1;
            if(h->sequence||h->generation){if(!w->ready||w->frozen||!w->reply||h->sequence!=w->reply||!h->generation||(w->generation&&w->generation!=h->generation))return -1;w->generation=h->generation;}
            if(net_packet(w,h,NULL))return -1;
        }
    }return 0;
}
static int writes(struct phone *w){
    if(w->state!=HTTP_WRITE&&!w->net.count)return 0;struct frame *f=w->state==HTTP_WRITE?&w->http:&w->net.f[0];uint64_t now=now_ms();if(!f->started)f->started=now;
    if(now-f->started>WRITE_MS)return -1;
    int n=SSL_write(w->ssl,f->b+f->at,(int)(f->n-f->at));if(n<=0)return ssl_result(w,n,&w->write_want);
    w->write_want=0;f->at+=(size_t)n;if(f->at==f->n&&w->state!=HTTP_WRITE)shift_net(&w->net);return 0;
}
static int frames(struct phone *w){
    for(unsigned batch=0;batch<8&&w->rxn>=2;batch++){
        if(!(w->rx[0]&0x80)||(w->rx[0]&0x70)||(w->rx[1]&0x80)||(w->rx[1]&127)==127)return -1;
        size_t head=2,n=w->rx[1]&127;if(n==126){if(w->rxn<4)return 0;head=4;n=((size_t)w->rx[2]<<8)|w->rx[3];if(n<126)return -1;}
        if(n>WCAP||head+n>FCAP)return -1;if(w->rxn<head+n)return 0;
        unsigned char b[WCAP];dc_xz_ws_frame f;size_t used;
        if(dc_xz_ws_decode_frame(w->rx,head+n,0,&f,b,sizeof b,&used))return -1;
        if(f.opcode==2){if(remote_packet(w,b,used))return -1;}
        else if(f.opcode==9){if(enqueue_frame(w,10,0,b,used))return -1;}
        else if(f.opcode==8)return -1;else if(f.opcode!=10)return -1;
        memmove(w->rx,w->rx+head+n,w->rxn-head-n);w->rxn-=head+n;
    }return 0;
}
static int reads(struct phone *w){
    if(w->rxn==sizeof w->rx)return -1;
    int n=SSL_read(w->ssl,w->rx+w->rxn,(int)(sizeof w->rx-w->rxn));if(n<=0)return ssl_result(w,n,&w->read_want);w->read_want=0;w->rxn+=(size_t)n;
    if(w->state==HTTP_READ){
        size_t end=0;for(size_t i=3;i<w->rxn;i++)if(!memcmp(w->rx+i-3,"\r\n\r\n",4)){end=i+1;break;}
        if(!end)return w->rxn>=8192?-1:0;
        /* upgrade parser edits its buffer: preserve any coalesced first frame. */
        unsigned char header[8193];if(end>8192)return -1;memcpy(header,w->rx,end);if(upgrade_ok(w,header,end))return -1;
        memmove(w->rx,w->rx+end,w->rxn-end);w->rxn-=end;w->state=WEBSOCKET;w->ping_ms=now_ms();
    }return frames(w);
}
int dc_phone_run(int fd,const char *path){
    struct phone *w=calloc(1,sizeof *w);if(!w)return 1;w->socket=-1;w->core=fd;w->state=WAIT_OPEN;int result=1;
    int type;socklen_t len=sizeof type;struct ucred cred;socklen_t clen=sizeof cred;
    if(geteuid()||getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&len)||type!=SOCK_SEQPACKET||getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&clen)||cred.uid||load_config(&w->config,path,&w->json)){result=2;goto done;}
    if(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK)<0)goto done;w->deadline=now_ms()+PHASE_MS;
    /* HTTP credentials and bounded binary packets use independent buffers. */
    for(;;){
        if(expire_command(w))break;
        int c=commands(w);if(c){result=c>0?0:2;break;}
        if(local_flush(w))break;
        uint64_t now=now_ms();if(w->closing&&(!w->net.count||now>=w->deadline)){result=0;break;}
        if(w->state!=WEBSOCKET&&!w->remote_closed&&now>=w->deadline)break;
        if(w->state==TLS_CONNECT){
            int r=SSL_connect(w->ssl);if(r==1){if(!peer_verified(w)||make_upgrade(w))break;}
            else if(ssl_result(w,r,&w->tls_want))break;
        }
        if(w->state==HTTP_WRITE||w->state==WEBSOCKET){if(writes(w))break;if(w->state==HTTP_WRITE&&w->http.at==w->http.n){memset(&w->http,0,sizeof w->http);w->state=HTTP_READ;}}
        if(w->closing&&!w->net.count){result=0;break;}
        if(w->state==WEBSOCKET){
            if(frames(w))break;
            if(!w->remote_closed&&now-w->ping_ms>=2000){dc_wire_header h=dc_wire_make(DC_CMD_GRANT,w->epoch,0,0,0);if(net_packet(w,&h,NULL))break;w->ping_ms=now;}
        }
        struct pollfd fds[2]={{fd,(short)(POLLIN|(w->local.count?POLLOUT:0)),0},{w->socket,0,0}};
        if(w->state==CONNECTING)fds[1].events=POLLOUT;
        else if(w->state==TLS_CONNECT)fds[1].events=w->tls_want;
        else if(w->state==HTTP_WRITE)fds[1].events=w->write_want?w->write_want:POLLOUT;
        else if(w->state==HTTP_READ||w->state==WEBSOCKET)fds[1].events=(short)((w->read_want?w->read_want:POLLIN)|((w->state==WEBSOCKET&&w->net.count)?(w->write_want?w->write_want:POLLOUT):0));
        int r=poll(fds,w->socket<0?1:2,w->ssl&&SSL_pending(w->ssl)?0:50);if(r<0){if(errno==EINTR)continue;break;}
        /* Core input is drained at the top before handling received audio. */
        if(fds[0].revents&POLLIN)continue;
        if(fds[0].revents&(POLLERR|POLLNVAL|POLLHUP)){result=0;break;}
        if(fds[1].revents&(POLLERR|POLLNVAL|POLLHUP))break;
        if(w->state==CONNECTING&&(fds[1].revents&POLLOUT)){int e=0;socklen_t size=sizeof e;if(getsockopt(w->socket,SOL_SOCKET,SO_ERROR,&e,&size)||e||tls_start(w))break;}
        else if((w->state==HTTP_READ||w->state==WEBSOCKET)&&((fds[1].revents&(w->read_want?w->read_want:POLLIN))||SSL_pending(w->ssl))){if(reads(w))break;}
    }
    if(result&&w->opened){freeze(w);if(w->command_pending)(void)cache_ack(w,"failed",result==2?"phone_invalid_command":"phone_transport_failed");(void)error_event(w,result==2?"phone_invalid_command":"phone_transport_failed");(void)local_flush(w);}
done:
    if(w->ssl)SSL_free(w->ssl);if(w->ctx)SSL_CTX_free(w->ctx);if(w->socket>=0)close(w->socket);
    memset(w,0,sizeof *w);free(w);return result;
}

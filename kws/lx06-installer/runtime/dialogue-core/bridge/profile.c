#define _GNU_SOURCE
#include "profile.h"
#include "../adapters/support/xiaozhi/ws_frame.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Same public OpenSSL 1.0 ABI compatibility as the legacy phone transport.
 * That transport is neither linked nor called by this candidate. */
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
extern unsigned char *profile_asn1_data(ASN1_STRING *) __asm__("ASN1_STRING_data");
static const unsigned char *asn1_data(const ASN1_STRING *s){return profile_asn1_data((ASN1_STRING *)s);}
static void tls_init(void){extern int SSL_library_init(void);extern void SSL_load_error_strings(void);(void)SSL_library_init();SSL_load_error_strings();}
#else
static const unsigned char *asn1_data(const ASN1_STRING *s){return ASN1_STRING_get0_data(s);}
static void tls_init(void){}
#endif

#define WIRE_CAP 8240u
#define FRAME_CAP (WIRE_CAP+8u)
#define PROFILE_PATH "/lx06/phone-audio/v1"
struct br_profile {
    int socket,connected,busy;
    SSL_CTX *ctx;SSL *ssl;
    br_discovery *discovery;br_session *session;br_operation *operation;
    br_profile_session_event event;void *event_context;
    br_profile_source_current source_current;void *source_context;
    uint64_t run,epoch,last_run,last_epoch,probe_ms;
    const char *error;
    size_t rxn;
    unsigned char rx[FRAME_CAP*2],tx[FRAME_CAP],wire[WIRE_CAP];
    dc_wire_packet packet;
};
_Static_assert(sizeof(br_profile)<64*1024,"bounded profile buffers, excludes TLS");
static uint64_t now_ms(void){
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)||t.tv_sec<0)return 0;
    return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;
}
static int identity(const br_profile *p,uint64_t run,uint64_t epoch){
    return p&&run&&epoch&&run==p->run&&epoch==p->epoch;
}
static void shutdown_profile(br_profile *p,const char *error){
    p->connected=0;
    if(p->session){
        (void)br_session_close(p->session,p->run,p->epoch);
        if(p->event)p->event(p->event_context,p->session,now_ms(),BR_OP_CLOSED);
    }
    if(p->discovery&&p->discovery->epoch==p->epoch)br_discovery_close(p->discovery);
    if(p->ssl)SSL_free(p->ssl);
    if(p->ctx)SSL_CTX_free(p->ctx);
    if(p->socket>=0)close(p->socket);
    p->ssl=NULL;p->ctx=NULL;p->socket=-1;p->discovery=NULL;p->session=NULL;p->operation=NULL;p->event=NULL;p->event_context=NULL;
    p->source_current=NULL;p->source_context=NULL;
    p->rxn=0;p->error=error;
    memset(p->rx,0,sizeof p->rx);memset(p->tx,0,sizeof p->tx);
    memset(p->wire,0,sizeof p->wire);memset(&p->packet,0,sizeof p->packet);
}
br_profile *br_profile_new(void){
    br_profile *p=calloc(1,sizeof(*p));if(p)p->socket=-1;return p;
}
void br_profile_free(br_profile *p){if(p){shutdown_profile(p,NULL);memset(p,0,sizeof(*p));free(p);}}
void br_profile_close(br_profile *p,uint64_t run,uint64_t epoch){
    if(identity(p,run,epoch)&&!p->busy)shutdown_profile(p,"bridge_unreachable");
}
static int cleared(const void *memory,size_t length){
    const unsigned char *bytes=memory;
    for(size_t i=0;i<length;i++)if(bytes[i])return 0;
    return 1;
}
int br_profile_transport_closed(const br_profile *p,uint64_t run,uint64_t epoch){
    if(!identity(p,run,epoch))return BR_PROFILE_INVALID;
    return !p->busy&&!p->connected&&p->socket<0&&!p->ssl&&!p->ctx&&
        !p->discovery&&!p->session&&!p->operation&&!p->event&&!p->event_context&&
        !p->source_current&&!p->source_context&&!p->rxn&&
        cleared(p->rx,sizeof p->rx)&&cleared(p->tx,sizeof p->tx)&&
        cleared(p->wire,sizeof p->wire)&&cleared(&p->packet,sizeof p->packet);
}
const char *br_profile_error(const br_profile *p){return p?p->error:"unsupported";}
static int source_current(br_profile *p){return !p->source_current||p->source_current(p->source_context)==1;}
static int wait_socket(br_profile *p,short events,uint64_t deadline){
    for(;;){
        uint64_t now=now_ms();if(!now||now>=deadline||!source_current(p))return -1;
        struct pollfd f={p->socket,events,0};
        int timeout=(int)(deadline-now>1000?1000:deadline-now);
        int rc=poll(&f,1,timeout);
        if(rc<0){if(errno==EINTR)continue;return -1;}
        if(rc&&f.revents&events)return 1;
        if(rc&&f.revents&(POLLERR|POLLHUP|POLLNVAL))return -1;
    }
}
static int ssl_wait(br_profile *p,int rc,uint64_t deadline){
    int e=SSL_get_error(p->ssl,rc);
    if(e==SSL_ERROR_WANT_READ)return wait_socket(p,POLLIN,deadline);
    if(e==SSL_ERROR_WANT_WRITE)return wait_socket(p,POLLOUT,deadline);
    return -1;
}
static int write_all(br_profile *p,const void *data,size_t n,uint64_t deadline){
    const unsigned char *b=data;size_t at=0;
    while(at<n){
        if(now_ms()>=deadline||!source_current(p))return -1;
        int rc=SSL_write(p->ssl,b+at,(int)(n-at));
        if(rc>0)at+=(size_t)rc;
        else if(ssl_wait(p,rc,deadline)<0)return -1;
    }
    return 1;
}
static int config_valid(const br_profile_config *c){
    if(!c||!c->host||!c->token||!c->ca_file||!c->port||c->port>65535)return 0;
    if(strnlen(c->host,16)>=16||strnlen(c->token,129)>128||strlen(c->token)<32||
       c->ca_file[0]!='/'||strnlen(c->ca_file,PATH_MAX)>=PATH_MAX)return 0;
    for(const unsigned char *s=(const unsigned char *)c->token;*s;s++)
        if(!((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return 0;
    struct in_addr ip;if(inet_pton(AF_INET,c->host,&ip)!=1)return 0;
    uint32_t a=ntohl(ip.s_addr);
    return (a>>24)==10||(a>>24)==127||(a>>20)==0xac1||(a>>16)==0xc0a8;
}
static int verified(br_profile *p,const br_profile_config *c){
    if(SSL_get_verify_result(p->ssl)!=X509_V_OK)return 0;
    X509 *cert=SSL_get_peer_certificate(p->ssl);if(!cert)return 0;
    unsigned char ip[4];(void)inet_pton(AF_INET,c->host,ip);int match=0,pinned=0;
    GENERAL_NAMES *sans=X509_get_ext_d2i(cert,NID_subject_alt_name,NULL,NULL);
    if(sans){
        for(int i=0;i<sk_GENERAL_NAME_num(sans);i++){
            const GENERAL_NAME *n=sk_GENERAL_NAME_value(sans,i);
            if(n->type==GEN_IPADD&&ASN1_STRING_length(n->d.iPAddress)==4&&
               !memcmp(asn1_data(n->d.iPAddress),ip,4))match=1;
        }
        GENERAL_NAMES_free(sans);
    }
    EVP_PKEY *key=X509_get_pubkey(cert);
    if(key){
        unsigned char der[16384],digest[SHA256_DIGEST_LENGTH],*cursor=der;
        int n=i2d_PUBKEY(key,NULL);
        if(n>0&&(size_t)n<=sizeof der&&i2d_PUBKEY(key,&cursor)==n&&SHA256(der,(size_t)n,digest)){
            unsigned diff=0;for(unsigned i=0;i<32;i++)diff|=digest[i]^c->spki_sha256[i];pinned=!diff;
        }
        EVP_PKEY_free(key);
    }
    X509_free(cert);return match&&pinned;
}
static int upgrade_ok(unsigned char *b,size_t n,const char *expected){
    if(n<4||memchr(b,0,n))return 0;
    b[n]=0;char *line=(char *)b,*end=strstr(line,"\r\n");if(!end)return 0;*end=0;
    if(strcmp(line,"HTTP/1.1 101 Switching Protocols"))return 0;
    unsigned upgrade=0,connection=0,accept=0,count=0;char *seen[32];line=end+2;
    while(*line){
        end=strstr(line,"\r\n");if(!end)return 0;*end=0;if(!*line)break;
        if(*line==' '||*line=='\t'||count==32)return 0;
        char *colon=strchr(line,':');if(!colon||colon==line)return 0;*colon++=0;
        for(char *s=line;*s;s++)if(!isalnum((unsigned char)*s)&&*s!='-')return 0;
        for(unsigned i=0;i<count;i++)if(!strcasecmp(line,seen[i]))return 0;
        seen[count++]=line;
        while(*colon==' '||*colon=='\t')colon++;
        char *tail=colon+strlen(colon);while(tail>colon&&(tail[-1]==' '||tail[-1]=='\t'))*--tail=0;
        for(char *s=colon;*s;s++)if((unsigned char)*s<32||*s==127)return 0;
        if(!strcasecmp(line,"Upgrade"))upgrade=!strcasecmp(colon,"websocket");
        else if(!strcasecmp(line,"Connection"))connection=!strcasecmp(colon,"Upgrade");
        else if(!strcasecmp(line,"Sec-WebSocket-Accept"))accept=!strcmp(colon,expected);
        else if(!strcasecmp(line,"Sec-WebSocket-Extensions")||!strcasecmp(line,"Sec-WebSocket-Protocol")||
                !strcasecmp(line,"Content-Length")||!strcasecmp(line,"Transfer-Encoding"))return 0;
        line=end+2;
    }
    return upgrade&&connection&&accept;
}
static int ws_send(br_profile *p,unsigned opcode,const void *data,size_t n,uint64_t deadline){
    unsigned char mask[4];size_t length=0;
    if(RAND_bytes(mask,4)!=1||dc_xz_ws_build_client_frame((int)opcode,data,n,mask,p->tx,sizeof p->tx,&length))return -1;
    int rc=write_all(p,p->tx,length,deadline);memset(p->tx,0,sizeof p->tx);return rc;
}
static int allow_encoded(void *unused,const dc_wire_header *h,const unsigned char *b,size_t n){
    (void)unused;(void)h;(void)b;(void)n;return 1; /* original owner's semantic encoder is required */
}
static int dial(br_profile *p,const br_profile_config *c,br_discovery *d,
    uint64_t run,uint64_t epoch,const void *open,size_t n,br_profile_source_current current,void *context){
    if(!p||p->busy||!d||!config_valid(c)||!run||run<=p->last_run||!epoch||epoch<=p->last_epoch||
       !open||!n||n>8192||(current&&current(context)!=1))return BR_PROFILE_INVALID;
    /* Validate before dropping an existing connection, without mutating it. */
    br_json *scratch=calloc(1,sizeof(*scratch));if(!scratch)return BR_PROFILE_INVALID;
    int ok=br_caps_discovery_request(epoch,open,n,scratch)==BR_CAPS_OK;
    free(scratch);if(!ok)return BR_PROFILE_INVALID;
    p->busy=1;shutdown_profile(p,NULL);p->last_run=p->run=run;p->last_epoch=p->epoch=epoch;
    p->source_current=current;p->source_context=context;
    p->discovery=d;uint64_t start=now_ms(),deadline=start+10000;const char *error="bridge_unreachable";
    if(!start||!source_current(p)||br_discovery_begin(d,epoch,start)!=1)goto failed;
    p->socket=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(p->socket<0)goto failed;
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)c->port)};
    (void)inet_pton(AF_INET,c->host,&a.sin_addr);
    int rc=connect(p->socket,(struct sockaddr *)&a,sizeof a);
    if(rc){
        if(errno!=EINPROGRESS||wait_socket(p,POLLOUT,deadline)<0)goto failed;
        int e=0;socklen_t size=sizeof e;
        if(getsockopt(p->socket,SOL_SOCKET,SO_ERROR,&e,&size)||e)goto failed;
    }
    tls_init();p->ctx=SSL_CTX_new(TLSv1_2_client_method());if(!p->ctx)goto failed;
    SSL_CTX_set_verify(p->ctx,SSL_VERIFY_PEER,NULL);
    if(SSL_CTX_load_verify_locations(p->ctx,c->ca_file,NULL)!=1)goto failed;
    p->ssl=SSL_new(p->ctx);if(!p->ssl||SSL_set_fd(p->ssl,p->socket)!=1)goto failed;
    for(;;){
        if(now_ms()>=deadline||!source_current(p))goto failed;
        rc=SSL_connect(p->ssl);if(rc==1)break;
        if(ssl_wait(p,rc,deadline)<0)goto failed;
    }
    if(!source_current(p)||!verified(p,c))goto failed;
    unsigned char random[16],hash[20];char key[25],source[128],accept[29];
    if(RAND_bytes(random,sizeof random)!=1||EVP_EncodeBlock((unsigned char *)key,random,sizeof random)!=24)goto failed;
    int length=snprintf(source,sizeof source,"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);
    if(length<0||(size_t)length>=sizeof source||!SHA1((unsigned char *)source,(size_t)length,hash)||
       EVP_EncodeBlock((unsigned char *)accept,hash,sizeof hash)!=28)goto failed;
    length=snprintf((char *)p->tx,sizeof p->tx,"GET " PROFILE_PATH " HTTP/1.1\r\nHost: %s:%u\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n\r\n",c->host,c->port,key,c->token);
    if(length<0||(size_t)length>=sizeof p->tx||write_all(p,p->tx,(size_t)length,deadline)<0)goto failed;
    memset(p->tx,0,sizeof p->tx);
    for(;;){
        size_t end=0;for(size_t i=3;i<p->rxn;i++)if(!memcmp(p->rx+i-3,"\r\n\r\n",4)){end=i+1;break;}
        if(end){
            unsigned char header[8193];if(end>8192)goto failed;
            memcpy(header,p->rx,end);
            if(end>=12&&!memcmp(header,"HTTP/1.1 401",12))error="token_invalid";
            if(!upgrade_ok(header,end,accept))goto failed;
            memmove(p->rx,p->rx+end,p->rxn-end);p->rxn-=end;break;
        }
        if(p->rxn>=8192||now_ms()>=deadline)goto failed;
        rc=SSL_read(p->ssl,p->rx+p->rxn,(int)(sizeof p->rx-p->rxn));
        if(rc>0)p->rxn+=(size_t)rc;else if(ssl_wait(p,rc,deadline)<0)goto failed;
    }
    if(now_ms()>=deadline||!source_current(p)||br_discovery_authenticated(d,epoch,now_ms())!=1)goto failed;
    memset(&p->packet,0,sizeof p->packet);
    p->packet.header=(dc_wire_header){.magic=DC_WIRE_MAGIC,.version=1,.type=BR_WIRE_OPEN,
        .epoch=epoch,.length=(uint32_t)n};memcpy(p->packet.payload,open,n);
    size_t written=0;
    if(br_wire_encode(&p->packet,BR_FROM_SPEAKER,epoch,allow_encoded,NULL,p->wire,sizeof p->wire,&written)!=1||
       ws_send(p,2,p->wire,written,now_ms()+2000)<0)goto failed;
    memset(&p->packet,0,sizeof p->packet);memset(p->wire,0,sizeof p->wire);
    p->connected=1;p->probe_ms=now_ms();p->busy=0;return BR_PROFILE_CONNECTED;
failed:
    shutdown_profile(p,error);p->busy=0;return BR_PROFILE_CLOSED;
}
int br_profile_dial(br_profile *p,const br_profile_config *c,br_discovery *d,
    uint64_t run,uint64_t epoch,const void *open,size_t n){
    return dial(p,c,d,run,epoch,open,n,NULL,NULL);
}
int br_profile_dial_source(br_profile *p,const br_profile_config *c,br_discovery *d,
    uint64_t run,uint64_t epoch,const void *open,size_t n,br_profile_source_current current,void *context){
    if(!current||!context)return BR_PROFILE_INVALID;
    return dial(p,c,d,run,epoch,open,n,current,context);
}
int br_profile_session(br_profile *p,uint64_t run,uint64_t epoch,br_session *s){
    if(!identity(p,run,epoch)||p->busy||!p->connected||p->session||!s||!s->live||
       s->run!=run||s->epoch!=epoch||s->operation.discovery!=p->discovery||
       p->discovery->phase!=BR_DISC_READY)return BR_PROFILE_INVALID;
    if(!source_current(p)){shutdown_profile(p,"unsupported");return BR_PROFILE_CLOSED;}
    p->session=s;return 1;
}
int br_profile_operation(br_profile *p,uint64_t run,uint64_t epoch,br_operation *op){
    if(!identity(p,run,epoch)||p->busy||!p->connected||p->session||p->operation||!op||
       !p->discovery||p->discovery->phase!=BR_DISC_READY||op->discovery!=p->discovery||
       p->discovery->business_used||!source_current(p))return BR_PROFILE_INVALID;
    p->operation=op;return 1;
}
int br_profile_submit(void *context,uint64_t run,uint64_t epoch,const void *data,size_t n){
    br_profile *p=context;
    if(!identity(p,run,epoch)||p->busy||!p->connected||(!p->session&&!p->operation)||
       p->discovery->epoch!=epoch||p->discovery->phase!=BR_DISC_READY||(p->session&&!p->session->live))return BR_PROFILE_INVALID;
    if(!source_current(p)){shutdown_profile(p,"unsupported");return BR_PROFILE_CLOSED;}
    if(br_wire_decode(data,n,BR_FROM_SPEAKER,epoch,allow_encoded,NULL,&p->packet)!=1)return BR_PROFILE_INVALID;
    memset(&p->packet,0,sizeof p->packet);p->busy=1;
    int rc=ws_send(p,2,data,n,now_ms()+2000);
    if(rc<0)shutdown_profile(p,"bridge_unreachable");
    p->busy=0;return rc<0?BR_PROFILE_CLOSED:1;
}
int br_profile_events(br_profile *p,uint64_t run,uint64_t epoch,br_profile_session_event event,void *context){
    if(!identity(p,run,epoch)||p->busy||!p->connected||!p->session||p->event||!event||
       !p->session->live||p->session->run!=run||p->session->epoch!=epoch)return BR_PROFILE_INVALID;
    p->event=event;p->event_context=context;return 1;
}
static int pump_message(br_profile *p,br_profile_fence_rx fence,void *context){
    if(!source_current(p))return -1;
    if(p->rxn<2)return 0;
    unsigned opcode=p->rx[0]&15;size_t head=2,n=p->rx[1]&127;
    if(!(p->rx[0]&128)||(p->rx[0]&112)||(p->rx[1]&128)||n==127)return -1;
    if(n==126){if(p->rxn<4)return 0;head=4;n=((size_t)p->rx[2]<<8)|p->rx[3];if(n<126)return -1;}
    if(n>WIRE_CAP||((opcode&8)&&n>125))return -1;
    if(p->rxn<head+n)return 0;
    dc_xz_ws_frame frame;size_t used=0;
    if(dc_xz_ws_decode_frame(p->rx,head+n,0,&frame,p->wire,sizeof p->wire,&used))return -1;
    memmove(p->rx,p->rx+head+n,p->rxn-head-n);p->rxn-=head+n;
    int event=0;uint64_t now=now_ms();
    if(opcode==8){
        if(used==1)return -1;
        if(used>=2&&p->wire[0]==0x0f&&p->wire[1]==0xa1)p->error="token_invalid";
        return -1;
    }
    if(opcode==9){if(ws_send(p,10,p->wire,used,now+2000)<0)return -1;return 100;}
    if(opcode==10)return 100; /* WS PONG is not our Wire heartbeat proof */
    if(opcode!=2)return -1;
    int decoded=br_wire_decode(p->wire,used,BR_FROM_PHONE,p->epoch,allow_encoded,NULL,&p->packet);
    if(decoded!=1){p->error=decoded==BR_WIRE_VERSION?"bridge_version_mismatch":"unsupported";return -1;}
    unsigned type=p->packet.header.type;memset(&p->packet,0,sizeof p->packet);
    if((p->session||p->operation)&&(type==BR_WIRE_AUDIO||type==BR_WIRE_REPLY_END||type==BR_WIRE_CANCEL_ACK||type==BR_WIRE_RESUMED)){
        if(!fence||fence(context,p->run,p->epoch,p->wire,used)<0)return -1;
        event=100;
    }else{
        event=p->session?br_session_receive(p->session,p->run,p->epoch,now,p->wire,used):
            p->operation?br_operation_receive(p->operation,p->epoch,now,p->wire,used):
            br_discovery_receive(p->discovery,p->epoch,now,p->wire,used);
        if(p->session&&p->event&&event)p->event(p->event_context,p->session,now,event);
        if(event<0&&!(p->operation&&event==BR_OP_ERROR)){
            const char *e=p->session?p->session->error:p->discovery->error;
            p->error=e?e:"unsupported";return -1;
        }
    }
    if(type==BR_WIRE_PING){
        size_t written=0;
        if(br_wire_pong(p->wire,used,p->epoch,&p->packet,p->tx,sizeof p->tx,&written)!=1)return -1;
        /* ws_send uses tx, so keep the encoded echo in the separate wire slot. */
        memcpy(p->wire,p->tx,written);
        if(ws_send(p,2,p->wire,written,now+2000)<0)return -1;
    }
    return event?event:100;
}
int br_profile_pump(br_profile *p,uint64_t run,uint64_t epoch,br_profile_fence_rx fence,void *context){
    if(!identity(p,run,epoch)||p->busy)return BR_PROFILE_INVALID;
    if(!p->connected)return BR_PROFILE_CLOSED;
    p->busy=1;uint64_t now=now_ms();
    if(!p->discovery||p->discovery->epoch!=epoch||!source_current(p))goto failed;
    int rc=p->session?br_session_poll(p->session,run,epoch,now):p->operation?br_operation_poll(p->operation,now):br_discovery_poll(p->discovery,now);
    if(p->session&&p->event&&rc)p->event(p->event_context,p->session,now,rc);
    if(rc<0){
        p->error=p->session?p->session->error:p->discovery->error;goto failed;
    }
    if(now-p->probe_ms>=2000&&!(p->discovery->probe&1)){
        size_t n=0;
        if(br_discovery_probe(p->discovery,epoch,now,p->wire,sizeof p->wire,&n)!=1||
           ws_send(p,2,p->wire,n,now+2000)<0)goto failed;
        p->probe_ms=now;
    }
    rc=pump_message(p,fence,context);if(rc<0&&!(p->operation&&rc==BR_OP_ERROR))goto failed;
    if(!rc){
        if(p->rxn==sizeof p->rx)goto failed;
        int n=SSL_read(p->ssl,p->rx+p->rxn,(int)(sizeof p->rx-p->rxn));
        if(n>0){p->rxn+=(size_t)n;rc=pump_message(p,fence,context);if(rc<0&&!(p->operation&&rc==BR_OP_ERROR))goto failed;}
        else{
            int e=SSL_get_error(p->ssl,n);
            if(e!=SSL_ERROR_WANT_READ&&e!=SSL_ERROR_WANT_WRITE)goto failed;
        }
    }
    memset(p->wire,0,sizeof p->wire);memset(&p->packet,0,sizeof p->packet);
    p->busy=0;return rc==100?0:rc;
failed:
    shutdown_profile(p,p->error?p->error:"bridge_unreachable");p->busy=0;return BR_PROFILE_CLOSED;
}

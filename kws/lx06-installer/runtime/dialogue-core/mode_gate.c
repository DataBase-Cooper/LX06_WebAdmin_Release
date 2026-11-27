#define _GNU_SOURCE
#include "mode_gate.h"
#include "io/compat.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

/* Deliberately flat bounded JSON: no arrays/objects, duplicate or unknown keys.
 * Decode Unicode before comparing keys so escaped aliases cannot bypass the
 * duplicate check. UTF-8 and surrogate pairs are checked, never replaced. */
struct field { char key[33],value[257];unsigned string; };
struct object { struct field f[10];unsigned count; };
struct cursor { const unsigned char *p;size_t n,at; };
static void space(struct cursor *c){while(c->at<c->n&&(c->p[c->at]==' '||c->p[c->at]=='\n'||c->p[c->at]=='\r'||c->p[c->at]=='\t'))c->at++;}
static int byte(struct cursor *c,unsigned b){space(c);return c->at<c->n&&c->p[c->at++]==b?0:-1;}
static int hex4(struct cursor *c,unsigned *v){*v=0;if(c->n-c->at<4)return -1;for(unsigned i=0;i<4;i++){unsigned b=c->p[c->at++],d;if(b>='0'&&b<='9')d=b-'0';else if(b>='a'&&b<='f')d=b-'a'+10;else if(b>='A'&&b<='F')d=b-'A'+10;else return -1;*v=*v*16+d;}return 0;}
static int utf8(const unsigned char *p,size_t n){for(size_t i=0;i<n;){unsigned cp=p[i++],k;if(cp<32||cp==127)return 0;if(cp<128)continue;if(cp>=0xc2&&cp<=0xdf){cp&=31;k=1;}else if(cp>=0xe0&&cp<=0xef){cp&=15;k=2;}else if(cp>=0xf0&&cp<=0xf4){cp&=7;k=3;}else return 0;if(i+k>n)return 0;for(unsigned j=0;j<k;j++){unsigned b=p[i++];if((b&0xc0)!=0x80)return 0;cp=(cp<<6)|(b&63);}if((k==1&&cp<128)||(k==2&&cp<2048)||(k==3&&cp<65536)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return 0;}return 1;}
static int string(struct cursor *c,char *out,size_t cap){
    if(byte(c,'"'))return -1;
    size_t at=0;
    while(c->at<c->n){
        unsigned b=c->p[c->at++];
        if(b=='"'){if(!utf8((const unsigned char *)out,at))return -1;out[at]=0;return 0;}
        if(b<32)return -1;
        if(b=='\\'){
            if(c->at==c->n)return -1;
            b=c->p[c->at++];
            if(b=='u'){
                unsigned cp;if(hex4(c,&cp))return -1;
                if(cp>=0xd800&&cp<=0xdbff){unsigned low;if(c->n-c->at<2||c->p[c->at++]!='\\'||c->p[c->at++]!='u'||hex4(c,&low)||low<0xdc00||low>0xdfff)return -1;cp=0x10000+((cp-0xd800)<<10)+low-0xdc00;}
                else if(cp>=0xdc00&&cp<=0xdfff)return -1;
                unsigned char encoded[4];size_t k;
                if(cp<128){encoded[0]=(unsigned char)cp;k=1;}else if(cp<2048){encoded[0]=(unsigned char)(0xc0|(cp>>6));encoded[1]=(unsigned char)(0x80|(cp&63));k=2;}else if(cp<65536){encoded[0]=(unsigned char)(0xe0|(cp>>12));encoded[1]=(unsigned char)(0x80|((cp>>6)&63));encoded[2]=(unsigned char)(0x80|(cp&63));k=3;}else{encoded[0]=(unsigned char)(0xf0|(cp>>18));encoded[1]=(unsigned char)(0x80|((cp>>12)&63));encoded[2]=(unsigned char)(0x80|((cp>>6)&63));encoded[3]=(unsigned char)(0x80|(cp&63));k=4;}
                if(at+k>=cap)return -1;
                memcpy(out+at,encoded,k);at+=k;continue;
            }
            if(b!='"'&&b!='\\'&&b!='/')return -1; /* controls have no role here */
        }
        if(at+1>=cap)return -1;
        out[at++]=(char)b;
    }return -1;
}
static int object(const void *data,size_t size,struct object *o){
    if(!data||!size||size>DC_MODE_REQUEST_CAP)return -1;
    memset(o,0,sizeof *o);struct cursor c={data,size,0};if(byte(&c,'{'))return -1;
    for(;;){
        space(&c);if(c.at<c.n&&c.p[c.at]=='}'){c.at++;break;}
        if(o->count==10)return -1;
        struct field *f=&o->f[o->count];
        if(string(&c,f->key,sizeof f->key)||byte(&c,':'))return -1;
        for(unsigned i=0;i<o->count;i++)if(!strcmp(o->f[i].key,f->key))return -1;
        space(&c);if(c.at==c.n)return -1;
        if(c.p[c.at]=='"'){f->string=1;if(string(&c,f->value,sizeof f->value))return -1;}
        else{size_t at=0;while(c.at<c.n&&c.p[c.at]!=','&&c.p[c.at]!='}'&&c.p[c.at]!=' '&&c.p[c.at]!='\n'&&c.p[c.at]!='\r'&&c.p[c.at]!='\t'){unsigned b=c.p[c.at++];if(at+1==sizeof f->value||b>=128)return -1;f->value[at++]=(char)b;}f->value[at]=0;if(!at)return -1;}
        o->count++;space(&c);if(c.at==c.n)return -1;if(c.p[c.at]=='}'){c.at++;break;}if(c.p[c.at++]!=',')return -1;space(&c);if(c.at==c.n||c.p[c.at]=='}')return -1;
    }space(&c);return c.at==c.n?0:-1;
}
static const struct field *find(const struct object *o,const char *key){for(unsigned i=0;i<o->count;i++)if(!strcmp(o->f[i].key,key))return &o->f[i];return NULL;}
static int value(const struct object *o,const char *key,const char *wanted,unsigned is_string){const struct field *f=find(o,key);return f&&f->string==is_string&&!strcmp(f->value,wanted);}
static int copy(const struct object *o,const char *key,char *out,size_t cap){const struct field *f=find(o,key);if(!f||!f->string||strlen(f->value)>=cap)return -1;strcpy(out,f->value);return 0;}
static int ident(const char *s,size_t low,size_t high,int dots){size_t n=strlen(s);if(n<low||n>high)return 0;for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='_'||s[i]=='-'||(dots&&s[i]=='.')))return 0;return 1;}
static int script(const char *s){size_t n=strlen(s);return ident(s,4,64,1)&&s[0]!='-'&&s[0]!='.'&&!strstr(s,"..")&&!strcmp(s+n-3,".sh");}
int dc_mode_parse_request(enum dc_mode_kind mode,const void *data,size_t size,dc_mode_request *r){
    struct object o;if(!r||(mode!=DC_MODE_PHONE&&mode!=DC_MODE_ACTION_RESULT)||object(data,size,&o)||o.count!=6||!value(&o,"v","1",0))return -1;
    memset(r,0,sizeof *r);
    if(copy(&o,"script_id",r->script,sizeof r->script)||!script(r->script)||copy(&o,"wake_source",r->source,sizeof r->source)||
       (strcmp(r->source,"local_wake")&&strcmp(r->source,"official_asr")&&strcmp(r->source,"manual"))||copy(&o,"wake_text",r->wake,sizeof r->wake)||!r->wake[0]||copy(&o,"action",r->action,sizeof r->action))return -1;
    if(mode==DC_MODE_PHONE)return copy(&o,"target",r->target,sizeof r->target)||strcmp(r->target,"doubao")||strcmp(r->action,"call.start")?-1:0;
    return copy(&o,"status",r->status,sizeof r->status)||!ident(r->action,1,64,1)||(strcmp(r->status,"success")&&strcmp(r->status,"failed")&&strcmp(r->status,"unknown"))?-1:0;
}
int dc_mode_read_request(int fd,enum dc_mode_kind mode,dc_mode_request *r){
    dc_io_stat st;int type;struct ucred peer;socklen_t n=sizeof type,pn=sizeof peer;
    if(fd<3||geteuid()!=0||dc_io_fstat(fd,&st)||!S_ISSOCK(st.st_mode)||st.st_uid||getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&n)||type!=SOCK_SEQPACKET||getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&pn)||peer.uid||peer.pid<=0)return -1;
    char data[DC_MODE_REQUEST_CAP];struct iovec v={data,sizeof data};union{struct cmsghdr align;unsigned char bytes[CMSG_SPACE(16*sizeof(int))];} ancillary;struct msghdr m={0};m.msg_iov=&v;m.msg_iovlen=1;m.msg_control=ancillary.bytes;m.msg_controllen=sizeof ancillary.bytes;
    ssize_t bytes=recvmsg(fd,&m,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);if(bytes<=0)return -1;
    int extra=0;for(struct cmsghdr *c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){extra=1;if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS&&c->cmsg_len>=CMSG_LEN(0)){size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);for(size_t i=0;i<count;i++){int passed;memcpy(&passed,(unsigned char *)CMSG_DATA(c)+i*sizeof(int),sizeof passed);close(passed);}}}
    return extra||(m.msg_flags&(MSG_TRUNC|MSG_CTRUNC))?-1:dc_mode_parse_request(mode,data,(size_t)bytes,r);
}
int dc_mode_escape_text(const char *s,char *out,size_t cap){size_t at=0;for(;*s;s++){if(*s=='"'||*s=='\\'){if(at+2>=cap)return -1;out[at++]='\\';}if(at+1>=cap)return -1;out[at++]=*s;}out[at]=0;return 0;}
int dc_mode_phone_open(const dc_mode_request *r,uint64_t epoch,uint64_t wall,uint64_t mono,dc_phone_gate *g,char *out,size_t cap){
    if(!r||!g||!out||!epoch||!mono||wall<UINT64_C(1704067200000)||wall>=UINT64_C(4102444800000)-10000||mono>UINT64_MAX-10000||!script(r->script)||strcmp(r->target,"doubao")||strcmp(r->action,"call.start"))return -1;
    char wake[193];if(dc_mode_escape_text(r->wake,wake,sizeof wake))return -1;
    memset(g,0,sizeof *g);g->epoch=epoch;g->deadline_ms=mono+10000;snprintf(g->command_id,sizeof g->command_id,"dc-%016llx",(unsigned long long)epoch);
    int n=snprintf(out,cap,"{\"v\":2,\"epoch\":\"%llu\",\"command_id\":\"%s\",\"script_id\":\"%s\",\"wake_source\":\"%s\",\"wake_text\":\"%s\",\"action\":\"call.start\",\"target\":\"doubao\",\"params\":{},\"deadline_unix_ms\":\"%llu\"}",(unsigned long long)epoch,g->command_id,r->script,r->source,wake,(unsigned long long)(wall+10000));
    return n<0||(size_t)n>=cap?-1:n;
}
int dc_mode_phone_tick(dc_phone_gate *g,uint64_t now){if(!g||g->terminal||now>=g->deadline_ms){if(g)g->terminal=1;return -1;}return 0;}
int dc_mode_phone_event(dc_phone_gate *g,const dc_wire_packet *p,uint64_t now){
    if(!p||dc_mode_phone_tick(g,now)||p->header.magic!=DC_WIRE_MAGIC||p->header.version!=DC_WIRE_VERSION||p->header.reserved||p->header.length>DC_WIRE_CAP||p->header.epoch!=g->epoch||p->header.generation||p->header.sequence||p->header.flags||p->header.rate||p->header.channels)goto refuse;
    if(p->header.type==DC_EVT_READY){if(p->header.length||g->ack_state!=2||!g->capability_seen||!g->capable||g->replayed)goto refuse;return 1;}
    struct object o;if(p->header.type!=DC_EVT_TEXT||object(p->payload,p->header.length,&o))goto refuse;
    if(value(&o,"kind","phone_status",1)){
        if(o.count!=3||!value(&o,"v","1",0)||(!value(&o,"production_capable","true",0)&&!value(&o,"production_capable","false",0)))goto refuse;
        g->capability_seen=1;g->capable=value(&o,"production_capable","true",0);if(!g->capable)goto refuse;return 0;
    }
    char epoch[32];snprintf(epoch,sizeof epoch,"%llu",(unsigned long long)g->epoch);
    if(o.count!=7||!value(&o,"v","2",0)||!value(&o,"kind","command_ack",1)||!value(&o,"epoch",epoch,1)||!value(&o,"command_id",g->command_id,1)||(!value(&o,"replayed","true",0)&&!value(&o,"replayed","false",0)))goto refuse;
    char code[65];if(copy(&o,"code",code,sizeof code)||!ident(code,1,64,0))goto refuse;
    unsigned state=value(&o,"status","started",1)?1:value(&o,"status","completed",1)?2:value(&o,"status","failed",1)?3:value(&o,"status","cancelled",1)?4:0;
    if(!state||state>=3||(g->ack_state==2&&(state!=2||strcmp(g->ack_code,code))))goto refuse;
    /* Replays are metadata, not evidence that this connection owns a call.
     * A replay observed before genuine completion permanently fences READY;
     * a matching replay after genuine completion cannot revoke it. */
    if(value(&o,"replayed","true",0)){
        if(g->ack_state!=2)g->replayed=1;
        return 0;
    }
    g->ack_state=state;strcpy(g->ack_code,code);return 0;
refuse:if(g)g->terminal=1;return -1;
}
const char *dc_mode_action_text(const dc_mode_request *r){if(!r)return NULL;return !strcmp(r->status,"success")?"操作已完成。":!strcmp(r->status,"failed")?"操作未成功，请检查。":!strcmp(r->status,"unknown")?"尚未确认操作结果，请先检查。":NULL;}
int dc_mode_child_limit(void){struct rlimit r;if(getrlimit(RLIMIT_NOFILE,&r)||r.rlim_cur<4||r.rlim_cur>1048576)return -1;return (int)r.rlim_cur;}
int dc_mode_child_isolate(int source,int target,int sink,int limit){
    if(source<3||target<3||source>=limit||target>=limit||sink<0||sink==source||limit<4||limit>1048576)return -1;
    if(dup2(sink,STDIN_FILENO)<0||dup2(sink,STDOUT_FILENO)<0||(source!=target&&dup2(source,target)<0)||fcntl(target,F_SETFD,0))return -1;
    for(int fd=3;fd<limit;fd++)if(fd!=target)close(fd);
    return 0;
}

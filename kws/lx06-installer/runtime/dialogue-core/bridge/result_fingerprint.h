/* Private bounded result comparison, NOT authentication or a second parser.
 * Uses the original strict JSON tokens. No allocation, IO, recursion or ABI
 * dependency on the transport's crypto library. Never persist/transmit this.
 * SHA-256 fingerprints only: original Wire/caps/task checks remain mandatory. */
#ifndef LX06_BR_RESULT_FINGERPRINT_H
#define LX06_BR_RESULT_FINGERPRINT_H
#include "../../common/bridge-contract/caps_json.h"
#include <string.h>
typedef struct {
    uint32_t h[8];unsigned char block[64];size_t used;uint64_t bytes;
} br_result_hash;
static uint32_t br_result_rotr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void br_result_block(br_result_hash *s){
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[16],a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for(unsigned i=0;i<16;i++)w[i]=((uint32_t)s->block[4*i]<<24)|((uint32_t)s->block[4*i+1]<<16)|
        ((uint32_t)s->block[4*i+2]<<8)|s->block[4*i+3];
    for(unsigned i=0;i<64;i++){
        if(i>=16){uint32_t x=w[(i-15)&15],y=w[(i-2)&15];
            w[i&15]+=(br_result_rotr(x,7)^br_result_rotr(x,18)^(x>>3))+w[(i-7)&15]+
                (br_result_rotr(y,17)^br_result_rotr(y,19)^(y>>10));}
        uint32_t t=h+(br_result_rotr(e,6)^br_result_rotr(e,11)^br_result_rotr(e,25))+
            ((e&f)^(~e&g))+k[i]+w[i&15];
        uint32_t u=(br_result_rotr(a,2)^br_result_rotr(a,13)^br_result_rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
    memset(w,0,sizeof w);
}
static void br_result_hash_init(br_result_hash *s){
    *s=(br_result_hash){.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
}
static void br_result_hash_add(br_result_hash *s,const void *p,size_t n){
    const unsigned char *data=p;s->bytes+=n;
    while(n){size_t take=64-s->used;if(take>n)take=n;
        memcpy(s->block+s->used,data,take);s->used+=take;data+=take;n-=take;
        if(s->used==64){br_result_block(s);s->used=0;}}
}
static void br_result_hash_finish(br_result_hash *s,unsigned char out[32]){
    uint64_t bits=s->bytes*8;s->block[s->used++]=0x80;
    if(s->used>56){memset(s->block+s->used,0,64-s->used);br_result_block(s);s->used=0;}
    memset(s->block+s->used,0,56-s->used);
    for(unsigned i=0;i<8;i++)s->block[63-i]=(unsigned char)(bits>>(8*i));
    br_result_block(s);
    for(unsigned i=0;i<32;i++)out[i]=(unsigned char)(s->h[i/4]>>(24-8*(i%4)));
    memset(s,0,sizeof *s);
}
static int br_result_key_cmp(const br_json *d,unsigned a,unsigned b){
    br_text x=br_json_text(d,a),y=br_json_text(d,b);size_t n=x.length<y.length?x.length:y.length;
    int rc=memcmp(x.data,y.data,n);return rc?rc:x.length<y.length?-1:x.length>y.length;
}
/* Objects use decoded UTF-8 key byte order, arrays retain order. Strings and
 * keys use decoded bytes and explicit lengths (including NUL). Scalar kinds
 * stay distinct; integers normalize -0 to 0, floats retain strict token text.
 * Container end markers and scalar LE16 lengths make the stream unambiguous.
 * Parsed strict input only, depth<=64 and <=8192B. */
static void br_result_fingerprint(const br_json *d,unsigned char out[32]){
    br_result_hash hash;br_result_hash_init(&hash);
    uint16_t parents[BR_JSON_DEPTH+2]={0},previous[BR_JSON_DEPTH+2]={0};
    unsigned depth=0,node=1;
    while(node){
        unsigned kind=(unsigned)br_json_kind(d,node);unsigned char tag=(unsigned char)kind;
        br_result_hash_add(&hash,&tag,1);
        if(kind==BR_J_OBJECT||kind==BR_J_ARRAY){
            parents[depth]=(uint16_t)node;previous[depth]=0;depth++;
        }else{
            br_text t=br_json_text(d,node);
            if(kind==BR_J_INT&&t.length==2&&t.data[0]=='-'&&t.data[1]=='0'){t.data++;t.length--;}
            unsigned char length[2]={(unsigned char)t.length,(unsigned char)(t.length>>8)};
            br_result_hash_add(&hash,length,2);br_result_hash_add(&hash,t.data,t.length);
        }
        for(;;){
            if(!depth){node=0;break;}
            unsigned parent=parents[depth-1],prev=previous[depth-1],next=0;
            if(br_json_kind(d,parent)==BR_J_ARRAY){
                next=prev?br_json_next(d,prev):br_json_child(d,parent);
                if(next>=br_json_next(d,parent))next=0;
            }else{
                for(unsigned k=br_json_child(d,parent);k&&k<br_json_next(d,parent);k=br_json_next(d,k+1))
                    if((!prev||br_result_key_cmp(d,k,prev)>0)&&(!next||br_result_key_cmp(d,k,next)<0))next=k;
            }
            if(next){
                previous[depth-1]=(uint16_t)next;
                if(br_json_kind(d,parent)==BR_J_OBJECT){
                    br_text key=br_json_text(d,next);unsigned char length[3]={BR_J_KEY,(unsigned char)key.length,(unsigned char)(key.length>>8)};
                    br_result_hash_add(&hash,length,3);br_result_hash_add(&hash,key.data,key.length);next++;
                }
                node=next;break;
            }
            unsigned char end=0;br_result_hash_add(&hash,&end,1);depth--;
        }
    }
    br_result_hash_finish(&hash,out);
}
#endif

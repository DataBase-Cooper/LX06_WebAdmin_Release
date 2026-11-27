/* Bounded 23-round Keccak/SHA3-style public challenge primitive.
 * No provider endpoint, login policy, credential, CAPTCHA or session state.
 * Keccak-f[1600] round 0 is omitted; rate=136, SHA3 domain padding=0x06.
 * Standard rotation/round constants; algorithms are independently implemented.
 */
#ifndef LX06_HASH23_H
#define LX06_HASH23_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

static inline uint64_t hash23_rot(uint64_t v,unsigned n){return n?(v<<n)|(v>>(64-n)):v;}
static inline uint64_t hash23_load(const unsigned char *p) {
    return (uint64_t)p[0]|(uint64_t)p[1]<<8|(uint64_t)p[2]<<16|(uint64_t)p[3]<<24|
           (uint64_t)p[4]<<32|(uint64_t)p[5]<<40|(uint64_t)p[6]<<48|(uint64_t)p[7]<<56;
}
static void hash23_permute(uint64_t a[25]) {
    static const uint64_t rc[24]={
        1,0x8082,UINT64_C(0x800000000000808a),UINT64_C(0x8000000080008000),
        0x808b,0x80000001,UINT64_C(0x8000000080008081),UINT64_C(0x8000000000008009),
        0x8a,0x88,0x80008009,0x8000000a,0x8000808b,UINT64_C(0x800000000000008b),
        UINT64_C(0x8000000000008089),UINT64_C(0x8000000000008003),UINT64_C(0x8000000000008002),
        UINT64_C(0x8000000000000080),0x800a,UINT64_C(0x800000008000000a),
        UINT64_C(0x8000000080008081),UINT64_C(0x8000000000008080),0x80000001,UINT64_C(0x8000000080008008)};
    for(int round=1;round<24;round++) {
        uint64_t c[5],d[5],b[25];
        /* Fixed lane offsets and rotations are deliberate. Generic indexed
         * loops force expensive variable 64-bit shifts/modulo on ARM32. Keep
         * the round loop small; only its 25-lane body is expanded. */
#define HASH23_COLUMN(x) c[x]=a[x]^a[(x)+5]^a[(x)+10]^a[(x)+15]^a[(x)+20]
        HASH23_COLUMN(0);HASH23_COLUMN(1);HASH23_COLUMN(2);HASH23_COLUMN(3);HASH23_COLUMN(4);
#undef HASH23_COLUMN
        d[0]=c[4]^hash23_rot(c[1],1);d[1]=c[0]^hash23_rot(c[2],1);
        d[2]=c[1]^hash23_rot(c[3],1);d[3]=c[2]^hash23_rot(c[4],1);d[4]=c[3]^hash23_rot(c[0],1);
#define HASH23_LANE(dst,src,n) b[dst]=hash23_rot(a[src]^d[(src)%5],n)
        HASH23_LANE(0,0,0); HASH23_LANE(10,1,1); HASH23_LANE(20,2,62); HASH23_LANE(5,3,28); HASH23_LANE(15,4,27);
        HASH23_LANE(16,5,36); HASH23_LANE(1,6,44); HASH23_LANE(11,7,6); HASH23_LANE(21,8,55); HASH23_LANE(6,9,20);
        HASH23_LANE(7,10,3); HASH23_LANE(17,11,10); HASH23_LANE(2,12,43); HASH23_LANE(12,13,25); HASH23_LANE(22,14,39);
        HASH23_LANE(23,15,41); HASH23_LANE(8,16,45); HASH23_LANE(18,17,15); HASH23_LANE(3,18,21); HASH23_LANE(13,19,8);
        HASH23_LANE(14,20,18); HASH23_LANE(24,21,2); HASH23_LANE(9,22,61); HASH23_LANE(19,23,56); HASH23_LANE(4,24,14);
#undef HASH23_LANE
#define HASH23_ROW(i) do { \
        a[i]=b[i]^((~b[(i)+1])&b[(i)+2]); \
        a[(i)+1]=b[(i)+1]^((~b[(i)+2])&b[(i)+3]); \
        a[(i)+2]=b[(i)+2]^((~b[(i)+3])&b[(i)+4]); \
        a[(i)+3]=b[(i)+3]^((~b[(i)+4])&b[i]); \
        a[(i)+4]=b[(i)+4]^((~b[i])&b[(i)+1]); \
        } while(0)
        HASH23_ROW(0);HASH23_ROW(5);HASH23_ROW(10);HASH23_ROW(15);HASH23_ROW(20);
#undef HASH23_ROW
        a[0]^=rc[round];
    }
}
static void hash23_digest(const unsigned char *data,size_t count,unsigned char out[32]) {
    uint64_t state[25]={0};unsigned char block[136];
    while(count>=136) {
        for(size_t i=0;i<17;i++)state[i]^=hash23_load(data+8*i);
        hash23_permute(state);data+=136;count-=136;
    }
    memset(block,0,sizeof(block));memcpy(block,data,count);block[count]=6;block[135]|=0x80;
    for(size_t i=0;i<17;i++)state[i]^=hash23_load(block+8*i);
    hash23_permute(state);
    for(int i=0;i<32;i++)out[i]=(unsigned char)(state[i/8]>>((i%8)*8));
}
static int hash23_solve(const char *salt,const char *expiry,unsigned limit,const unsigned char target[32]) {
    unsigned char input[256],digest[32];
    int prefix=snprintf((char*)input,sizeof(input),"%s_%s_",salt,expiry);
    if(prefix<0||prefix>200||!limit||limit>200000)return -1;
    struct timespec began,now;
    if(clock_gettime(CLOCK_MONOTONIC,&began))return -1;
    (void)setpriority(PRIO_PROCESS,0,15);
    /* The salt/expiry prefix is constant for this one challenge, never reused
     * across challenges. Pre-absorb complete prefix blocks and its tail once.
     * Up to six decimal digits fit in the last block unless the prefix lies
     * directly on a rate boundary; those uncommon lengths retain the general
     * digest path, including the mandatory extra padding block at 136 bytes. */
    size_t tail=(size_t)prefix%136;
    int single_tail=tail+6<136;
    uint64_t seed[25]={0},wanted[4];
    if(single_tail) {
        size_t left=(size_t)prefix;const unsigned char *p=input;
        while(left>=136) {
            for(size_t i=0;i<17;i++)seed[i]^=hash23_load(p+8*i);
            hash23_permute(seed);p+=136;left-=136;
        }
        for(size_t i=0;i<left;i++)seed[i/8]^=(uint64_t)p[i]<<((i%8)*8);
        for(int i=0;i<4;i++)wanted[i]=hash23_load(target+8*i);
    }
    unsigned char decimal[6]={'0'};unsigned digits=1;
    for(unsigned n=0;n<limit;n++) {
        if(!(n&255)) {
            if(clock_gettime(CLOCK_MONOTONIC,&now)||now.tv_sec-began.tv_sec>=15)return -1;
        }
        if(single_tail) {
            uint64_t state[25];memcpy(state,seed,sizeof(state));
            for(unsigned i=0;i<digits;i++) {
                size_t at=tail+i;state[at/8]^=(uint64_t)decimal[i]<<((at%8)*8);
            }
            size_t end=tail+digits;
            state[end/8]^=UINT64_C(6)<<((end%8)*8);
            state[16]^=UINT64_C(0x8000000000000000);
            hash23_permute(state);
            if(state[0]==wanted[0]&&state[1]==wanted[1]&&state[2]==wanted[2]&&state[3]==wanted[3])return (int)n;
        } else {
            memcpy(input+prefix,decimal,digits);
            hash23_digest(input,(size_t)prefix+digits,digest);
            if(!memcmp(digest,target,32))return (int)n;
        }
        /* Increment the same ASCII spelling snprintf("%u") would produce,
         * without invoking a variadic formatter for each candidate. */
        unsigned place=digits;
        while(place&&decimal[place-1]=='9')decimal[--place]='0';
        if(place)decimal[place-1]++;
        else {memmove(decimal+1,decimal,digits);decimal[0]='1';digits++;}
    }
    return -1;
}
#endif

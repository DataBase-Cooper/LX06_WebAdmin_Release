/* Frozen pre-optimization reference for differential tests/benchmarks only. */
/* Bounded 23-round Keccak/SHA3-style public challenge primitive.
 * No provider endpoint, login policy, credential, CAPTCHA or session state.
 * Keccak-f[1600] round 0 is omitted; rate=136, SHA3 domain padding=0x06.
 * Standard rotation/round constants; algorithms are independently implemented.
 */
#ifndef LX06_REFERENCE_HASH23_H
#define LX06_REFERENCE_HASH23_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

static uint64_t reference_hash23_rot(uint64_t v,unsigned n){return n?(v<<n)|(v>>(64-n)):v;}
static void reference_hash23_permute(uint64_t a[25]) {
    static const uint64_t rc[24]={
        1,0x8082,UINT64_C(0x800000000000808a),UINT64_C(0x8000000080008000),
        0x808b,0x80000001,UINT64_C(0x8000000080008081),UINT64_C(0x8000000000008009),
        0x8a,0x88,0x80008009,0x8000000a,0x8000808b,UINT64_C(0x800000000000008b),
        UINT64_C(0x8000000000008089),UINT64_C(0x8000000000008003),UINT64_C(0x8000000000008002),
        UINT64_C(0x8000000000000080),0x800a,UINT64_C(0x800000008000000a),
        UINT64_C(0x8000000080008081),UINT64_C(0x8000000000008080),0x80000001,UINT64_C(0x8000000080008008)};
    static const unsigned rot[25]={0,1,62,28,27,36,44,6,55,20,3,10,43,25,39,41,45,15,21,8,18,2,61,56,14};
    for(int round=1;round<24;round++) {
        uint64_t c[5],d[5],b[25];
        for(int x=0;x<5;x++)c[x]=a[x]^a[x+5]^a[x+10]^a[x+15]^a[x+20];
        for(int x=0;x<5;x++)d[x]=c[(x+4)%5]^reference_hash23_rot(c[(x+1)%5],1);
        for(int y=0;y<5;y++)for(int x=0;x<5;x++) {
            int i=x+5*y;b[y+5*((2*x+3*y)%5)]=reference_hash23_rot(a[i]^d[x],rot[i]);
        }
        for(int y=0;y<5;y++)for(int x=0;x<5;x++)a[x+5*y]=b[x+5*y]^((~b[(x+1)%5+5*y])&b[(x+2)%5+5*y]);
        a[0]^=rc[round];
    }
}
static void reference_hash23_digest(const unsigned char *data,size_t count,unsigned char out[32]) {
    uint64_t state[25]={0};unsigned char block[136];
    while(count>=136) {
        for(size_t i=0;i<136;i++)state[i/8]^=(uint64_t)data[i]<<((i%8)*8);
        reference_hash23_permute(state);data+=136;count-=136;
    }
    memset(block,0,sizeof(block));memcpy(block,data,count);block[count]=6;block[135]|=0x80;
    for(size_t i=0;i<136;i++)state[i/8]^=(uint64_t)block[i]<<((i%8)*8);
    reference_hash23_permute(state);
    for(int i=0;i<32;i++)out[i]=(unsigned char)(state[i/8]>>((i%8)*8));
}
static int reference_hash23_solve(const char *salt,const char *expiry,unsigned limit,const unsigned char target[32]) {
    unsigned char input[256],digest[32];
    int prefix=snprintf((char*)input,sizeof(input),"%s_%s_",salt,expiry);
    if(prefix<0||prefix>200||!limit||limit>200000)return -1;
    struct timespec began,now;
    if(clock_gettime(CLOCK_MONOTONIC,&began))return -1;
    (void)setpriority(PRIO_PROCESS,0,15);
    for(unsigned n=0;n<limit;n++) {
        if(!(n&255)) {
            if(clock_gettime(CLOCK_MONOTONIC,&now)||now.tv_sec-began.tv_sec>=15)return -1;
        }
        int digits=snprintf((char*)input+prefix,sizeof(input)-(size_t)prefix,"%u",n);
        if(digits<0)return -1;
        reference_hash23_digest(input,(size_t)(prefix+digits),digest);
        if(!memcmp(digest,target,32))return (int)n;
    }
    return -1;
}
#endif

/* Exact miio_helper 1.94.13 call-site compatibility fix, ARM EABI.
 * Preserve the original DID string before atoi loses its high bits, then use
 * the stock 64-bit JSON API at the matching response_dinfo field only.
 * No identity files, credentials, network connections or global JSON changes.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static __thread long long pending_did;
static __thread int pending_valid;

/* Builder pins the complete executable SHA. At runtime additionally require
 * the exact instruction sequence and return PC before touching this path. */
static int target_site(uintptr_t caller, uintptr_t expected) {
    static const uint32_t code[]={0xe28400f8,0xebffd8b1,0xe59f117c,
                                 0xe1a02000,0xe1a00006,0xebffd925};
    return caller==expected &&
        !memcmp((const void *)(uintptr_t)0x1e668,code,sizeof(code));
}

static int parse_did(const char *s,long long *value) {
    if(!s || !*s)return 0;
    unsigned n=0;long long v=0;
    for(const unsigned char *p=(const unsigned char *)s;*p;++p) {
        if(*p<'0'||*p>'9'||++n>19)return 0;
        int digit=*p-'0';
        if(v>(LLONG_MAX-digit)/10)return 0;
        v=v*10+digit;
    }
    if(v<=0)return 0;
    *value=v;return 1;
}

int atoi(const char *s) {
    uintptr_t caller=(uintptr_t)__builtin_return_address(0);
    if(target_site(caller,0x1e670)) {
        pending_valid=parse_did(s,&pending_did);
        /* This intermediate integer is consumed only by the paired hook.
         * Preserve ordinary small-DID behavior without overflowing atoi. */
        return pending_valid && pending_did<=INT_MAX ? (int)pending_did:INT_MAX;
    }
    int (*original)(const char *)=dlsym(RTLD_NEXT,"atoi");
    if(original)return original(s);
    errno=ENOSYS;return 0;
}

int json_add_int(void *object,const char *key,int value) {
    uintptr_t caller=(uintptr_t)__builtin_return_address(0);
    if(target_site(caller,0x1e680) && key && !strcmp(key,"did")) {
        int valid=pending_valid;long long did=pending_did;pending_valid=0;
        int (*wide)(void *,const char *,long long)=dlsym(RTLD_NEXT,"json_add_int64");
        if(!valid || !wide) {
            syslog(LOG_ERR,"lx06-did-width: invalid DID or missing int64 API; refusing truncated identity");
            return -1;
        }
        return wide(object,key,did);
    }
    int (*original)(void *,const char *,int)=dlsym(RTLD_NEXT,"json_add_int");
    return original ? original(object,key,value):-1;
}

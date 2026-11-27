#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
extern void *json_object_new_object(void);
extern void *json_object_object_get(void *,const char *);
extern long long json_object_get_int64(void *);
extern int json_object_get_type(void *);
extern void json_object_put(void *);
extern int json_add_int(void *,const char *,int);
extern int fixture_run(const char *,void *);
extern long long legacy_strtoll(const char *,char **,int) __asm__("strtoll");
__asm__(".text\n.arm\n.global fixture_run\nfixture_run:\n"
        "push {r4,r5,r6,lr}\nsub r4,r0,#248\nmov r6,r1\nldr pc,=0x1e668\n");
static void gateway(uintptr_t where,void *function) {
    uint32_t code[]={0xe51ff004,(uint32_t)(uintptr_t)function};
    memcpy((void *)where,code,sizeof(code));
}
int main(void) {
    assert(mmap((void *)0x14000,0x1000,7,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)0x14000);
    assert(mmap((void *)0x1e000,0x1000,7,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)0x1e000);
    uint32_t code[]={0xe28400f8,0xebffd8b1,0xe59f117c,0xe1a02000,0xe1a00006,0xebffd925,0xe8bd8070};
    memcpy((void *)0x1e668,code,sizeof(code));
    *(const char **)0x1e7f4="did";
    gateway(0x14938,dlsym(RTLD_DEFAULT,"atoi"));
    gateway(0x14b18,dlsym(RTLD_DEFAULT,"json_add_int"));
    __builtin___clear_cache((char *)0x14000,(char *)0x15000);
    __builtin___clear_cache((char *)0x1e000,(char *)0x1f000);
    const char *ids[]={"123456789","2147483647","2147483648","3000000000",
        "4294967295","4294967296","10000000000","9223372036854775807"};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);++i) {
        void *o=json_object_new_object();assert(fixture_run(ids[i],o)==0);
        void *did=json_object_object_get(o,"did");
        assert(did && json_object_get_type(did)==3);
        assert(json_object_get_int64(did)==legacy_strtoll(ids[i],NULL,10));
        printf("DID %s preserved as JSON int64\n",ids[i]);json_object_put(o);
    }
    const char *bad[]={"","0","-1","+123","12x"," 123","9223372036854775808","999999999999999999999"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        void *o=json_object_new_object();assert(fixture_run(bad[i],o)==-1);
        assert(!json_object_object_get(o,"did"));json_object_put(o);
    }
    void *o=json_object_new_object();assert(json_add_int(o,"did",77)==0);
    assert(json_object_get_int64(json_object_object_get(o,"did"))==77);
    assert(json_add_int(o,"volume",23)==0);
    assert(json_object_get_int64(json_object_object_get(o,"volume"))==23);
    int (*ordinary_atoi)(const char *)=dlsym(RTLD_DEFAULT,"atoi");
    assert(ordinary_atoi("3000000000")==2147483647); /* ordinary call retains stock ABI */
    json_object_put(o);puts("invalid IDs rejected; unrelated calls unchanged; PASS");
    return 0;
}

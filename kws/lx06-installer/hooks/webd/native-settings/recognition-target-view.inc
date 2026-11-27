/* A-05 HOST FIXTURE ONLY. Original serializer, lock and retained revision.
 * Public payload and private settings record have DIFFERENT budgets.
 * No product endpoint, runtime grant, new parser, store or writer. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition target view is fixture-only"
#endif
struct rtv_snapshot {
    struct ctl_revision *revision; /* defined by the original owner below */
    char *data;
    size_t bytes,budget;
};
#ifndef RTV_ALLOC
#define RTV_ALLOC(n) malloc(n)
#endif
#ifndef RTV_FREE
#define RTV_FREE(p,n) free(p)
#endif
static void rtv_wipe(void *data,size_t bytes) {
    volatile unsigned char *p=data;
    while(bytes--) *p++=0;
}
static int rtv_render(const struct cfg *cfg,const struct ctl_map *map,struct rtv_snapshot *s) {
    if(!s||s->data||s->bytes||s->budget>GSV_BYTES) return -1;
    struct gsv_buf count={.capacity=s->budget};
    if(gsv_public(cfg,map,&count)) return -1;
    char *data=RTV_ALLOC(count.used+1);
    if(!data) return -1;
    data[0]=0;
    struct gsv_buf write={.data=data,.capacity=count.used};
    if(gsv_public(cfg,map,&write)||write.used!=count.used) {
        rtv_wipe(data,count.used+1);RTV_FREE(data,count.used+1);return -1;
    }
    s->data=data;s->bytes=count.used;return 0;
}

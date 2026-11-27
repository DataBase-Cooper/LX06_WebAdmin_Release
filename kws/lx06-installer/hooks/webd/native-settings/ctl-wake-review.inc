/* A HOST ONLY dependency component, not a public/global lease. Include after
 * ORIGINAL wake readers, ctl-owner, wc_blob helpers. Caller already holds the
 * settings writer scope. Borrow existing wake mutation lock NONBLOCKING;
 * no creation, repairs, models, subprocesses, keeper RPC or runtime decision.
 * Other dependencies MUST remain covered by the caller's external owner. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) || !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "wake review component is fixture-only"
#endif
#include "ctl-wake-mutation.inc"
#define WAKE_REVIEW_BYTES 8192u
struct wake_review_source {
    struct wake_mutation_scope *mutation;
    int failed;
    struct wc_blob conf,official,binding;
    size_t count;
    char words[WW_MAXWORDS][24];
    int official_enabled,binding_present;
    char bound[80];
};
static void wake_review_erase(void *bytes,size_t size) {
    volatile unsigned char *p=bytes;
    for(size_t i=0;i<size;i++) p[i]=0;
}
static int wake_review_release(struct wake_review_source *s) {
    if(!s) return 0;
    int rc=0;
    wc_blob_release(&s->conf);wc_blob_release(&s->official);wc_blob_release(&s->binding);
    if(wake_mutation_release(s->mutation)) rc=-1;
    wake_review_erase(s,sizeof(*s));
    free(s);return rc;
}
static int wake_review_current(struct wake_review_source *s) {
    if(!s||s->failed) return -1;
    if(wake_mutation_current(s->mutation)||
       wc_blob_current(s->mutation->wake,"wakeword.conf",&s->conf)||
       wc_blob_current(s->mutation->wake,"official_xiaoai",&s->official)||
       wc_blob_current(s->mutation->wake,"official_bind",&s->binding)) {
        s->failed=1;return -1;
    }
    return 0;
}
static struct wake_review_source *wake_review_acquire(void *scope,
        const char *wake_path,const char *lock_directory) {
    if(!scope||ctl_writer_current(scope)||!wake_path||!lock_directory) return NULL;
    struct wake_review_source *s=calloc(1,sizeof(*s));if(!s) return NULL;
    s->mutation=wake_mutation_acquire(wake_path,lock_directory);
    if(!s->mutation) goto bad;
    int wake=s->mutation->wake;
    if(wc_blob_capture(wake,"wakeword.conf",WAKE_REVIEW_BYTES,&s->conf)||
       wc_blob_capture(wake,"official_xiaoai",WAKE_REVIEW_BYTES,&s->official)||
       wc_blob_capture(wake,"official_bind",79,&s->binding)||
       (s->conf.present&&memchr(s->conf.bytes,0,s->conf.size))||
       (s->binding.present&&memchr(s->binding.bytes,0,s->binding.size))||
       wake_review_current(s)) goto bad;
    char path[128];ww_word_t words[WW_MAXWORDS+1];memset(words,0,sizeof(words));
    snprintf(path,sizeof(path),"/proc/self/fd/%d/wakeword.conf",wake);
    /* Original parser reads the same anchored file; never a copied grammar.
     * One extra slot detects a truncated valid word set instead of blessing it. */
    wake_review_conf_path=path;
    wake_review_io_failed=0;
    int count=ww_read_conf(words,WW_MAXWORDS+1);
    wake_review_conf_path=NULL;
    if(count<0||count>WW_MAXWORDS) goto bad_words;
    s->count=(size_t)count;
    for(int i=0;i<count;i++) strcpy(s->words[i],words[i].name);
    snprintf(path,sizeof(path),"/proc/self/fd/%d/official_xiaoai",wake);
    s->official_enabled=lx06_official_kws_enabled_at(path);
    snprintf(path,sizeof(path),"/proc/self/fd/%d/official_bind",wake);
    s->binding_present=lx06_official_bind_read_at(path,s->bound,sizeof(s->bound));
    if(wake_review_io_failed||s->binding_present<0||ctl_writer_current(scope)||wake_review_current(s)) goto bad_words;
    wake_review_erase(words,sizeof(words));return s;
bad_words:
    wake_review_erase(words,sizeof(words));
bad:
    wake_review_release(s);return NULL;
}

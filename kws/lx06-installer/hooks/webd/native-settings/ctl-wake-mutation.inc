/* A HOST FIXTURE ONLY. One participant in the ORIGINAL wake mutation flock.
 * No bootstrap, reader, parser, public lease, settings lock or runtime grant.
 * Lock order for composite callers: settings -> wake; never acquire settings
 * from a callback holding wake. External owners retain their own dependencies. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "wake mutation participant is fixture-only"
#endif
#ifndef LX06_WAKE_MUTATION_INCLUDED
#define LX06_WAKE_MUTATION_INCLUDED
struct wake_mutation_scope {
    int wake,locks,lock,failed,checking;
    struct ctl_store_root *wake_root,*lock_root;
    struct stat lock_stat;
};
static int wake_mutation_release(struct wake_mutation_scope *s) {
    if(!s) return 0;
    int rc=0;
    if(s->lock>=0&&close(s->lock)) rc=-1;
    if(s->wake>=0&&close(s->wake)) rc=-1;
    if(s->locks>=0&&close(s->locks)) rc=-1;
    ctl_store_root_release(s->wake_root);ctl_store_root_release(s->lock_root);
    free(s);return rc;
}
static int wake_mutation_current(struct wake_mutation_scope *s) {
    struct stat opened,named;
    if(!s||s->failed) return -1;
    if(ctl_store_root_current(s->wake_root,s->wake)||
       ctl_store_root_current(s->lock_root,s->locks)||
       fstat(s->lock,&opened)||
       fstatat(s->locks,"lx06-ww-mutate.lock",&named,AT_SYMLINK_NOFOLLOW)||
       !ctl_same_stat(&s->lock_stat,&opened)||!ctl_same_stat(&opened,&named)) {
        s->failed=1;return -1;
    }
    return 0;
}
static struct wake_mutation_scope *wake_mutation_acquire(const char *wake_path,const char *lock_path) {
    if(!wake_path||!lock_path) return NULL;
    struct wake_mutation_scope *s=calloc(1,sizeof(*s));if(!s) return NULL;
    s->wake=s->locks=s->lock=-1;
    s->wake=open(wake_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    s->locks=open(lock_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(s->wake<0||s->locks<0) goto bad;
    s->wake_root=ctl_path_root_open(wake_path,s->wake);
    s->lock_root=ctl_path_root_open(lock_path,s->locks);
    if(!s->wake_root||!s->lock_root||!ctl_script_dir(s->wake)) goto bad;
    s->lock=app_open_file(s->locks,"lx06-ww-mutate.lock",O_RDWR);
    if(s->lock<0||flock(s->lock,LOCK_EX|LOCK_NB)||fstat(s->lock,&s->lock_stat)||
       wake_mutation_current(s)) goto bad;
    return s;
bad:
    wake_mutation_release(s);return NULL;
}
/* Private synchronous call receipt. Dispatch is not a persistence guarantee:
 * the original handler/result owns mutation, error and durability semantics.
 * A release error cannot turn an already completed callback into a retry. */
struct wake_mutation_call { int entered,result,release_failed; };
typedef int (*wake_mutation_callback)(struct wake_mutation_scope *,void *);
/* Reuse immediately before an original commit syscall, including after its
 * fsync. An observed owner refusal is sticky for this borrowed scope. */
static int wake_mutation_guard(struct wake_mutation_scope *s,ctl_owner_gate gate,void *ctx) {
    if(!s) return -1;
    if(s->checking) {s->failed=1;return -1;}
    s->checking=1;
    if(wake_mutation_current(s)||(gate&&gate(ctx))||wake_mutation_current(s)) s->failed=1;
    s->checking=0;return s->failed?-1:0;
}
static int wake_mutation_run(const char *wake,const char *locks,
        wake_mutation_callback callback,void *context,ctl_owner_gate gate,void *gate_context,
        struct wake_mutation_call *out) {
    if(!out) return -1;
    *out=(struct wake_mutation_call){0,-1,0};
    if(!callback) return -1;
    struct wake_mutation_scope *s=wake_mutation_acquire(wake,locks);
    if(!s) return -1;
    if(wake_mutation_guard(s,gate,gate_context)) {
        out->release_failed=wake_mutation_release(s)!=0;return -1;
    }
    out->entered=1;out->result=callback(s,context);
    out->release_failed=wake_mutation_release(s)!=0;
    return 0;
}
#endif

/* T21/T24 synchronous HOST FIXTURE ONLY; include after ctl-owner.inc.
 * Cooperating upload/delete/metadata writers join the EXISTING settings .lock.
 * No new lock, parser, file writer, deployment sequence or product endpoint.
 * Mutation callbacks retain their original commit/durability return values. */
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "T24 writer participant is fixture-only"
#endif
struct ctl_writer_scope {
    int store, lock;
    struct ctl_store_root *root;
};
typedef int (*ctl_writer_mutation)(void *borrowed_scope,void *ctx);

/* Borrowed synchronously, only inside the mutation callback. Call this before
 * each actual filesystem mutation when a callback has multiple steps. */
int ctl_writer_current(void *borrowed_scope) {
    struct ctl_writer_scope *s=borrowed_scope;
    return !s||ctl_store_root_current(s->root,s->store)||
        cfg_store_lock_valid(s->store,s->lock)?-1:0;
}
int ctl_writer_store(void *borrowed_scope) {
    struct ctl_writer_scope *s=borrowed_scope;
    return ctl_writer_current(s)?-1:s->store;
}
int ctl_writer_lock(void *borrowed_scope) {
    struct ctl_writer_scope *s=borrowed_scope;
    return ctl_writer_current(s)?-1:s->lock;
}

/* 0 = callback ran, *result is ITS unchanged result (even negative/uncertain).
 * -1 = unavailable, unsafe or busy; -2 = gate refused before mutation.
 * Refusals never alter *result. These are private fixture codes, not HTTP/Wire.
 * Do not retry a callback automatically or check cancellation after its commit.
 * The existing store must already exist; bootstrap remains an owner concern. */
int ctl_writer_run(ctl_writer_mutation mutation,void *ctx,ctl_owner_gate gate,
                   void *gate_ctx,int *result) {
    if(!mutation||!result) return -1;
    struct ctl_writer_scope s={.store=-1,.lock=-1,.root=NULL};
    int rc=-1;
    if(gate&&gate(gate_ctx)) return -2;
    s.store=cfg_store_open(0);
    if(s.store<0) goto done;
    s.root=ctl_store_root_open(s.store);
    if(!s.root) goto done;
    s.lock=openat(s.store,".lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(s.lock<0||ctl_writer_current(&s)) goto done;
    if(gate&&gate(gate_ctx)) { rc=-2; goto done; }
    /* A synchronous gate can replace an ancestor or the lock pathname. */
    if(ctl_writer_current(&s)) goto done;
    *result=mutation(&s,ctx);
    rc=0;
done:
    /* Never LOCK_UN: closing only our reference preserves any legitimate
     * same-open-description lease retained by the existing owner machinery. */
    if(s.lock>=0) close(s.lock);
    ctl_store_root_release(s.root);
    if(s.store>=0) close(s.store);
    return rc;
}

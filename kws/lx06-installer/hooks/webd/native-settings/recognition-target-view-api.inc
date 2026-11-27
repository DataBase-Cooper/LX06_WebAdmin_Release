/* Include after the original owner and recognition projection. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "recognition target view API is fixture-only"
#endif
void recognition_target_view_close(struct rtv_snapshot *s) {
    if(!s) return;
    ctl_owner_release(s->revision);
    if(s->data) {rtv_wipe(s->data,s->bytes+1);RTV_FREE(s->data,s->bytes+1);}
    rtv_wipe(s,sizeof(*s));free(s);
}
/* Snapshot bytes are immutable, owned until close, and include NO record or
 * executable body. No path-currency guarantee or permission to execute/save.
 * Return length explicitly; the terminator is outside the payload budget. */
const char *recognition_target_view_data(const struct rtv_snapshot *s,size_t *bytes) {
    if(!s||!s->revision||!s->data||!bytes) return NULL;
    *bytes=s->bytes;return s->data;
}
int recognition_target_view_recognition(const struct rtv_snapshot *s,const char *field,const char *app,
        rec_resolve_prompt resolve,rec_model_ready ready,void *ctx,char out[CFG_BYTES+1]) {
    return s?rec_owner_loaded_view(s->revision,field,app,resolve,ready,ctx,out):-1;
}
/* Private synchronous loan. Public view_data never includes this projection. */
int recognition_target_profile_borrow(struct rtv_snapshot *s,int scripts,struct ts_profile *out) {
    return s?ctl_owner_profile_borrow(scripts,s->revision,out):-1;
}
int recognition_target_profile_current(struct rtv_snapshot *s,int scripts) {
    return s?ctl_owner_profile_current(scripts,s->revision):-1;
}
void recognition_target_profile_release(struct rtv_snapshot *s) {
    if(s&&s->revision) s->revision->profile_ready=s->revision->profile_borrowed=0;
}

/* Normal Web entry borrows the original native joint settings transaction.
 * No second writer, model request, automatic retry or transport lives here. */
#ifndef LX06_APP_TARGET_SETTINGS_H
#define LX06_APP_TARGET_SETTINGS_H
#include <stddef.h>
struct rtv_snapshot;
struct ts_profile;
#ifndef LX06_REC_UNBIND_RECEIPT_DEFINED
#define LX06_REC_UNBIND_RECEIPT_DEFINED
struct rec_unbind_receipt {
    int settings_result,required,attempted,binding_result,release_failed;
};
#endif
/* Read-only declaration/schema check, never permission to execute or save. */
int app_target_settings_supported(int source_fd);
/* Same immutable loaded record; only schema-declared non-password values are
 * projected. Absent values stay absent. Credentials/defaults are never public. */
int app_target_settings_load(int scripts,int source_fd,const char *name,
    int (*gate)(void *),void *ctx,struct rtv_snapshot **loaded,char public_values[8193]);
/* One non-preview attempt consumes loaded's save readiness on every outcome.
 * Only rc0 returns saved; rc1 is uncertain and rc2 is saved/unbind incomplete.
 * The Web caller reuses its original auth/source/busy gates on each call. */
int app_target_settings_save_connection(int scripts,int source_fd,const char *name,
    struct rtv_snapshot *loaded,const char *connection,size_t length,
    int (*gate)(void *),void *ctx,struct rtv_snapshot **saved,struct rec_unbind_receipt *receipt);
const char *recognition_target_view_data(const struct rtv_snapshot *,size_t *);
void recognition_target_view_close(struct rtv_snapshot *);
int recognition_target_profile_borrow(struct rtv_snapshot *,int scripts,struct ts_profile *);
int recognition_target_profile_current(struct rtv_snapshot *,int scripts);
void recognition_target_profile_release(struct rtv_snapshot *);
#endif

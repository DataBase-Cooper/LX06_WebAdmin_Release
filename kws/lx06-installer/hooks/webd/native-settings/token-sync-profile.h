/* A-owned saved connection projection. Borrowed only for one caps request. */
#ifndef LX06_TOKEN_SYNC_PROFILE_H
#define LX06_TOKEN_SYNC_PROFILE_H
#include <stddef.h>
#include <stdint.h>
struct ts_profile {
    char host[129],ca_file[2049],token[129],spki[65];unsigned port;
    char phone_allowlist[2049],speaker_allowlist[2049];
};
/* One synchronous continuation of a confirmed native save. No second save,
 * retry, business operation or source permission is supplied by this adapter.
 * gate follows the original settings convention: 0=current, nonzero=refused;
 * it is adapted to B16's 1=current contract only inside the implementation.
 * Caller holds saved bytes and stable_ca through return, then wipes/releases. */
struct ts_b16;
typedef int (*ts_saved_current)(void *);
enum { TS_SYNC_UNCONFIRMED=-1, TS_SYNC_FAILED=0, TS_SYNC_READY=1,
       TS_SYNC_TOKEN_INVALID=4001 };
struct ts_b16 *ts_b16_new(void);
void ts_b16_free(struct ts_b16 *);
int ts_b16_caps_saved(struct ts_b16 *, const struct ts_profile *, const char *stable_ca,
    uint64_t run, uint64_t epoch, const void *caps_open, size_t length,
    ts_saved_current gate, void *context);
#endif

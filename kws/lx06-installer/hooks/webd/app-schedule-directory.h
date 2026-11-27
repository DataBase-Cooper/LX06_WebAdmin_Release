/* A-owned complete deployed-directory snapshot. HOST candidate only.
 * No directory path is guessed by B and no timer or idle authority is added. */
#ifndef LX06_APP_SCHEDULE_DIRECTORY_H
#define LX06_APP_SCHEDULE_DIRECTORY_H
#include "app-extension-objects.h"
#ifndef LX06_INTERACTIVE_FLOW_FIXTURE
#error "saved schedule directory requires retained native sources"
#endif
struct eo_schedule_directory;
/* One declared naming contract, shared with a future writer: full deployed
 * script name (including extension) + .json, in both settings and carrier
 * directories. No stem stripping, alternate names or upload fallback.
 * Example: apps/alarm.sh -> settings/alarm.sh.json + schedules/alarm.sh.json.
 * The owner supplies trusted opened dirs [formal apps, settings, schedules],
 * their opened parents and exact parent entries. APPDIR is never an input.
 * Missing carrier means no tasks. An explicit empty carrier is still parsed,
 * hashed and protected by the same lock. Orphans/invalid carriers fail closed.
 * Every effective deployed name is enumerated; hidden/non-executable names
 * excluded by the original app_name_safe syntax are metadata, not scripts.
 */
int eo_schedule_filename(const char *script,char out[80]);
/* Fresh output only; failure leaves *out NULL. old is optional solely to share
 * its existing verified settings lock during a staged reload. A replacement
 * never mutates/releases the old snapshot, owners, registry or state records.
 * revision is the original nonzero A configuration generation, not wall time.
 */
int eo_schedule_directory_load(struct eo_schedule_directory **out,
    const int directories[3],const int parents[3],const char *const entries[3],
    uint64_t revision,eo_digest,eo_nfc,void *,const struct eo_schedule_directory *old);
size_t eo_schedule_directory_count(const struct eo_schedule_directory *);
size_t eo_schedule_directory_tasks(const struct eo_schedule_directory *);
size_t eo_schedule_directory_encoded_bytes(const struct eo_schedule_directory *);
/* Actual retained A allocations, including each full eo_view. This is not
 * B's existing runner-only accounting. Parser/transient scratch is additional.
 * Current retained A representation exceeds the formal whole-scheduler budget
 * for nonempty configurations: production resource acceptance stays blocked. */
size_t eo_schedule_directory_retained_bytes(const struct eo_schedule_directory *);
/* Nonempty definitions, in bytewise script-name order; disabled tasks count.
 * Empty definitions remain part of the full snapshot, not runner capacity.
 * Returned pointers remain borrowed until successful close. */
int eo_schedule_directory_at(const struct eo_schedule_directory *,size_t,
    const char **script,struct eo_owner **,struct eo_saved_source **);
/* Complete apps/settings/carrier names + identities + metadata, original
 * anchors and named lock, every retained source, and empty carrier contents.
 * The complete encoded name-set budget is 64KiB; overflow explicitly refuses.
 * Enumeration uses a 512-byte getdents64 buffer and two <=256-byte names,
 * rescanning in lexical order without libc DIR allocation or a collected list.
 * Directory metadata must remain unchanged throughout each complete scan. */
int eo_schedule_directory_current(const struct eo_schedule_directory *);
/* Registry lifetime loan: one consumer; cannot close while registered or held.
 * Binding uses the original registry and must release only after its reset. */
int eo_schedule_directory_retain(struct eo_schedule_directory *);
int eo_schedule_directory_unretain(struct eo_schedule_directory *);
int eo_schedule_directory_close(struct eo_schedule_directory *);
#endif

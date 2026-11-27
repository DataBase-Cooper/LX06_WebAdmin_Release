/* Complete compact A directory scope, using the same canonical carrier map. */
#ifndef LX06_APP_SCHEDULE_COMPACT_DIRECTORY_H
#define LX06_APP_SCHEDULE_COMPACT_DIRECTORY_H
#include "app-schedule-compact.h"
struct eo_compact_schedule_directory;
typedef int (*eo_compact_schedule_script_load)(void *,struct eo_compact_schedule_root *,const char *script);
/* Every carrier, including empty ones, invokes load exactly once in byte order.
 * The callback must fully parse that source with the same A root and can only
 * stage unpublished data. Any refusal leaves *out NULL; caller discards all
 * its unpublished typed staging. No incumbent source or registry is modified.
 * root's real original lock/anchors remain retained, including empty config. */
int eo_compact_schedule_directory_load(struct eo_compact_schedule_root *,
    eo_compact_schedule_script_load,void *,struct eo_compact_schedule_directory **out);
int eo_compact_schedule_directory_current(void *);
int eo_compact_schedule_directory_retain(void *);
int eo_compact_schedule_directory_unretain(void *);
int eo_compact_schedule_directory_close(struct eo_compact_schedule_directory **);
size_t eo_compact_schedule_directory_bytes(const struct eo_compact_schedule_directory *);
size_t eo_compact_schedule_directory_encoded(const struct eo_compact_schedule_directory *);
#endif

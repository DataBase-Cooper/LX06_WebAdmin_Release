/* Isolated state machine only. Caller must authenticate identities and serialize
 * all calls. It provides no IPC, persistence, process or lease authentication. */
#ifndef LX06_SESSION_STATUS_H
#define LX06_SESSION_STATUS_H
#include <stdint.h>
struct status_id { uint32_t supervisor, group; uint64_t supervisor_ticks, group_ticks; };
enum session_status_error_code { SESSION_STATUS_NONE=0, SESSION_STATUS_CAPTURE_OVERFLOW=1 };
struct session_status { struct status_id id; unsigned active, error; };
int session_status_begin(struct session_status *, const struct status_id *);
int session_status_report(struct session_status *, const struct status_id *, unsigned);
int session_status_end(struct session_status *, const struct status_id *);
unsigned session_status_error(const struct session_status *);
#endif

#include "session-status.h"

static int valid(const struct status_id *id) {
    return id && id->supervisor>1 && id->group>1 && id->supervisor_ticks && id->group_ticks;
}
static int equal(const struct status_id *a, const struct status_id *b) {
    return a->supervisor==b->supervisor && a->group==b->group &&
           a->supervisor_ticks==b->supervisor_ticks && a->group_ticks==b->group_ticks;
}
int session_status_begin(struct session_status *s, const struct status_id *id) {
    if(!s || !valid(id) || s->active || (valid(&s->id) && equal(&s->id,id))) return 0;
    s->id=*id; s->active=1; s->error=SESSION_STATUS_NONE; return 1;
}
int session_status_report(struct session_status *s, const struct status_id *id, unsigned error) {
    if(!s || !valid(id) || !s->active || !equal(&s->id,id) || error!=SESSION_STATUS_CAPTURE_OVERFLOW) return 0;
    s->error=error; return 1;
}
int session_status_end(struct session_status *s, const struct status_id *id) {
    if(!s || !valid(id) || !s->active || !equal(&s->id,id)) return 0;
    s->active=0; return 1;
}
unsigned session_status_error(const struct session_status *s) {
    return s ? s->error : SESSION_STATUS_NONE;
}

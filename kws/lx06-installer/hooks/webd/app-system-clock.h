/* The existing webd event loop owns this continuity history. This is not a
 * calendar, configuration source, idle owner, or permission to dispatch.
 * Like webd's other shared C consumers, include the implementation so every
 * existing standalone/unified build links the same provider bytes. */
#ifndef LX06_WEBD_SYSTEM_CLOCK_H
#define LX06_WEBD_SYSTEM_CLOCK_H
#include "../../runtime/common/system-clock.c"

static struct lx06_system_clock webd_system_clock_state;

/* Serial event-loop callers only. Always resample at the point of use; a
 * previously trusted tick is never cached as permission for a later action.
 * A future scheduler still needs the original config and atomic idle owner. */
static int webd_system_clock_read(int64_t *wall_ms,uint64_t *mono_ms) {
    return lx06_system_clock_read(&webd_system_clock_state,wall_ms,mono_ms);
}

static void webd_system_clock_poll(void) {
    int64_t wall_ms;uint64_t mono_ms;
    (void)webd_system_clock_read(&wall_ms,&mono_ms);
}
#endif

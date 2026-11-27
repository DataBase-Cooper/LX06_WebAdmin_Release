/* Linux synchronized system-clock source. No setter, network or timer owner.
 * One zero-initialized state per serialized event loop; never share it across
 * processes or reset it to manufacture trust. A consumer still owns idle,
 * source identity, calendar policy and execution authority. */
#ifndef LX06_SYSTEM_CLOCK_H
#define LX06_SYSTEM_CLOCK_H
#include <stdint.h>
struct lx06_system_clock {
    uint64_t mono_ns;
    int64_t offset_low_ns,offset_high_ns;
    unsigned long tolerance_scaled;
    unsigned have_mono,baseline;
};
/* 1 synchronized continuous sample, 0 untrusted, -1 invalid arguments.
 * Every call makes read-only Linux adjtimex(modes=0) and clock_gettime calls.
 * Startup and every loss require two consecutive valid synchronized samples;
 * the first establishes a baseline and cannot authorize dispatch. TIME_OK,
 * no unsynchronized/error/leap flags, coherent bounded samples and advancing
 * monotonic time are all required. Normal frequency adjustment is bounded by
 * the kernel's scaled-ppm tolerance, never by a wall-clock date or uptime.
 * On a missing/invalid sample, wall=0 and mono retains the last valid value
 * solely so the original scheduler can revoke a candidate without inventing
 * monotonic rollback. An observed rollback is returned as such and refuses.
 * The returned values alone must never be treated as trusted when return!=1.
 */
int lx06_system_clock_read(struct lx06_system_clock *,int64_t *wall_ms,uint64_t *mono_ms);
#endif

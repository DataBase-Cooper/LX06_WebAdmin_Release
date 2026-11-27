/* Local calendar API 1. PROVISIONAL(P-20261111-0532-B/P-20261111-1343-B).
 * Asia/Shanghai only (UTC+08), 2000..2199. No libc TZ/time_t or IO.
 * A supplies parsed effective values; this is not a settings parser. */
#ifndef LX06_BR_SCHEDULE_CALENDAR_H
#define LX06_BR_SCHEDULE_CALENDAR_H
#include <stdint.h>
enum br_calendar_kind { BR_CAL_DAILY=1,BR_CAL_EVERY_N,BR_CAL_WEEKLY,BR_CAL_MONTHLY };
typedef struct {
    enum br_calendar_kind kind;
    int32_t anchor_day; /* Unix civil day, EVERY_N only */
    unsigned interval_days; /* 1..365, EVERY_N only */
    uint32_t weekdays,monthdays; /* bits ISO weekday-1 / month date-1 */
    unsigned first_minute,last_minute; /* inclusive, 0..1439; equal = fixed */
} br_calendar;
int br_calendar_day(unsigned year,unsigned month,unsigned date,int32_t *day);
int br_calendar_date(int32_t day,unsigned *year,unsigned *month,unsigned *date);
int br_calendar_valid(const br_calendar *);
int br_calendar_wall(int64_t unix_ms,int32_t *local_day);
int br_calendar_matches(const br_calendar *,int32_t day);
/* Deterministic non-security PRNG with rejection sampling. Owner supplies
 * nonzero random seed ONCE per config/task and persists it BEFORE use.
 * Same config/task/day => same minute across restart, rollback and catch-up. */
int br_calendar_when(const br_calendar *,int32_t day,uint64_t seed,int64_t *unix_ms);
/* Inclusive wall bound; direction +1 next / -1 latest. At most 367 days.
 * 1 found, 0 beyond calendar range, -1 invalid; output unchanged on failure. */
int br_calendar_find(const br_calendar *,int64_t wall,uint64_t seed,int direction,
                     int32_t *day,int64_t *unix_ms);
#endif

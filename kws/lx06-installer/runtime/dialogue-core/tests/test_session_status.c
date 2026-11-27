/* Offline contract for the isolated status state machine; not an IPC test. */
#include <stdio.h>
#include <stdlib.h>
#include "../session-status.h"
extern int session_status_begin(struct session_status *, const struct status_id *) __attribute__((weak));
extern int session_status_report(struct session_status *, const struct status_id *, unsigned) __attribute__((weak));
extern int session_status_end(struct session_status *, const struct status_id *) __attribute__((weak));
extern unsigned session_status_error(const struct session_status *) __attribute__((weak));
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    struct session_status s = {0};
    const struct status_id a={10,11,100,101}, b={10,11,100,102}, invalid={0,11,100,101};
    CHECK(session_status_begin && session_status_report && session_status_end && session_status_error);
    CHECK(!session_status_begin(&s,&invalid));
    CHECK(session_status_begin(&s,&a));
    CHECK(!session_status_begin(&s,&b));
    CHECK(session_status_error(&s)==0);
    CHECK(!session_status_report(&s,&b,1));
    CHECK(!session_status_report(&s,&a,2));
    CHECK(session_status_report(&s,&a,1));
    CHECK(session_status_error(&s)==1);
    CHECK(!session_status_begin(&s,&a));
    CHECK(session_status_error(&s)==1);
    CHECK(!session_status_end(&s,&b));
    CHECK(session_status_end(&s,&a));
    CHECK(!session_status_report(&s,&a,1));
    CHECK(session_status_error(&s)==1);
    CHECK(session_status_begin(&s,&b));
    CHECK(session_status_error(&s)==0);
    CHECK(!session_status_report(&s,&a,1));
    CHECK(session_status_error(&s)==0);
    CHECK(session_status_report(&s,&b,1));
    CHECK(session_status_error(&s)==1);
    puts("session status seam: PASS");
    return 0;
}

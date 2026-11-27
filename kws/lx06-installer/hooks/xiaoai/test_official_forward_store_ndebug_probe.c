/* Link alongside the store test with -Wl,--wrap=of_store_select.
 * A disabled assert must not turn the test into a successful no-op. */
#include <stdio.h>
#include <unistd.h>
#include "official-forward-store.h"

static unsigned select_calls;
int __real_of_store_select(int, const char *, of_store_lookup, void *);
int __wrap_of_store_select(int d, const char *s, of_store_lookup f, void *ctx) {
    ++select_calls;
    return __real_of_store_select(d, s, f, ctx);
}
__attribute__((destructor)) static void check_exercised(void) {
    if (select_calls < 300) {
        fprintf(stderr, "store test did not execute selections: %u\n", select_calls);
        _exit(2);
    }
}

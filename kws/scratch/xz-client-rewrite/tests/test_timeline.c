#include "../src/timeline.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static void test_trace_has_monotonic_sequence_and_clock(void) {
    FILE *f = tmpfile();
    assert(f != NULL);
    xz_timeline trace;
    xz_timeline_init(&trace, f);
    assert(xz_timeline_emit_at(&trace, 100, "WS_CONNECTED", "") == 1);
    assert(xz_timeline_emit_at(&trace, 110, "HELLO_OK", "rate=16000") == 2);
    fflush(f);
    rewind(f);
    char line[256];
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "seq=1 mono_ms=100 event=WS_CONNECTED") != NULL);
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "seq=2 mono_ms=110 event=HELLO_OK rate=16000") != NULL);
    fclose(f);
}

int main(void) {
    test_trace_has_monotonic_sequence_and_clock();
    FILE *f = tmpfile();
    assert(f != NULL);
    xz_timeline trace;
    xz_timeline_init(&trace, f);
    char archive_path[] = "/tmp/xz-timeline-test-XXXXXX";
    int archive_fd = mkstemp(archive_path);
    assert(archive_fd >= 0); close(archive_fd);
    trace.archive_path = archive_path;
    /* Archive plus current must retain every sequence across repeated
     * rotations between polling snapshots. A gap cannot become a false pass. */
    unsigned char seen[6001] = {0};
    for (int i = 0; i < 6000; ++i)
    {
        xz_timeline_emit_at(&trace, (uint64_t)i, "LONG_SESSION", "payload");
        if (i % 500 == 499) {
            FILE *old = fopen(archive_path, "r");
            assert(old);
            char entry[256]; unsigned long long seq;
            while (fgets(entry, sizeof(entry), old))
                if (sscanf(entry, "[TRACE] seq=%llu", &seq) == 1 && seq <= 6000) seen[seq] = 1;
            fclose(old);
            rewind(f);
            while (fgets(entry, sizeof(entry), f))
                if (sscanf(entry, "[TRACE] seq=%llu", &seq) == 1 && seq <= 6000) seen[seq] = 1;
            fseek(f, 0, SEEK_END);
        }
    }
    for (int i = 1; i <= 6000; ++i) assert(seen[i]);
    unlink(archive_path);
    assert(lseek(fileno(f), 0, SEEK_END) < 49152);
    rewind(f);
    char line[256]; int final_seen = 0;
    while (fgets(line, sizeof(line), f)) if (strstr(line, "seq=6000 ")) final_seen = 1;
    assert(final_seen);
    fclose(f);
    puts("timeline tests: PASS");
    return 0;
}

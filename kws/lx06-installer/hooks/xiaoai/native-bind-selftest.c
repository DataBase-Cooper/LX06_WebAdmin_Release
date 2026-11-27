/* ARM/host check for routing only the stock arbitration winner. */
#define _GNU_SOURCE
#define LX06_NATIVE_BIND_SELFTEST
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LX06_SESSION_OWNER_H
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
#define LX06_SESSION_STATBUF struct stat64
#else
#define LX06_SESSION_STATBUF struct stat
#endif
static int busy, preempts;
static int lx06_session_busy(void) { return busy; }
static int lx06_session_take_idle(void) { return busy ? -1 : open("/dev/null", O_RDONLY|O_CLOEXEC); }
static int lx06_session_lock_idle(int mode) { (void)mode; return lx06_session_take_idle(); }
static int lx06_session_preempt(void) { preempts++; return 1; }
static int fake_asr_active(void) { return 0; }
static void *fake_dlsym(void *handle, const char *symbol) {
    (void)handle;
    if (!strcmp(symbol, "get_asr_flag")) return fake_asr_active;
    return NULL;
}
#define dlsym fake_dlsym
#include "native-wake-bridge.c"
static unsigned candidates, stock_wins, generation;
static int got_stock_win(void *ctx, unsigned event, float angle) {
    (void)ctx; (void)event; (void)angle;
    ++stock_wins;
    return 12;
}
static void got_wake(void *ctx, unsigned event, float angle) {
    (void)ctx; (void)angle;
    if (event == 1) {
        ++candidates;
        generation = mdspeech_submission();
    }
}
static void no_dispatch(void) {
    assert(access(LX06_OFFICIAL_BIND_EVENT, F_OK) != 0);
}
static void dispatched(const char *expected) {
    char id[80];
    FILE *f = fopen(LX06_OFFICIAL_BIND_EVENT, "r");
    assert(f && fgets(id, sizeof(id), f));
    fclose(f);
    assert(!strcmp(id, expected));
    assert(unlink(LX06_OFFICIAL_BIND_EVENT) == 0);
}
int main(void) {
    callback = got_wake;
    stock_mdspeech_winner = got_stock_win;
    FILE *f = fopen(LX06_OFFICIAL_FILE, "w");
    assert(f);
    fputc('1', f);
    fclose(f);
    forward_wake(NULL, 1, 0);
    assert(candidates == 1 && mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    assert(lx06_official_bind_write_at(LX06_OFFICIAL_BIND_PATH, "doubao.sh") == 0);
    forward_wake(NULL, 1, 0);
    assert(candidates == 2 && stock_wins == 1);
    no_dispatch();
    assert(mdspeech_winner(NULL, 1, 0) == 0);
    dispatched("doubao.sh\n");
    assert(mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    forward_wake(NULL, 0x101, 0);
    assert(candidates == 2);
    no_dispatch();
    assert(lx06_official_bind_write_at(LX06_OFFICIAL_BIND_PATH, "xz.sh") == 0);
    forward_wake(NULL, 1, 0);
    no_dispatch(); /* A remote win never calls the local winner. */
    mdspeech_deadline = lx06_idle_epoch_now_ms();
    assert(mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    forward_wake(NULL, 1, 0);
    assert(mdspeech_winner(NULL, 1, 0) == 0);
    dispatched("xz.sh\n");
    forward_wake(NULL, 1, 0);
    assert(lx06_official_bind_write_at(LX06_OFFICIAL_BIND_PATH, "doubao.sh") == 0);
    assert(mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    got_wake(NULL, 1, 0); /* A local alias uses the stock callback directly. */
    assert(mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    forward_wake(NULL, 1, 0);
    mdspeech_failed(generation);
    assert(mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    forward_wake(NULL, 1, 0);
    forward_wake(NULL, 1, 0);
    assert(mdspeech_winner(NULL, 1, 0) == 12);
    no_dispatch();
    busy = 1;
    int before = preempts;
    forward_wake(NULL, 1, 0);
    assert(preempts == before + 1);
    no_dispatch();
    f = fopen(LX06_OFFICIAL_FILE, "w");
    assert(f);
    fputc('0', f);
    fclose(f);
    before = preempts;
    forward_wake(NULL, 1, 0);
    assert(preempts == before);
    no_dispatch();
    puts("native arbitration routing passed");
    return 0;
}

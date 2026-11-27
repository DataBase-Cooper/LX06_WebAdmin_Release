#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "../src/player.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>

static void get_self_path(char *out, size_t capacity) {
    ssize_t length = readlink("/proc/self/exe", out, capacity - 1);
    assert(length > 0 && (size_t)length < capacity);
    out[length] = '\0';
}

static int fake_aplay(void) {
    const char *path = getenv("XZ_PLAYER_CAPTURE");
    assert(path && *path);
    int output = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(output >= 0);
    unsigned char buffer[256];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (n == 0) break;
        assert(n > 0);
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t wrote = write(output, buffer + off, (size_t)n - off);
            assert(wrote > 0);
            off += (size_t)wrote;
        }
    }
    close(output);
    return 0;
}

static void test_player_drains_pcm_and_reports_child_exit(int pause) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    char dir[PATH_MAX], self[PATH_MAX], alias[PATH_MAX], capture[PATH_MAX];
    assert(snprintf(dir, sizeof(dir), "%s/xz-player-test-XXXXXX", tmpdir) < (int)sizeof(dir));
    assert(mkdtemp(dir) != NULL);
    get_self_path(self, sizeof(self));
    assert(snprintf(alias, sizeof(alias), "%s/aplay", dir) < (int)sizeof(alias));
    assert(symlink(self, alias) == 0);
    assert(snprintf(capture, sizeof(capture), "%s/capture.pcm", dir) < (int)sizeof(capture));
    assert(setenv("PATH", dir, 1) == 0);
    assert(setenv("XZ_PLAYER_CAPTURE", capture, 1) == 0);

    xz_player player;
    assert(xz_player_init(&player) == 0);
    assert(xz_player_start(&player, "aplay", "fake-device", 24000) == 0);
    const unsigned char pcm[] = {0, 1, 0, 2, 0, 3, 0, 4};
    assert(xz_player_write(&player, pcm, sizeof(pcm)) == 0);
    if (pause) {
        /* Wait for the real worker to emit padding during a producer gap.
         * The fake sink records PCM exactly, including the resumed samples. */
        struct stat state;
        for (int i = 0; i < 200; ++i) {
            if (!stat(capture, &state) && state.st_size >= (off_t)sizeof(pcm) + 960) break;
            struct timespec delay = {0, 10000000L};
            nanosleep(&delay, NULL);
        }
        assert(!stat(capture, &state) && state.st_size >= (off_t)sizeof(pcm) + 960);
        assert(xz_player_write(&player, pcm, sizeof(pcm)) == 0);
    } else {
        const struct timespec delay = {0, 30000000L};
        nanosleep(&delay, NULL);
    }
    assert(xz_player_finish(&player) == 0);

    int code = -1, done = 0;
    for (int i = 0; i < 200 && !done; ++i) {
        int rc = xz_player_poll(&player, &code);
        assert(rc >= 0);
        done = rc == 1;
        if (!done) {
            struct timespec delay = {0, 10000000L};
            nanosleep(&delay, NULL);
        }
    }
    assert(done && code == 0);

    FILE *file = fopen(capture, "rb");
    assert(file);
    unsigned char actual[sizeof(pcm)];
    assert(fread(actual, 1, sizeof(actual), file) == sizeof(actual));
    assert(memcmp(actual, pcm, sizeof(pcm)) == 0);
    if (pause) {
        assert(fseek(file, -(long)sizeof(pcm), SEEK_END) == 0);
        long end = ftell(file);
        long padding = end - (long)sizeof(pcm);
        assert(padding >= 960 && padding % 960 == 0);
        assert(fseek(file, sizeof(pcm), SEEK_SET) == 0);
        for (long i = 0; i < padding; ++i) assert(fgetc(file) == 0);
        assert(fread(actual, 1, sizeof(actual), file) == sizeof(actual));
    }
    assert(fgetc(file) == EOF);
    fclose(file);
    assert(memcmp(actual, pcm, sizeof(pcm)) == 0);
    xz_player_destroy(&player);

    unlink(capture);
    unlink(alias);
    rmdir(dir);
}

static void test_player_abort_stops_playback_child(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    char dir[PATH_MAX], self[PATH_MAX], alias[PATH_MAX], capture[PATH_MAX];
    assert(snprintf(dir, sizeof(dir), "%s/xz-player-abort-XXXXXX", tmpdir) < (int)sizeof(dir));
    assert(mkdtemp(dir) != NULL);
    get_self_path(self, sizeof(self));
    assert(snprintf(alias, sizeof(alias), "%s/aplay", dir) < (int)sizeof(alias));
    assert(symlink(self, alias) == 0);
    assert(snprintf(capture, sizeof(capture), "%s/capture.pcm", dir) < (int)sizeof(capture));
    assert(setenv("PATH", dir, 1) == 0);
    assert(setenv("XZ_PLAYER_CAPTURE", capture, 1) == 0);

    xz_player player;
    assert(xz_player_init(&player) == 0);
    assert(xz_player_start(&player, "aplay", "fake-device", 24000) == 0);
    const unsigned char pcm[4096] = {0};
    assert(xz_player_write(&player, pcm, sizeof(pcm)) == 0);
    assert(xz_player_abort(&player) == 0);
    int code = 0;
    assert(xz_player_poll(&player, &code) == 1);
    assert(code == 128 + SIGKILL);
    xz_player_destroy(&player);
    unlink(capture);
    unlink(alias);
    rmdir(dir);
}

int main(int argc, char **argv) {
    (void)argc;
    if (strcmp(argv[0], "aplay") == 0) return fake_aplay();
    test_player_drains_pcm_and_reports_child_exit(0);
    test_player_drains_pcm_and_reports_child_exit(1);
    test_player_abort_stops_playback_child();
    puts("player tests: PASS");
    return 0;
}

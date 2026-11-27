#ifndef XZ_PLAYER_H
#define XZ_PLAYER_H

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

/* Stock ALSA/DTS can take several seconds to open a new playback stream.
 * Keep the microphone/network loop nonblocking through that startup jitter.
 * This is bounded (~5.46 seconds at 24kHz mono), never a growing TTS buffer. */
#define XZ_PLAYER_QUEUE_CAPACITY 262144u

typedef struct {
    pid_t pid;
    int fd;
    int closing;
    int aborting;
    int done;
    int error;
    int exit_code;
    int thread_started;
    unsigned period_ms;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned char queue[XZ_PLAYER_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t used;
} xz_player;

int xz_player_init(xz_player *player);
int xz_player_start(xz_player *player, const char *program, const char *device, int sample_rate);
int xz_player_write(xz_player *player, const void *data, size_t length);
size_t xz_player_queued(xz_player *player);
int xz_player_finish(xz_player *player);
int xz_player_poll(xz_player *player, int *exit_code);
int xz_player_abort(xz_player *player);
void xz_player_destroy(xz_player *player);

#endif

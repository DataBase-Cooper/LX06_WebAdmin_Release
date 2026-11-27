#ifndef XZ_PLAYER_H
#define XZ_PLAYER_H

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>
#include "echo.h"

/* PCM only bridges the decoder and ALSA (normally about half a second).
 * Compressed backlog stays in the bounded, lazy tmpfs spool in ogg_audio.
 * Allocate on playback start and release on finish/cancel. */
#define XZ_PLAYER_QUEUE_CAPACITY (256u * 1024u)

typedef struct {
    pid_t pid;
    int fd;
    int closing;
    int aborting;
    int done;
    int error;
    int exit_code;
    int thread_started;
    int playback_started;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned char *queue;
    size_t head;
    size_t tail;
    size_t used;
    size_t prebuffer;
    db_echo *echo;
} xz_player;

int xz_player_init(xz_player *player);
int xz_player_start(xz_player *player, const char *program, const char *device,
                    int sample_rate, unsigned prebuffer_ms);
int xz_player_write(xz_player *player, const void *data, size_t length);
size_t xz_player_queued(xz_player *player);
int xz_player_playback_started(const xz_player *player);
int xz_player_finish(xz_player *player);
int xz_player_poll(xz_player *player, int *exit_code);
int xz_player_abort(xz_player *player);
void xz_player_destroy(xz_player *player);

#endif

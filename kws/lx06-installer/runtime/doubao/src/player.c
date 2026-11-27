#define _GNU_SOURCE
#include "player.h"
#include "../../../hooks/xiaoai/dialog-light.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PLAYER_CHUNK 8192u
#define ECHO_PIPE_BYTES 8192
#define ECHO_WRITE_BYTES 4096u
#define PLAYER_PREFILL_MAX_MS 1400u

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

static void sleep_ms(long ms) {
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static int wait_child_nonblocking(xz_player *player) {
    int status = 0;
    pthread_mutex_lock(&player->mutex);
    pid_t pid = player->pid;
    if (pid <= 0) {
        pthread_mutex_unlock(&player->mutex);
        return 1;
    }
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == 0) {
        pthread_mutex_unlock(&player->mutex);
        return 0;
    }
    player->pid = -1;
    if (result == pid && WIFEXITED(status)) player->exit_code = WEXITSTATUS(status);
    else if (result == pid && WIFSIGNALED(status)) player->exit_code = 128 + WTERMSIG(status);
    else player->exit_code = 255;
    if (!player->closing && !player->aborting) player->error = 1;
    if (player->error && player->exit_code == 0) player->exit_code = 255;
    player->done = 1;
    pthread_cond_broadcast(&player->cond);
    pthread_mutex_unlock(&player->mutex);
    return 1;
}

static int write_pipe_all(xz_player *player,int fd,const unsigned char *data,size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t pending = length - offset;
        if (player->echo && pending > ECHO_WRITE_BYTES) pending = ECHO_WRITE_BYTES;
        ssize_t n = write(fd, data + offset, pending);
        if (n > 0) {
            if(player->echo) {
                struct timespec now;
                if(!clock_gettime(CLOCK_MONOTONIC,&now))
                    db_echo_reference(player->echo,data+offset,(size_t)n,
                                      (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000);
            }
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void *player_worker(void *arg) {
    xz_player *player = (xz_player *)arg;
    int light_started=0;
    uint64_t first_queued=0;
    for (;;) {
        unsigned char chunk[PLAYER_CHUNK];
        size_t length = 0;
        int fd = -1;

        pthread_mutex_lock(&player->mutex);
        while (!player->closing && !player->aborting) {
            if (player->used) {
                uint64_t now=monotonic_ms();
                if (!first_queued) first_queued=now;
                /* An initial TTS packet is often followed by a stream gap.
                 * Give ALSA audio to drain before that gap arrives. */
                if (light_started || player->used>=player->prebuffer ||
                    now-first_queued>=PLAYER_PREFILL_MAX_MS) break;
            }
            /* A short first write can fit in the pipe even if ALSA immediately
             * exits. Detect that exit without requiring another audio packet
             * or a cloud TTS stop to wake this worker. Only this worker reaps. */
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 50000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&player->cond, &player->mutex, &deadline) == ETIMEDOUT) {
                pthread_mutex_unlock(&player->mutex);
                int exited = wait_child_nonblocking(player);
                pthread_mutex_lock(&player->mutex);
                if (exited) {
                    fd = player->fd;
                    player->fd = -1;
                    pthread_mutex_unlock(&player->mutex);
                    if (fd >= 0) close(fd);
                    return NULL;
                }
            }
        }
        if (player->aborting) {
            player->used = 0;
            player->tail = player->head;
            fd = player->fd;
            player->fd = -1;
            pthread_mutex_unlock(&player->mutex);
            if (fd >= 0) close(fd);
            for (int i = 0; i < 10 && !wait_child_nonblocking(player); ++i) sleep_ms(10);
            pthread_mutex_lock(&player->mutex);
            pid_t pid = player->pid;
            if (pid > 0) kill(pid, SIGKILL);
            pthread_mutex_unlock(&player->mutex);
            while (!wait_child_nonblocking(player)) sleep_ms(10);
            return NULL;
        }
        if (!player->used && player->closing) {
            fd = player->fd;
            player->fd = -1;
            pthread_mutex_unlock(&player->mutex);
            if (fd >= 0) close(fd);
            while (!wait_child_nonblocking(player)) sleep_ms(10);
            return NULL;
        }

        length = player->used;
        if (!light_started)
            printf("doubao: playback start queued_bytes=%zu sealed=%d\n",length,player->closing);
        if (length > sizeof(chunk)) length = sizeof(chunk);
        size_t contiguous = XZ_PLAYER_QUEUE_CAPACITY - player->tail;
        if (length > contiguous) length = contiguous;
        memcpy(chunk, player->queue + player->tail, length);
        player->tail = (player->tail + length) % XZ_PLAYER_QUEUE_CAPACITY;
        player->used -= length;
        fd = player->fd;
        pthread_cond_broadcast(&player->cond);
        pthread_mutex_unlock(&player->mutex);

        if (fd < 0 || write_pipe_all(player, fd, chunk, length) != 0) {
            pthread_mutex_lock(&player->mutex);
            player->error = 1;
            player->closing = 1;
            player->aborting = 1;
            player->used = 0;
            pid_t pid = player->pid;
            if (pid > 0) kill(pid, SIGTERM);
            pthread_cond_broadcast(&player->cond);
            pthread_mutex_unlock(&player->mutex);
        } else if(!light_started) {
            /* Local first PCM write, not cloud audio.done or queued metadata. */
            __atomic_store_n(&player->playback_started,1,__ATOMIC_RELEASE);
            (void)lx06_dialog_light(LX06_LIGHT_SPEAKING);
            light_started=1;
        }
    }
}

int xz_player_init(xz_player *player) {
    if (!player) return -1;
    memset(player, 0, sizeof(*player));
    player->pid = -1;
    player->fd = -1;
    if (pthread_mutex_init(&player->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&player->cond, NULL) != 0) {
        pthread_mutex_destroy(&player->mutex);
        return -1;
    }
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGPIPE, &action, NULL) != 0) {
        pthread_cond_destroy(&player->cond);
        pthread_mutex_destroy(&player->mutex);
        return -1;
    }
    return 0;
}

int xz_player_start(xz_player *player, const char *program, const char *device,
                    int sample_rate, unsigned prebuffer_ms) {
    if (!player || !program || !*program || !device || !*device ||
        (sample_rate != 16000 && sample_rate != 24000) || player->thread_started || player->done)
        return -1;

    int pipes[2];
    if (pipe(pipes) != 0) return -1;
    /* A 64 KiB pipe can hold over a second of audio that the speaker
     * has yet to play. Keep that lead within the echo reference history. */
    if (player->echo && fcntl(pipes[1], F_SETPIPE_SZ, ECHO_PIPE_BYTES) < 0)
        fputs("doubao: playback pipe sizing unavailable\n", stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipes[1]);
        if (dup2(pipes[0], STDIN_FILENO) < 0) _exit(126);
        close(pipes[0]);
        char rate[16];
        snprintf(rate, sizeof(rate), "%d", sample_rate);
        execlp(program, program, "-q", "-D", device, "-f", "S16_LE", "-c", "1",
               "-r", rate, "-t", "raw", "--buffer-size", "4800", "--period-size", "480",
               "-", (char *)NULL);
        _exit(127);
    }

    close(pipes[0]);
    pthread_mutex_lock(&player->mutex);
    player->pid = pid;
    player->fd = pipes[1];
    player->closing = 0;
    player->aborting = 0;
    player->done = 0;
    player->error = 0;
    player->exit_code = -1;
    player->prebuffer = (size_t)sample_rate * 2 * prebuffer_ms / 1000;
    player->head = player->tail = player->used = 0;
    pthread_mutex_unlock(&player->mutex);

    player->queue=malloc(XZ_PLAYER_QUEUE_CAPACITY);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,256*1024);
    int thread_error=player->queue ? pthread_create(&player->thread,&attr,player_worker,player) : ENOMEM;
    pthread_attr_destroy(&attr);
    if (thread_error != 0) {
        free(player->queue);player->queue=NULL;
        close(pipes[1]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pthread_mutex_lock(&player->mutex);
        player->pid = -1;
        player->fd = -1;
        player->done = 1;
        player->exit_code = 255;
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }
    player->thread_started = 1;
    return 0;
}

int xz_player_playback_started(const xz_player *player) {
    return __atomic_load_n(&player->playback_started,__ATOMIC_ACQUIRE);
}

int xz_player_write(xz_player *player, const void *data, size_t length) {
    if (!player || (!data && length) || length > XZ_PLAYER_QUEUE_CAPACITY) return -1;
    pthread_mutex_lock(&player->mutex);
    if (!player->thread_started || player->aborting || player->done) {
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }
    /* The prior utterance has sealed its pipe. The main loop retains the next
     * utterance until this player exits; it must not drop PCM or block the mic. */
    if (player->closing) { pthread_mutex_unlock(&player->mutex); return -2; }
    if (length > XZ_PLAYER_QUEUE_CAPACITY - player->used) {
        pthread_mutex_unlock(&player->mutex); return -3;
    }
    size_t first = XZ_PLAYER_QUEUE_CAPACITY - player->head;
    if (first > length) first = length;
    if (first) memcpy(player->queue + player->head, data, first);
    if (length > first) memcpy(player->queue, (const unsigned char *)data + first, length - first);
    player->head = (player->head + length) % XZ_PLAYER_QUEUE_CAPACITY;
    player->used += length;
    pthread_cond_signal(&player->cond);
    pthread_mutex_unlock(&player->mutex);
    return 0;
}

int xz_player_finish(xz_player *player) {
    if (!player || !player->thread_started) return -1;
    pthread_mutex_lock(&player->mutex);
    player->closing = 1;
    pthread_cond_broadcast(&player->cond);
    pthread_mutex_unlock(&player->mutex);
    return 0;
}

size_t xz_player_queued(xz_player *player) {
    pthread_mutex_lock(&player->mutex);
    size_t used=player->used;
    pthread_mutex_unlock(&player->mutex);
    return used;
}

int xz_player_poll(xz_player *player, int *exit_code) {
    if (!player || !exit_code) return -1;
    pthread_mutex_lock(&player->mutex);
    int done = player->done;
    if (done) *exit_code = player->exit_code;
    pthread_mutex_unlock(&player->mutex);
    return done ? 1 : 0;
}

int xz_player_abort(xz_player *player) {
    if (!player || !player->thread_started) return 0;
    pthread_mutex_lock(&player->mutex);
    if (!player->done) {
        player->aborting = 1;
        player->closing = 1;
        player->used = 0;
        player->tail = player->head;
        pthread_cond_broadcast(&player->cond);
        /* Abort is used for explicit stops. Finish still drains normal
         * playback. Do not let aplay drain or wait on a full PCM pipe here. */
        if (player->pid > 0) kill(player->pid, SIGKILL);
    }
    pthread_mutex_unlock(&player->mutex);

    pthread_join(player->thread, NULL);
    player->thread_started = 0;
    return 0;
}

void xz_player_destroy(xz_player *player) {
    if (!player) return;
    if (player->thread_started) {
        int code = -1;
        if (xz_player_poll(player, &code) == 0) xz_player_abort(player);
        else {
            pthread_join(player->thread, NULL);
            player->thread_started = 0;
        }
    }
    pthread_cond_destroy(&player->cond);
    pthread_mutex_destroy(&player->mutex);
    free(player->queue);
    memset(player, 0, sizeof(*player));
}

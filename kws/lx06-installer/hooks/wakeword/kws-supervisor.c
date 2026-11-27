/* Isolated KWS pipeline supervisor; see docs/kws-supervisor-offline.md. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void on_stop(int sig) { (void)sig; stopping = 1; }

static pid_t spawn(char **argv, int source, int sink, int fds[2]) {
    pid_t pid = fork();
    if (pid != 0) return pid;
    if ((source >= 0 && dup2(source, STDIN_FILENO) < 0) ||
        (sink >= 0 && dup2(sink, STDOUT_FILENO) < 0)) _exit(126);
    if (source < 0) {
        close(STDIN_FILENO);
    }
    close(fds[0]); close(fds[1]);
    signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
    execvp(argv[0], argv);
    _exit(127);
}

int main(int argc, char **argv) {
    int i, fds[2], status, fault = 0, requested = 0, draining = 0;
    struct timespec drain_start;
    pid_t children[2] = {-1, -1};
    struct sigaction sa;
    struct pollfd control = { STDIN_FILENO, POLLIN | POLLHUP, 0 };
    if (argc == 2 && !strcmp(argv[1], "--epoch-drain-supported")) return 0;
    for (i = 1; i < argc && strcmp(argv[i], "--"); ++i) {}
    if (i <= 1 || i >= argc - 1) {
        fprintf(stderr, "usage: kws-supervisor reader [args...] -- detector [args...]\n");
        return 2;
    }
    argv[i] = NULL;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_stop; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL)) return 2;
    /* Reset inherited SIG_IGN/SA_NOCLDWAIT: we alone retain and reap children. */
    sa.sa_handler = SIG_DFL;
    if (sigaction(SIGCHLD, &sa, NULL) || pipe(fds)) return 2;
    children[0] = spawn(&argv[1], -1, fds[1], fds);
    if (children[0] > 0) children[1] = spawn(&argv[i+1], fds[0], -1, fds);
    close(fds[0]); close(fds[1]);
    if (children[0] < 0 || children[1] < 0) fault = 1;
    while (!fault && !requested) {
        int n;
        for (int k = 0; k < 2; ++k) {
            if (children[k] <= 0) continue;
            pid_t r = waitpid(children[k], &status, WNOHANG | WUNTRACED);
            if (r == children[k]) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) children[k] = -1;
                if (!draining || WIFSTOPPED(status)) fault = 1;
                else if (k == 1 && !WIFEXITED(status)) fault = 1;
            } else if (r < 0 && errno != EINTR) { children[k] = -1; fault = 1; }
        }
        if (draining && !fault) {
            if (children[0] < 0 && children[1] < 0) {
                puts("KWS_DRAINED"); fflush(stdout); break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec-drain_start.tv_sec)*1000L +
                           (now.tv_nsec-drain_start.tv_nsec)/1000000L;
            if (draining == 1 && elapsed >= 350) {
                /* A slow forensic write may finish after dispatch. Preserve
                 * its transaction and the caller's original event deadline. */
                puts("KWS_DRAIN_PENDING"); fflush(stdout); draining = 2;
            }
        }
        if (stopping) requested = 1;
        if (fault || requested) break;
        n = poll(&control, 1, 50);
        if (n < 0 && errno != EINTR) { fault = 1; break; }
        if (n > 0 && control.revents) {
            char c;
            ssize_t count = read(STDIN_FILENO, &c, 1);
            if (count == 0 || (count == 1 && c == 'Q')) requested = 1;
            else if (count == 1 && c == 'E' && !draining) {
                /* Closing the writer delivers EOF to the detector. Its normal
                 * exit path flushes the scored audio and decision record. */
                kill(children[0], SIGKILL);
                clock_gettime(CLOCK_MONOTONIC, &drain_start);
                draining = 1;
            }
            else if (count < 0 && errno != EINTR) fault = 1;
        }
    }
    /* Only direct, not-yet-reaped children are signalled: no PID reuse window. */
    for (int k = 0; k < 2; ++k) if (children[k] > 0) kill(children[k], SIGKILL);
    for (int k = 0; k < 2; ++k) if (children[k] > 0) {
        while (waitpid(children[k], &status, 0) < 0 && errno == EINTR) {}
    }
    return fault ? 1 : 0;
}

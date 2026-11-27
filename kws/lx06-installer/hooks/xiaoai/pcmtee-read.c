/* pcmtee-read.c — raw s16 dgram reader for pcmtee fanout (taps/<name>.usock)
 * usage: pcmtee-read --name N -d SECS > out.s16   (byte rate ≈32k => mono 16k live)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv)
{
    const char *name = "cap"; int dur = 10;
    int i;
    for (i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--name")) name = argv[i + 1];
        else if (!strcmp(argv[i], "-d")) dur = atoi(argv[i + 1]);
    }
    char path[128];
    snprintf(path, sizeof(path), "/tmp/xiaoai-agent/taps/%s.usock", name);
    unlink(path);
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (bind(s, (struct sockaddr *)&a, (socklen_t)(strlen(a.sun_path) + offsetof(struct sockaddr_un, sun_path) + 1)) < 0) {
        perror("bind"); return 1;
    }
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    unsigned long total = 0;
    time_t t0 = time(NULL);
    while ((int)(time(NULL) - t0) < dur) {
        char buf[16384];
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n > 0) { total += (unsigned long)n; fwrite(buf, 1, (size_t)n, stdout); fflush(stdout); }
    }
    unlink(path);
    fprintf(stderr, "PCMTEE-READ total=%lu bytes (%.0f B/s)\n", total, (double)total / (double)dur);
    return 0;
}

/* LX06 tapbind.so · mico_aivs_lab 的 LD_PRELOAD（门控 env XAOAI_LAB2=1）· armhf
 *
 * 双路由（V28）：xaudio-tap 占用原生路径 /tmp/mico_aivs_lab/usock/speech.usock
 * （fan-out + 路由），真实引擎必须改绑 LAB2 路径才能同时存活。本 preload 在
 * lab 进程 bind(原生 speech 路径) 时改写为 /tmp/mico_aivs_lab2/usock/speech.usock：
 *   - 只改写该精确路径（common.usock 等其他 bind 不受影响）；
 *   - 改绑前 unlink 旧文件（防上一周期残留 EADDRINUSE）；
 *   - 每次改绑追加一行 /tmp/xiaoai-agent/lab2.trace（验活证据）；
 *   - 未设 XAOAI_LAB2 时零行为（官方模式不受影响）。
 *
 * 由 start.sh 打补丁到 /etc/init.d/mico_aivs_lab（bind mount 覆盖）：
 *   procd_set_param env XAOAI_LAB2=1
 *   procd_set_param env LD_PRELOAD=/usr/lib/lx06/xiaoai/tapbind.so
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define FROM "/tmp/mico_aivs_lab/usock/speech.usock"
#define TO   "/tmp/mico_aivs_lab2/usock/speech.usock"

static int (*real_bind)(int, const struct sockaddr *, socklen_t);

__attribute__((constructor))
static void tapbind_init(void)
{
    real_bind = (int (*)(int, const struct sockaddr *, socklen_t))
        dlsym(RTLD_NEXT, "bind");
}

int bind(int fd, const struct sockaddr *a, socklen_t l)
{
    if (real_bind && a && a->sa_family == AF_UNIX &&
        l >= (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1) &&
        strncmp(((const struct sockaddr_un *)a)->sun_path, FROM, sizeof(FROM) - 1) == 0 &&
        getenv("XAOAI_LAB2")) {
        struct sockaddr_un u;
        memset(&u, 0, sizeof(u));
        u.sun_family = AF_UNIX;
        strncpy(u.sun_path, TO, sizeof(u.sun_path) - 1);
        unlink(TO);
        {
            FILE *tr = fopen("/tmp/xiaoai-agent/lab2.trace", "a");
            if (tr) { fprintf(tr, "bind %s -> %s\n", FROM, TO); fclose(tr); }
        }
        return real_bind(fd, (const struct sockaddr *)&u, (socklen_t)sizeof(u));
    }
    if (!real_bind) { errno = ENOSYS; return -1; }
    return real_bind(fd, a, l);
}

/* tapbind-mipns2.so · mipns-xiaomi（pns 服务）的 LD_PRELOAD（门控 env XAOAI_MIPNS2=1）· armhf
 *
 * V44 音频数据面 = 引擎绑定的 /tmp/mipns/usock/speech.usock（客户端向其发请求，
 * 引擎回包含麦克风帧）。tap 需占用该原生路径，真引擎改绑 /tmp/mipns2/usock/speech.usock。
 * 本 preload 在 mipns 进程 bind(原生路径) 时改写为 mipns2：
 *   - 只改写该精确路径；其余 bind 不受影响；
 *   - 改绑前 unlink 旧文件（防残留 EADDRINUSE）；
 *   - 每次改绑追加 /tmp/xiaoai-agent/mipns2.trace（验活证据）；
 *   - 未设 XAOAI_MIPNS2 时零行为。
 *
 * 由 bind-mount 覆盖的 /etc/init.d/pns 注入：
 *   procd_set_param env XAOAI_MIPNS2=1
 *   procd_set_param env LD_PRELOAD=/tmp/xt/tapbind-mipns2.so
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

#define FROM "/tmp/mipns/usock/speech.usock"
#define TO   "/tmp/mipns2/usock/speech.usock"

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
        getenv("XAOAI_MIPNS2")) {
        struct sockaddr_un u;
        memset(&u, 0, sizeof(u));
        u.sun_family = AF_UNIX;
        strncpy(u.sun_path, TO, sizeof(u.sun_path) - 1);
        unlink(TO);
        {
            FILE *tr = fopen("/tmp/xiaoai-agent/mipns2.trace", "a");
            if (tr) { fprintf(tr, "bind %s -> %s\n", FROM, TO); fclose(tr); }
        }
        return real_bind(fd, (const struct sockaddr *)&u, (socklen_t)sizeof(u));
    }
    if (!real_bind) { errno = ENOSYS; return -1; }
    return real_bind(fd, a, l);
}

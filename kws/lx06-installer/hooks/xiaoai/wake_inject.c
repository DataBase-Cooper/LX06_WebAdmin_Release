/* wake_inject.c — LD_PRELOAD shim for mipns-xiaomi (LX06 V42.3 experiment)
 * 拦截 clock_gettime（引擎主循环高频调用），发现 /tmp/lx06-inject-wake 触发文件
 * 就在 pns 进程内调用 libxaudio_engine 的 set_wakeup_status()，模拟官方唤醒后置位。
 * 触发文件由 webd/测试脚本创建，调用后立即删除（一次性）。
 */
#define _GNU_SOURCE
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <syslog.h>

#define TRIGGER "/tmp/lx06-inject-wake"
#define CHECK_INTERVAL_S 0.05

static int (*real_ck)(clockid_t, struct timespec *);
static double last_chk = 0.0;
static int inited = 0;

static void do_inject(void)
{
    void (*fn)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "set_wakeup_status");
    if (!fn) {
        syslog(3, "wakeinject: [E] set_wakeup_status not found\n");
        return;
    }
    syslog(3, "wakeinject: trigger -> call set_wakeup_status()\n");
    fn();
    syslog(3, "wakeinject: set_wakeup_status() done\n");
}

int clock_gettime(clockid_t clk, struct timespec *tp)
{
    if (!real_ck) {
        real_ck = (int (*)(clockid_t, struct timespec *))dlsym(RTLD_NEXT, "clock_gettime");
        if (!real_ck) return -1;
    }
    int r = real_ck(clk, tp);
    if (r == 0 && tp) {
        double now = (double)tp->tv_sec + (double)tp->tv_nsec / 1e9;
        if (!inited || (now - last_chk) > CHECK_INTERVAL_S) {
            last_chk = now;
            inited = 1;
            struct stat st;
            if (stat(TRIGGER, &st) == 0) {
                unlink(TRIGGER);
                do_inject();
            }
        }
    }
    return r;
}

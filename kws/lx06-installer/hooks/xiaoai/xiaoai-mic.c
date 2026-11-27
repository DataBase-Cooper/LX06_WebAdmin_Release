/* LX06 xiaoai-mic V29 —— 单路由麦克风枢纽（整体重写）
 * armhf 静态 · root · 取代旧 xiaoai-mic + xaudio-tap + vpm-feed 三进程
 *
 * ============================ 架构 ============================
 * 麦硬件
 *   │  原生 AIVS lab 全链：降噪 / 回声消除(AEC) / 波束成形 / VAD /
 *   │  播放时录音 / 全双工连续对话（本程序不碰 ALSA、不加载 libvpm，
 *   │  全部复用固件原生能力）
 *   ▼
 * mipns-xiaomi（常驻：官方 KWS「小爱同学」+ 常驻唤醒；麦路径唯一 owner；
 *   pcm16 流，baked pns 已去 -r opus32）
 *   │  原生 speech 协议（UP datagram，protobuf 风格）
 *   ▼
 * /tmp/mico_aivs_lab/usock/speech.usock（原生路径，本程序独占绑定）
 *   │
 *   ▼
 * xiaoai-mic（唯一麦克风程序 = 本进程）
 *   │
 *   ├─ ① 单路由·永远官方：所有 UP datagram 原样透传（不解析/不攒包/
 *   │    零分配/零子进程）→ /tmp/mico_aivs_lab2/usock/speech.usock
 *   │    （LAB2：mico 官方引擎，baked tapbind.so 改绑，官方「自己的」
 *   │    speech 路径）→ 小米官方云 ASR/NLP → 回复 TTS
 *   │    （common.usock 注册/TTS 通道由 mico 原生持有，不变）。
 *   │    mico 的 DOWN datagram 原样透传回 mipns —— 对话轮次/VAD/LED/
 *   │    DIALOG_FINISH/中途打断 全部官方引擎原生处理，零模拟。
 *   │
 *   ├─ ② 扇出：所有 UP datagram + ③ 的会话 PCM → /tmp/xiaoai-agent/taps/ 下各 xxx.usock
 *   │    （kws=本地唤醒词 / rec=录音 / xz=小智 / db=豆包；任何脚本
 *   │    bind 自己的 taps/xxx.usock 即得经原生 DSP 的 16k PCM）
 *   │    消费端 = xaudio-tap-read（不变）。
 *   │
 *   ├─ ③ voip 会话保持（原 vpm-feed）：会话 = 麦 DSP 电源。无会话时
 *   │    mipns 不出流（实测 mico 健康+无会话 → tap 恒 0B）。本程序
 *   │    attach pns 真实 fifo fd（/proc/<pns>/fd/N，同 inode）+ event 5
 *   │    保持会话；idle 连续 DSP 音频按 UP TRANSMITTING datagram 打包
 *   │    只送 ② 扇出（不送 LAB2：官方引擎的 uplink 走原生 UP 路径）。
 *   │    dry 2s → 先 re-attach（免事件风暴）；attach 不到 → 5s 冷却后
 *   │    整段重开（7/8/2/5）。无外部 voip_guard（旧 190s 周期根除）。
 *   │
 *   ├─ ④ kws_arm：官方「小爱同学」开关（/data/wakeword/official_xiaoai，
 *   │    缺省=开，仅写 0 才关）→ DOWN type 9(arm)/8(disarm) 注入 mipns。
 *   │    每秒检查，仅边沿变化时发一个 datagram（raw sendto，无子进程）。
 *   │
 *   └─ ⑤ DB_Noplay：/tmp/xiaoai-agent/db_noplay（mtime 120s TTL，门控等
 *       脚本 touch）。ON → voip event 7（官方回复 TTS 流静音）+
 *       official-reply.mute（official-asr 不代答）；OFF → event 8。
 *       边沿触发；ubus 走 vfork 异步不等待（热路径零阻塞）。
 *
 * ===================== 低延迟保证（=官方直连） =====================
 * 官方 uplink 热路径 = poll 唤醒 → recvfrom → sendto(LAB2) 单 datagram
 * 进单 datagram 出，亚毫秒级转发，不解析/不攒包/不阻塞。解析（扇出打包）
 * 只服务 ② 且发生在官方转发之后；一切慢操作（ubus/sh/mkfifo/sleep）
 * 只在冷路径（会话重开、DB_Noplay 边沿、心跳）。相对官方直连（mico
 * 直接 bind 原生路径）多一跳 dgram 内核拷贝 + 一次进程切换 ≈ <1ms，
 * 而官方对话时延由云 ASR/NLP/TTS（数百 ms 级）主导 → 体感一致。
 *
 * ================= "小爱官方麦克风静音" =================
 * mico（官方麦消费端）不独占麦路径：baked 改绑 LAB2，只收本程序
 * 转发的完整流（单路由 100% 透传，官方云永远听得见用户）→ 不与
 * 本程序抢麦、无双重消费；同时废除旧双路由（route 文件 / agent.usock
 * 本地路由），麦路径上只有一个 speech 入口。
 *
 * ============ glibc 4.9 内核 syscall 陷阱（与 keeper 同源） ============
 * mico 4.9.61（arm64 内核跑 ARM32）无 64 位 time 系（403=clock_gettime64、
 * 422=futex_time64、435=clone3、436=close_range）/ statx(397) / rseq(398)
 * → do_ni_syscall 每次打一份完整寄存器 dump（oops 风暴）。静态 glibc ≥2.36
 * 的 INLINE_SYSCALL 经 iplt 蹦床 __libc_do_syscall：同名遮蔽该符号（静态
 * 链接本对象先于 libc.a），命中白名单返回 -ENOSYS 不进内核（glibc 按设计
 * 走旧号回退：403→263、397→fstatat64、435→clone、436→逐个 close），
 * 其余原样透传。不能一刀切 ≥386（386-396 内核有实现且 glibc TLS 依赖）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>

#if defined(__arm__)
__attribute__((naked, used))
long __libc_do_syscall(long n, long a0, long a1, long a2,
                       long a3, long a4, long a5) {
    (void)n; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    __asm__ __volatile__ (
        "push {r7, fp, ip, lr}\n"
        "mov fp, r12\n"        /* 先存 syscall 号：ip(r12) 马上要当 scratch 用 */
        "movw ip, #397\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #398\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #403\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #422\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #435\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "movw ip, #436\n"
        "cmp fp, ip\n"
        "beq 1f\n"
        "mov r7, fp\n"
        "svc #0\n"
        "pop {r7, fp, ip, pc}\n"
        "1:\n"
        "mvn r0, #37\n"       /* r0 = ~37 = -ENOSYS。glibc 见 r0<0 置
                                 errno=38 返回 -1，走旧号回退；返回 +38
                                 会被当成功踩空指针 */
        "pop {r7, fp, ip, pc}\n"
        : : : "memory");
}
#else
__attribute__((used))
long __libc_do_syscall(long n, long a0, long a1, long a2,
                       long a3, long a4, long a5) {
    return (long)syscall(n, a0, a1, a2, a3, a4, a5);
}
#endif

/* ---------- 常量 ---------- */
#define MAXD   8192      /* dgram 缓冲（>8192 的 dgram recv 报 EMSGSIZE；
                           与消费端 xaudio-tap-read 一致，超限即丢该包） */
#define FRAME  960       /* 480 样本 = 30ms，16k S16 mono（vpm-feed 同款） */
#define FIFO   "/tmp/mico-voip-audio.fifo"
#define TAPDIR "/tmp/xiaoai-agent/taps"
#define NOPLAY   "/tmp/xiaoai-agent/db_noplay"
#define MUTEFILE "/tmp/xiaoai-agent/official-reply.mute"
#define LOG_MAX  131072  /* 日志 128KB 滚动 */

enum { D_DIS_WAKEUP = 8, D_EN_WAKEUP = 9 };   /* 下行 KWS arm/disarm */

static const char *listen_path = "/tmp/mico_aivs_lab/usock/speech.usock";
static const char *lab_path    = "/tmp/mico_aivs_lab2/usock/speech.usock";
static const char *tapdir      = TAPDIR;
static const char *client_path = "/tmp/xiaoai-agent/mic2lab.usock";
static const char *g_log       = "/data/open-xiaoai/mic.log";

static volatile sig_atomic_t g_stop;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

/* ---------- 时间 ---------- */
static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* 墙钟 ms（CLOCK_REALTIME，syscall 79 gettimeofday 32 位）：与文件 mtime
 * 同一时间基（noplay_check 用 MONOTONIC 会恒为大负数）。 */
static int64_t wall_ms(void)
{
    struct timeval tv;
    if (syscall(79, &tv, (void *)0) != 0)
        return 0;
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------- 日志（stderr + 文件滚动） ---------- */
static void logmsg(const char *fmt, ...)
{
    char line[512], ts[32];
    va_list ap;
    time_t w = time(NULL);
    struct tm tmv;
    localtime_r(&w, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[xiaoai-mic %s] %s\n", ts, line);
    static FILE *lg = NULL;
    if (!lg) {
        if (access(g_log, F_OK) == 0) {
            FILE *t = fopen(g_log, "r+");
            if (t) {
                fseek(t, 0, SEEK_END);
                if (ftell(t) > LOG_MAX) {
                    fclose(t);
                    char l1[256];
                    snprintf(l1, sizeof(l1), "%s.1", g_log);
                    rename(g_log, l1);
                } else {
                    fclose(t);
                }
            }
        }
        lg = fopen(g_log, "a");
    }
    if (lg) {
        fprintf(lg, "[%s] %s\n", ts, line);
        fflush(lg);
    }
}

/* ---------- 子进程（vfork(156)+execve；ni-trap 下 fork/system 亦安全，
 * 但 vfork 更省：无 clone，无页表复制） ---------- */
/* 同步（冷路径：需要顺序/结果时，如会话事件序列） */
static void run_cmd(const char *cmd)
{
    long pid = syscall(156); /* vfork */
    if (pid == 0) {
        char *argv[] = { "/bin/sh", "-c", (char *)cmd, 0 };
        char *envp[] = { 0 };
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    if (pid > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
}
/* 异步（边沿事件：fire-and-forget，主循环不等待 → 热路径零阻塞） */
static void run_cmd_async(const char *cmd)
{
    long pid = syscall(156);
    if (pid == 0) {
        char *argv[] = { "/bin/sh", "-c", (char *)cmd, 0 };
        char *envp[] = { 0 };
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    /* pid > 0：不等；zombie 由 init 收 */
}

/* ---------- protobuf 风格编码（与消费端 xaudio-tap-read 对称） ---------- */
static void enc_varint(uint8_t *b, int *o, unsigned long long v)
{
    while (v >= 0x80) { b[(*o)++] = (uint8_t)(v | 0x80); v >>= 7; }
    b[(*o)++] = (uint8_t)v;
}
static void enc_fv(uint8_t *b, int *o, int num, unsigned long long v)
{
    enc_varint(b, o, (unsigned long long)num << 3);
    enc_varint(b, o, v);
}
static void enc_fb(uint8_t *b, int *o, int num, const uint8_t *d, size_t l)
{
    enc_varint(b, o, ((unsigned long long)num << 3) | 2);
    enc_varint(b, o, l);
    memcpy(b + *o, d, l);
    *o += (int)l;
}

/* 会话 PCM → UP TRANSMITTING datagram（vpm-feed 同款字节布局：
 * 顶层 field1=0(UPWARD) field2=body；body field1=3(TRANSMITTING)
 * field4=data；data field1=0(pcm16) field2=pcm） */
static size_t pack_pcm(uint8_t *out, const uint8_t *pcm, size_t pcm_n)
{
    uint8_t data[16 + FRAME];
    uint8_t body[32 + FRAME];
    size_t dn = 0, bn = 0, on = 0;
    data[dn++] = 0x08; data[dn++] = 0x00;
    data[dn++] = 0x12;
    dn += enc_varint(data + dn, (unsigned)pcm_n);
    memcpy(data + dn, pcm, pcm_n);
    dn += pcm_n;

    body[bn++] = 0x08; body[bn++] = 0x03;
    body[bn++] = 0x22;
    bn += enc_varint(body + bn, (unsigned)dn);
    memcpy(body + bn, data, dn);
    bn += dn;

    out[on++] = 0x08; out[on++] = 0x00;
    out[on++] = 0x12;
    on += enc_varint(out + on, (unsigned)bn);
    memcpy(out + on, body, bn);
    return on + bn;
}

/* ---------- pns voip 事件（pnshelper event_notify，src=3） ----------
 * 2=会话准备 5=会话开始(flag:1) 7=回复静音 8=静音释放/会话结束相关。
 * system() 同步（冷路径专用：整段重开的 7/8/2/5 必须保序）。 */
static int pns_event(int ev)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd),
             "ubus -t 5 call pnshelper event_notify '{\"src\":3,\"event\":%d}' >/dev/null 2>&1",
             ev);
    return system(cmd) == 0 ? 0 : -1;
}

/* ---------- taps 扇出（connect 的 dgram socket；离线丢包下轮重连） ---------- */
static int taps[16];
static int n_taps;

static void taps_close(void)
{
    int i;
    for (i = 0; i < n_taps; i++) close(taps[i]);
    n_taps = 0;
}
static void taps_refresh(void)
{
    DIR *d;
    struct dirent *de;
    taps_close();
    d = opendir(tapdir);
    if (!d) return;
    while ((de = readdir(d)) != NULL && n_taps < 16) {
        size_t l = strlen(de->d_name);
        struct sockaddr_un a;
        int fd;
        if (l < 7 || strcmp(de->d_name + l - 6, ".usock") != 0) continue;
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd < 0) continue;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s/%s", tapdir, de->d_name);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
            close(fd);
            continue;
        }
        taps[n_taps++] = fd;
    }
    closedir(d);
}
static void taps_fanout(const uint8_t *pkt, size_t n)
{
    int i;
    for (i = 0; i < n_taps; i++)
        (void)send(taps[i], pkt, n, MSG_NOSIGNAL);
}

/* ---------- 目录/绑定 ---------- */
static void mkdir_parents(const char *path)
{
    char dir[120];
    char *slash;
    size_t n;
    slash = strrchr(path, '/');
    if (!slash || slash == path) return;
    n = (size_t)(slash - path);
    if (n >= sizeof(dir)) return;
    memcpy(dir, path, n);
    dir[n] = 0;
    slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        mkdir(dir, 0755);
        *slash = '/';
    }
    mkdir(dir, 0755);
}

/* retry bind：lazy umount 残留 mount point / 上一轮未退干净的旧实例 →
 * unlink 成功但 bind EADDRINUSE（V20261024.1 实测）→ 重试 5×1.5s。 */
static int bind_path(const char *path)
{
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return -1; }
    mkdir_parents(path);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    int ok = -1;
    for (int i = 0; i < 5; i++) {
        unlink(path);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0) { ok = 0; break; }
        if (errno != EADDRINUSE) break;
        usleep(1500000);
    }
    if (ok < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    return s;
}

/* ---------- voip 会话保持（原 vpm-feed 逐字移植） ----------
 * 会话 = xaudio 麦 DSP 电源：voip 会话在 → 音频出流 → tap(rec/kws) 有
 * 数据 → 本地 KWS/录音才听得见（实测 mico 健康+无会话时 tap 恒 0B）。
 * 官方 NLP 与 flag:1 的竞争由 pns 在官方唤醒启动时自行清 flag 解决
 * （实测 5/5 有回复，首包至多被拒一次后恢复）。 */

/* pns 重启后仍写它启动时 open 的旧 fifo（unlink 后显示 deleted）→
 * 直接打开 /proc/<pns>/fd/N（同一 inode）即可接上真实流，免事件风暴。 */
static int attach_mipns_fifo(void)
{
    struct dirent *de;
    DIR *d;
    char proc[64];
    int pid = 0;
    DIR *pd = opendir("/proc");
    struct dirent *pe;
    if (pd) {
        while ((pe = readdir(pd)) != NULL) {
            char cpath[64], buf[80];
            int cfd, n;
            if (pe->d_name[0] < '1' || pe->d_name[0] > '9') continue;
            snprintf(cpath, sizeof(cpath), "/proc/%s/cmdline", pe->d_name);
            cfd = open(cpath, O_RDONLY);
            if (cfd < 0) continue;
            n = (int)read(cfd, buf, sizeof(buf) - 1);
            close(cfd);
            if (n <= 0) continue;
            buf[n] = 0;
            if (strstr(buf, "mipns-xiaomi")) { pid = atoi(pe->d_name); break; }
        }
        closedir(pd);
    }
    if (pid <= 0) return -1;
    snprintf(proc, sizeof(proc), "/proc/%d/fd", pid);
    d = opendir(proc);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char path[96], link[160];
        ssize_t n;
        int fd;
        if (de->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/%d/fd/%s", pid, de->d_name);
        n = readlink(path, link, sizeof(link) - 1);
        if (n < 0) continue;
        link[n] = 0;
        if (!strstr(link, "mico-voip-audio.fifo")) continue;
        fd = open(path, O_RDONLY | O_NONBLOCK);
        closedir(d);
        if (fd >= 0) {
            pns_event(5);
            return fd;
        }
        return -1;
    }
    closedir(d);
    return -1;
}

/* 整段重开：7/8 放开旧 fifo，新建，再 2 和 5（约 0.5s 阻塞，冷路径）。 */
static int voip_session_full(void)
{
    int fd;
    pns_event(7);
    pns_event(8);
    unlink(FIFO);
    if (mkfifo(FIFO, 0666) != 0 && errno != EEXIST) return -1;
    if (pns_event(2) != 0) return -1;
    usleep(300000);
    fd = open(FIFO, O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    usleep(200000);
    if (pns_event(5) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 优先 attach pns 正在写的 fifo fd（pns 先于本程序启动时唯一正确姿势；
 * 直接整段重开的 unlink+mkfifo 会让 pns 继续写已删除 inode → 干等） */
static int open_stream(void)
{
    int fd = attach_mipns_fifo();
    if (fd >= 0) return fd;
    return voip_session_full();
}

/* ---------- 官方「小爱同学」开关（缺省=开，仅写 0 才关） ---------- */
static int official_xiaoai_on(void)
{
    char c[8] = {0};
    int f = open("/data/wakeword/official_xiaoai", O_RDONLY);
    int n;
    if (f < 0) return 1;
    n = (int)read(f, c, sizeof(c) - 1);
    close(f);
    if (n <= 0) return 1;
    while (n > 0 && (c[n - 1] == '\n' || c[n - 1] == '\r' || c[n - 1] == ' ')) n--;
    c[n] = 0;
    return strcmp(c, "0") != 0;
}

/* ---------- KWS arm 注入（raw sendto，无子进程；边沿触发） ---------- */
static int g_listen = -1;
static int have_mipns = 0;
static struct sockaddr_un mipns_addr;
static char mipns_path[108];
static int g_kws_arm = -1;

static void kws_arm_inject(void)
{
    uint8_t inner[16], outer[32];
    int io = 0, oo = 0, on, type;
    if (!have_mipns || g_listen < 0) return;
    on = official_xiaoai_on();
    if (g_kws_arm == on) return;
    type = on ? D_EN_WAKEUP : D_DIS_WAKEUP;
    enc_fv(inner, &io, 1, (unsigned long long)type);
    enc_fv(outer, &oo, 1, 1);                 /* DOWNWARD */
    enc_fb(outer, &oo, 3, inner, (size_t)io); /* body */
    if (sendto(g_listen, outer, (size_t)oo, MSG_NOSIGNAL,
               (const struct sockaddr *)&mipns_addr, sizeof(mipns_addr)) < 0) {
        logmsg("kws_arm send type=%d: %s", type, strerror(errno));
        return;
    }
    g_kws_arm = on;
    logmsg("kws_arm=%d type=%d", on, type);
}

/* ---------- DB_Noplay（边沿触发；ubus 异步不等待） ---------- */
static int g_noplay = 0;

static void noplay_set(int on)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "/bin/ubus -t 5 call pnshelper event_notify "
             "'{\"src\":3,\"event\":%d}' >/dev/null 2>&1", on ? 7 : 8);
    run_cmd_async(cmd);
    if (on) {
        int f = open(MUTEFILE, O_WRONLY | O_CREAT, 0644);
        if (f >= 0) close(f);
    } else {
        unlink(MUTEFILE);
    }
    g_noplay = on;
    logmsg("DB_Noplay %s (voip event %d)", on ? "ON" : "OFF", on ? 7 : 8);
}

static void noplay_check(void)
{
    struct stat st;
    int on = 0;
    if (stat(NOPLAY, &st) == 0) {
        long age = (long)(wall_ms() / 1000) - (long)st.st_mtime;
        on = (age >= 0 && age < 120);
    }
    if (on != g_noplay)
        noplay_set(on);
}

/* ---------- 系统时钟同步（首次启动） ---------- */
static void sync_sys_clock(void)
{
    char buf[64];
    unsigned sec;
    struct { int tv_sec; int tv_usec; } tv;
    int pfd[2];
    ssize_t n;
    if (pipe(pfd) != 0) return;
    long pid = syscall(156);
    if (pid == 0) {
        dup2(pfd[1], 1);
        close(pfd[0]);
        close(pfd[1]);
        char *argv[] = { "/bin/sh", "-c", "/bin/date +%s 2>/dev/null", 0 };
        char *envp[] = { 0 };
        execve("/bin/sh", argv, envp);
        _exit(127);
    }
    close(pfd[1]);
    n = (pid > 0) ? read(pfd[0], buf, sizeof(buf) - 1) : -1;
    if (pid > 0) {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    close(pfd[0]);
    if (n <= 0) return;
    buf[n] = 0;
    sec = (unsigned)strtoul(buf, NULL, 10);
    if (sec < 1000000000u) return;
    tv.tv_sec = (int)sec;
    tv.tv_usec = 0;
    syscall(79, &tv, 0);
}

/* ---------- 主 ---------- */
int main(int argc, char **argv)
{
    int i, ls, lg;
    struct sockaddr_un lab_addr;
    int fifo_fd = -1;
    uint8_t fifo_acc[8192];
    size_t fifo_got = 0;
    int64_t last_byte = 0, last_reopen = 0;
    unsigned long up_pkts = 0, down_pkts = 0, feed_pkts = 0;
    int64_t last_tick = 0, last_hb = 0, g_start_ms;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--listen"))      listen_path = argv[i + 1];
        else if (!strcmp(argv[i], "--lab"))    lab_path    = argv[i + 1];
        else if (!strcmp(argv[i], "--tapdir")) tapdir      = argv[i + 1];
        else if (!strcmp(argv[i], "--clientbind")) client_path = argv[i + 1];
        else if (!strcmp(argv[i], "--log"))    g_log       = argv[i + 1];
        else if (!strcmp(argv[i], "--labpath"))    lab_path    = argv[i + 1]; /* 兼容 */
        else if (!strcmp(argv[i], "--labclient"))  client_path = argv[i + 1]; /* 兼容 */
        else if (!strcmp(argv[i], "--sock") || !strcmp(argv[i], "--peer") ||
                 !strcmp(argv[i], "--route") || !strcmp(argv[i], "--rdef"))
            { /* V28 及以前的参数：双路由已废除，静默忽略 */ }
        else {
            fprintf(stderr, "xiaoai-mic: unknown arg %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGPIPE, SIG_IGN);

    g_start_ms = now_ms();
    mkdir(tapdir, 0755);

    /* ① 原生路径（mipns uplink 唯一入口） */
    ls = bind_path(listen_path);
    if (ls < 0) return 1;
    g_listen = ls;
    chmod(listen_path, 0666);
    fcntl(ls, F_SETFL, O_NONBLOCK);

    /* ② LAB2 侧：bind 自己的 client 路径（mico 的 DOWN 回发到这），
     *    数据显式 sendto lab_path（LAB2 未 bind 前 ENOENT 丢包，下包重试） */
    lg = bind_path(client_path);
    if (lg < 0) { close(ls); return 1; }
    fcntl(lg, F_SETFL, O_NONBLOCK);
    memset(&lab_addr, 0, sizeof(lab_addr));
    lab_addr.sun_family = AF_UNIX;
    strncpy(lab_addr.sun_path, lab_path, sizeof(lab_addr.sun_path) - 1);

    taps_refresh();
    sync_sys_clock();
    logmsg("ready listen=%s lab=%s tapdir=%s pid=%d",
           listen_path, lab_path, tapdir, (int)getpid());

    /* ③ 会话：pns 可能尚未启动（S55 晚于本服务）→ 失败留 5s 冷却重试 */
    fifo_fd = open_stream();
    last_byte = now_ms();
    last_reopen = last_byte;
    if (fifo_fd >= 0)
        logmsg("voip session attached (streaming)");
    else
        logmsg("voip session pending (retry in 5s)");

    while (!g_stop) {
        struct pollfd pf[3];
        int r;
        pf[0].fd = ls;  pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = lg;  pf[1].events = POLLIN; pf[1].revents = 0;
        pf[2].fd = fifo_fd;
        pf[2].events = (fifo_fd >= 0) ? POLLIN : 0;
        pf[2].revents = 0;
        r = poll(pf, 3, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* UP：mipns → 官方（先转发，后扇出 —— 官方对话优先） */
        if (pf[0].revents & (POLLIN | POLLHUP)) {
            int guard = 0;
            for (;;) {
                uint8_t buf[MAXD];
                struct sockaddr_un from;
                socklen_t flen = sizeof(from);
                ssize_t n = recvfrom(ls, buf, sizeof(buf), 0,
                                     (struct sockaddr *)&from, &flen);
                if (n > 0) {
                    if (from.sun_family == AF_UNIX &&
                        (!have_mipns ||
                         strcmp(mipns_path, from.sun_path) != 0)) {
                        have_mipns = 1;
                        g_kws_arm = -1;      /* 新 mipns：强制重新 arm */
                        strncpy(mipns_path, from.sun_path,
                                sizeof(mipns_path) - 1);
                        mipns_path[sizeof(mipns_path) - 1] = 0;
                        memcpy(&mipns_addr, &from, sizeof(from));
                        logmsg("mipns uplink seen from %s", from.sun_path);
                    } else if (from.sun_family == AF_UNIX) {
                        memcpy(&mipns_addr, &from, sizeof(from));
                    }
                    if (sendto(lg, buf, (size_t)n, MSG_NOSIGNAL,
                               (const struct sockaddr *)&lab_addr,
                               sizeof(lab_addr)) < 0)
                        up_pkts++;   /* ENOENT：LAB2 未 bind（重启窗口）丢包 */
                    else
                        up_pkts++;
                    taps_fanout(buf, (size_t)n);
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                else if (n < 0 && errno == EINTR)
                    continue;
                else
                    break;
                if (++guard > 64) break;
            }
        }

        /* DOWN：mico(LAB2) → mipns（轮次/VAD/LED/打断全原生透传） */
        if (pf[1].revents & (POLLIN | POLLHUP)) {
            int guard = 0;
            for (;;) {
                uint8_t buf[MAXD];
                ssize_t n = recv(lg, buf, sizeof(buf), 0);
                if (n > 0) {
                    if (have_mipns) {
                        if (sendto(ls, buf, (size_t)n, MSG_NOSIGNAL,
                                   (const struct sockaddr *)&mipns_addr,
                                   sizeof(mipns_addr)) >= 0)
                            down_pkts++;
                    }
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                else if (n < 0 && errno == EINTR)
                    continue;
                else
                    break;
                if (++guard > 64) break;
            }
        }

        /* ③ 会话 PCM（idle 连续音频）→ 打包 → 只送 taps 扇出 */
        if (fifo_fd >= 0 &&
            (pf[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            int guard = 0;
            for (;;) {
                uint8_t tmp[4096];
                ssize_t n = read(fifo_fd, tmp, sizeof(tmp));
                if (n > 0) {
                    last_byte = now_ms();
                    size_t off = 0;
                    while (off < (size_t)n) {
                        size_t c = (8192 - fifo_got < (size_t)n - off)
                                     ? 8192 - fifo_got : (size_t)n - off;
                        memcpy(fifo_acc + fifo_got, tmp + off, c);
                        fifo_got += c;
                        off += c;
                        while (fifo_got >= FRAME) {
                            uint8_t pkt[32 + FRAME];
                            size_t pn = pack_pcm(pkt, fifo_acc, FRAME);
                            memmove(fifo_acc, fifo_acc + FRAME,
                                    fifo_got - FRAME);
                            fifo_got -= FRAME;
                            taps_fanout(pkt, pn);
                            feed_pkts++;
                        }
                    }
                } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                    break;
                } else if (n == 0) {
                    /* EOF：writer 断开 → 按 dry 处理（下轮重接） */
                    break;
                } else {
                    break;
                }
                if (++guard > 8) break;
            }
        }

        /* 低频任务（poll 200ms 超时保证周期性） */
        {
            int64_t now = now_ms();
            if (fifo_fd < 0) {
                if (now - last_reopen >= 5000) {
                    last_reopen = now;
                    fifo_fd = open_stream();
                    last_byte = now;
                    if (fifo_fd >= 0)
                        logmsg("voip session reopened");
                }
            } else if (now - last_byte >= 2000 && now - last_reopen >= 2000) {
                /* dry 2s：先 re-attach pns 真实 fifo fd（免事件风暴）；
                 * attach 不到 → 5s 冷却后整段重开 */
                close(fifo_fd);
                fifo_fd = attach_mipns_fifo();
                last_reopen = now;
                if (fifo_fd >= 0) {
                    last_byte = now;
                    logmsg("voip dry 2s, re-attached");
                } else {
                    fifo_fd = -1;
                    logmsg("voip dry 2s, reopen after 5s");
                }
            }
            if (now - last_tick >= 1000) {
                last_tick = now;
                kws_arm_inject();
                noplay_check();
                taps_refresh();
            }
            if (now - last_hb >= 120000) {
                last_hb = now;
                logmsg("hb up=%lds up=%lu down=%lu feed=%lu sess=%d taps=%d noplay=%d",
                       (long)((now - g_start_ms) / 1000),
                       up_pkts, down_pkts, feed_pkts,
                       (fifo_fd >= 0) ? 1 : 0, n_taps, (int)g_noplay);
            }
        }
    }

    unlink(listen_path);
    unlink(client_path);
    taps_close();
    if (fifo_fd >= 0) close(fifo_fd);
    close(ls);
    close(lg);
    logmsg("exited up=%lu down=%lu feed=%lu", up_pkts, down_pkts, feed_pkts);
    return 0;
}

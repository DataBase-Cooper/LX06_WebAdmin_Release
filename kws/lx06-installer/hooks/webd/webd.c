/* lx06 webd — 小爱音箱 Pro (LX06) 轻量 HTTP 控制台（V19，固件内组件）。
 *
 * 最终约束（用户确认）：
 *   - SSH 固定 22 + 恒仅局域网；HTTP 固定 88 + 仅局域网。
 *     两者「仅局域网」由 /usr/lib/lx06/ssh-lan.sh 在 DHCP bound 事件里用
 *     iptables 统一施加；webd 本身绑 0.0.0.0:88，不做源地址判断。
 *   - 刷机零配置：密码恒 DB_COOPER（镜像烘焙），用户刷完用
 *     http://<音箱IP>:88 登录改密码；SSH 与 HTTP 共用这一份密码。
 *   - 页面/逻辑在只读 squashfs（/usr/lib/lx06/web/ + 本二进制），
 *     用户数据全在 /data（恢复出厂清空）。
 *   - 改密码即时生效、不重启任何服务：
 *       HTTP 侧 webd 每次 /api/login 现读 /data/lx06-ssh.conf 的 hash；
 *       SSH 侧 PAM(pam_unix) 每次认证现读 /etc/shadow（bind 自
 *       /data/etc/shadow）。改密只改这两个文件 → 两入口的下一次登录
 *       即用新密码；已建立的 SSH 连接不受影响。
 *
 * API（密码 = conf 的 ssh_root_hash，$1$ MD5-crypt）：
 *   POST /api/login          {"password"}        校验 → 会话 cookie
 *   POST /api/logout                              销毁会话
 *   GET  /api/status                              22/88/LAN/ssh/uptime/内存/已装脚本
 *   GET  /api/system-stats                        实时 CPU%(全核汇总差值)/内存%(3s 轮询用)
 *   POST /api/password       {"old","new"}       验旧 → 写 conf + /data/etc/shadow
 *                                                  → 即时生效（不重启）
 *   POST /api/reboot                              先应答后 fork reboot -f
 *   POST /api/apps        multipart，保存 /tmp 原件及 /data/lx06/apps 持久副本。
 *   GET/POST /api/autostart  同登录会话管理原子持久 on/off 开关，不直接启动用户代码。
 *   POST /api/apps/run    {"name","args","profile"}，仅经本地原生 keeper。
 *   POST /api/apps/kill   {"name"}，keeper 确认整个专属 UID 树停止。
 *   POST /api/apps/uninstall {"name"}，keeper 停净后，若脚本前 64 行有
 *     # WEBD-UNINSTALL-TIMEOUT: N
 *     则限时以 root 执行该脚本 --uninstall，再删除受管内容。无此标记不跑脚本。
 *
 * 用户应用以独立低权限 UID 执行，HOME=/data/lx06/user/NAME。默认 600s、
 * 小虚拟地址空间；宽 VM/驻留必须由已登录管理者显式 profile 授权。
 * 不兼容旧 root 安装脚本，不恢复任意 /data 快照，不失败降级为 root。
 * 单线程 select ≤4 连接；接收缓冲使用启动时确定的全连接共享内存预算。
 *
 * MD5-crypt（$1$）内嵌实现，与 md5_crypt.go 逐字节一致（Go 版对 glibc
 * crypt(3) 19 组向量验证过）——登录/改密不依赖 libcrypt。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
/* ARM 使用真正的 stat64 布局和 glibc 2.25 旧符号，不能把 stat64 写进
 * 较小的 struct stat；主机回归则使用主机 ABI。新增 fd 校验同走旧符号。 */
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
#define LX06_STATBUF struct stat64
extern int __xstat64(int, const char *, struct stat64 *);
extern int __fxstat64(int, int, struct stat64 *);
extern int __fxstatat64(int, int, const char *, struct stat64 *, int);
#define LX06_STAT(p, st) __xstat64(3, (p), (st))
#define LX06_FSTAT(fd, st) __fxstat64(3, (fd), (st))
#define LX06_STATAT(fd, p, st, f) __fxstatat64(3, (fd), (p), (st), (f))
#else
#define LX06_STATBUF struct stat
#define LX06_STAT(p, st) stat((p), (st))
#define LX06_FSTAT(fd, st) fstat((fd), (st))
#define LX06_STATAT(fd, p, st, f) fstatat((fd), (p), (st), (f))
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/prctl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/file.h> /* flock（standalone 单实例护栏） */
#include "../xiaoai/session-owner.h"
#include "../xiaoai/session-epoch.h"
#include <math.h>
#include "keeper_client.h"
#define LX06_POLICY_STAT LX06_STATBUF
#define LX06_POLICY_FSTAT LX06_FSTAT
#include "../robust/script-policy.h"
#define LX06_LOG_STAT LX06_STATBUF
#define LX06_LOG_FSTAT LX06_FSTAT
#include "../robust/bounded-log.h"
#include "../xiaoai/channel-route.h"
#include "../xiaoai/official-bind.h"
#include "../xiaoai/template-matcher.c"
#include "../xiaoai/script-phrase-check.c"
#include "keeper_args.h"
#include "app-system-clock.h"

#ifndef PORT
#define PORT      88
#endif
#ifndef WEBDIR
#define WEBDIR    "/usr/lib/lx06/web"
#endif
#define CONF      "/data/lx06-ssh.conf"
#define LOGCONF_PATH "/data/lx06-log.conf"   /* 日志传送配置（看门狗重启前读取发送） */
#define SHADOW    "/data/etc/shadow"
/* 上传原件在 /tmp，root 只读持久副本在 /data/lx06/apps；用户仅可写专属 HOME。
   两份均由管理端校验/原子写入，旧 root 安装器没有兼容执行回退。 */
#ifndef APPDIR
#define APPDIR    "/tmp/lx06-uploads"
#endif
#ifndef APPPERSIST
#define APPPERSIST "/data/lx06/apps" /* root 0755，用户 UID 只读/执行 */
#endif
#ifndef APPDATA
#define APPDATA "/data/lx06"
#endif
#define APPLOGDIR  "/tmp/lx06-keeper-logs" /* daemon 管道收集，每 app 硬上限 64KB */
#define BKDIR      "/data/lx06/.bk" /* root 0700，仅运行元数据，不向用户暴露 */
#define LOGDIR    "/data/lx06/logs"
#ifndef RUNLOGDIR
#define RUNLOGDIR "/tmp/lx06-logs"
#endif
#define REQLOG    RUNLOGDIR "/webd-req.log"    /* HTTP 请求日志，64K 轮转 */
#define SSHLOG    RUNLOGDIR "/ssh-auth.log"    /* dropbear wrapper 轮转 */
#define WEBDLOG   RUNLOGDIR "/webd.log"        /* 本次启动的 webd 日志 */
#define LOGROT_MAX 65536                        /* fix15：所有日志硬上限 64K（含 ssh/web/app 全部） */
#ifndef SESSFILE
#define SESSFILE  "/tmp/lx06-logs/.webd_session"
#endif
#define MAXCONN   4
#define MAXBODY   (64 * 1024 * 1024)  /* 历史名：不再作为文件大小上限；接收预算启动时按实机内存动态计算（256MB/大内存机型通吃）。 */
#define MIN_DATA_FREE_KB 1024L        /* 部署前 /data 剩余空间下限（应用落盘余量） */
#define MAXHDR    16384

/* ============================ MD5 (RFC 1321) ============================ */
typedef struct {
  unsigned a, b, c, d;
  unsigned long long bits;
  unsigned char buf[64];
  unsigned off;
} MD5;
#define ROL(x,n) ((((x)&0xffffffffu)<<(n))|((x)>>(32-(n))))
static void md5_block(MD5 *m, const unsigned char p[64]) {
  unsigned x[16], i, a, b, c, d, t;
  for (i = 0; i < 16; i++)
    x[i] = (unsigned)p[i*4] | ((unsigned)p[i*4+1] << 8) |
           ((unsigned)p[i*4+2] << 16) | ((unsigned)p[i*4+3] << 24);
  a = m->a; b = m->b; c = m->c; d = m->d;
  /* round 1 */
    t = ROL(a + (d ^ (b & (c ^ d))) + x[0] + 0xd76aa478u, 7) + b; a = t;
    t = ROL(d + (c ^ (a & (b ^ c))) + x[1] + 0xe8c7b756u, 12) + a; d = t;
    t = ROL(c + (b ^ (d & (a ^ b))) + x[2] + 0x242070dbu, 17) + d; c = t;
    t = ROL(b + (a ^ (c & (d ^ a))) + x[3] + 0xc1bdceeeu, 22) + c; b = t;
    t = ROL(a + (d ^ (b & (c ^ d))) + x[4] + 0xf57c0fafu, 7) + b; a = t;
    t = ROL(d + (c ^ (a & (b ^ c))) + x[5] + 0x4787c62au, 12) + a; d = t;
    t = ROL(c + (b ^ (d & (a ^ b))) + x[6] + 0xa8304613u, 17) + d; c = t;
    t = ROL(b + (a ^ (c & (d ^ a))) + x[7] + 0xfd469501u, 22) + c; b = t;
    t = ROL(a + (d ^ (b & (c ^ d))) + x[8] + 0x698098d8u, 7) + b; a = t;
    t = ROL(d + (c ^ (a & (b ^ c))) + x[9] + 0x8b44f7afu, 12) + a; d = t;
    t = ROL(c + (b ^ (d & (a ^ b))) + x[10] + 0xffff5bb1u, 17) + d; c = t;
    t = ROL(b + (a ^ (c & (d ^ a))) + x[11] + 0x895cd7beu, 22) + c; b = t;
    t = ROL(a + (d ^ (b & (c ^ d))) + x[12] + 0x6b901122u, 7) + b; a = t;
    t = ROL(d + (c ^ (a & (b ^ c))) + x[13] + 0xfd987193u, 12) + a; d = t;
    t = ROL(c + (b ^ (d & (a ^ b))) + x[14] + 0xa679438eu, 17) + d; c = t;
    t = ROL(b + (a ^ (c & (d ^ a))) + x[15] + 0x49b40821u, 22) + c; b = t;
  /* round 2 */
    t = ROL(a + (c ^ (d & (b ^ c))) + x[1] + 0xf61e2562u, 5) + b; a = t;
    t = ROL(d + (b ^ (c & (a ^ b))) + x[6] + 0xc040b340u, 9) + a; d = t;
    t = ROL(c + (a ^ (b & (d ^ a))) + x[11] + 0x265e5a51u, 14) + d; c = t;
    t = ROL(b + (d ^ (a & (c ^ d))) + x[0] + 0xe9b6c7aau, 20) + c; b = t;
    t = ROL(a + (c ^ (d & (b ^ c))) + x[5] + 0xd62f105du, 5) + b; a = t;
    t = ROL(d + (b ^ (c & (a ^ b))) + x[10] + 0x02441453u, 9) + a; d = t;
    t = ROL(c + (a ^ (b & (d ^ a))) + x[15] + 0xd8a1e681u, 14) + d; c = t;
    t = ROL(b + (d ^ (a & (c ^ d))) + x[4] + 0xe7d3fbc8u, 20) + c; b = t;
    t = ROL(a + (c ^ (d & (b ^ c))) + x[9] + 0x21e1cde6u, 5) + b; a = t;
    t = ROL(d + (b ^ (c & (a ^ b))) + x[14] + 0xc33707d6u, 9) + a; d = t;
    t = ROL(c + (a ^ (b & (d ^ a))) + x[3] + 0xf4d50d87u, 14) + d; c = t;
    t = ROL(b + (d ^ (a & (c ^ d))) + x[8] + 0x455a14edu, 20) + c; b = t;
    t = ROL(a + (c ^ (d & (b ^ c))) + x[13] + 0xa9e3e905u, 5) + b; a = t;
    t = ROL(d + (b ^ (c & (a ^ b))) + x[2] + 0xfcefa3f8u, 9) + a; d = t;
    t = ROL(c + (a ^ (b & (d ^ a))) + x[7] + 0x676f02d9u, 14) + d; c = t;
    t = ROL(b + (d ^ (a & (c ^ d))) + x[12] + 0x8d2a4c8au, 20) + c; b = t;
  /* round 3 */
    t = ROL(a + (b ^ c ^ d) + x[5] + 0xfffa3942u, 4) + b; a = t;
    t = ROL(d + (a ^ b ^ c) + x[8] + 0x8771f681u, 11) + a; d = t;
    t = ROL(c + (d ^ a ^ b) + x[11] + 0x6d9d6122u, 16) + d; c = t;
    t = ROL(b + (c ^ d ^ a) + x[14] + 0xfde5380cu, 23) + c; b = t;
    t = ROL(a + (b ^ c ^ d) + x[1] + 0xa4beea44u, 4) + b; a = t;
    t = ROL(d + (a ^ b ^ c) + x[4] + 0x4bdecfa9u, 11) + a; d = t;
    t = ROL(c + (d ^ a ^ b) + x[7] + 0xf6bb4b60u, 16) + d; c = t;
    t = ROL(b + (c ^ d ^ a) + x[10] + 0xbebfbc70u, 23) + c; b = t;
    t = ROL(a + (b ^ c ^ d) + x[13] + 0x289b7ec6u, 4) + b; a = t;
    t = ROL(d + (a ^ b ^ c) + x[0] + 0xeaa127fau, 11) + a; d = t;
    t = ROL(c + (d ^ a ^ b) + x[3] + 0xd4ef3085u, 16) + d; c = t;
    t = ROL(b + (c ^ d ^ a) + x[6] + 0x04881d05u, 23) + c; b = t;
    t = ROL(a + (b ^ c ^ d) + x[9] + 0xd9d4d039u, 4) + b; a = t;
    t = ROL(d + (a ^ b ^ c) + x[12] + 0xe6db99e5u, 11) + a; d = t;
    t = ROL(c + (d ^ a ^ b) + x[15] + 0x1fa27cf8u, 16) + d; c = t;
    t = ROL(b + (c ^ d ^ a) + x[2] + 0xc4ac5665u, 23) + c; b = t;
  /* round 4 */
    t = ROL(a + (c ^ (b | ~d)) + x[0] + 0xf4292244u, 6) + b; a = t;
    t = ROL(d + (b ^ (a | ~c)) + x[7] + 0x432aff97u, 10) + a; d = t;
    t = ROL(c + (a ^ (d | ~b)) + x[14] + 0xab9423a7u, 15) + d; c = t;
    t = ROL(b + (d ^ (c | ~a)) + x[5] + 0xfc93a039u, 21) + c; b = t;
    t = ROL(a + (c ^ (b | ~d)) + x[12] + 0x655b59c3u, 6) + b; a = t;
    t = ROL(d + (b ^ (a | ~c)) + x[3] + 0x8f0ccc92u, 10) + a; d = t;
    t = ROL(c + (a ^ (d | ~b)) + x[10] + 0xffeff47du, 15) + d; c = t;
    t = ROL(b + (d ^ (c | ~a)) + x[1] + 0x85845dd1u, 21) + c; b = t;
    t = ROL(a + (c ^ (b | ~d)) + x[8] + 0x6fa87e4fu, 6) + b; a = t;
    t = ROL(d + (b ^ (a | ~c)) + x[15] + 0xfe2ce6e0u, 10) + a; d = t;
    t = ROL(c + (a ^ (d | ~b)) + x[6] + 0xa3014314u, 15) + d; c = t;
    t = ROL(b + (d ^ (c | ~a)) + x[13] + 0x4e0811a1u, 21) + c; b = t;
    t = ROL(a + (c ^ (b | ~d)) + x[4] + 0xf7537e82u, 6) + b; a = t;
    t = ROL(d + (b ^ (a | ~c)) + x[11] + 0xbd3af235u, 10) + a; d = t;
    t = ROL(c + (a ^ (d | ~b)) + x[2] + 0x2ad7d2bbu, 15) + d; c = t;
    t = ROL(b + (d ^ (c | ~a)) + x[9] + 0xeb86d391u, 21) + c; b = t;
  m->a += a; m->b += b; m->c += c; m->d += d;
}
static void md5_init(MD5 *m) {
  m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
  m->bits = 0; m->off = 0;
  memset(m->buf, 0, sizeof(m->buf));   /* 根因修复：buf 未初始化读（O2 间歇错，gdb attach 碰巧对） */
}
static void md5_upd(MD5 *m, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  m->bits += (unsigned long long)len << 3;
  while (len > 0) {
    unsigned n = 64 - m->off;
    if (n > len) n = (unsigned)len;
    memcpy(m->buf + m->off, p, n);
    m->off += n; p += n; len -= n;
    if (m->off == 64) { md5_block(m, m->buf); m->off = 0; }
  }
}
static void md5_fin(MD5 *m, unsigned char out[16]) {
  unsigned long long bits = m->bits;
  unsigned char lenb[8];
  int i, n = (int)m->off;
  for (i = 0; i < 8; i++) lenb[i] = (unsigned char)((bits >> (8*i)) & 0xff);
  m->buf[n++] = 0x80;
  if (n > 56) {
    while (n < 64) m->buf[n++] = 0;
    md5_block(m, m->buf);
    n = 0;
  }
  while (n < 56) m->buf[n++] = 0;
  for (i = 0; i < 8; i++) m->buf[n++] = lenb[i];
  md5_block(m, m->buf);
  out[0]  = m->a & 0xff; out[1]  = (m->a >> 8) & 0xff;
  out[2]  = (m->a >> 16) & 0xff; out[3] = (m->a >> 24) & 0xff;
  out[4]  = m->b & 0xff; out[5]  = (m->b >> 8) & 0xff;
  out[6]  = (m->b >> 16) & 0xff; out[7] = (m->b >> 24) & 0xff;
  out[8]  = m->c & 0xff; out[9]  = (m->c >> 8) & 0xff;
  out[10] = (m->c >> 16) & 0xff; out[11] = (m->c >> 24) & 0xff;
  out[12] = m->d & 0xff; out[13] = (m->d >> 8) & 0xff;
  out[14] = (m->d >> 16) & 0xff; out[15] = (m->d >> 24) & 0xff;
}
static void md5_digest(const void *data, size_t len, unsigned char out[16]) {
  MD5 m;
  md5_init(&m);
  md5_upd(&m, data, len);
  md5_fin(&m, out);
}

/* ================= MD5-crypt（$1$，与 md5_crypt.go 逐字节一致） ================= */
static const char B64T[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
static void b64out32(unsigned w, int n, char *o) {
  int i;
  for (i = 0; i < n; i++) { o[i] = B64T[w & 0x3f]; w >>= 6; }
}
/* 输出 "$1$<salt>$<22>"。salt 可带 $1$ 前缀，截断 8 字符（$ : \n 终止）。 */
static void md5crypt(const char *pw, const char *salt_in, char *out) {
  unsigned char alt[16], res[16], one;
  size_t plen = strlen(pw), i, cnt;
  int r, k;
  const char *e;
  char salt[9];
  if (strncmp(salt_in, "$1$", 3) == 0) salt_in += 3;
  e = strpbrk(salt_in, "$:\n");
  k = e ? (int)(e - salt_in) : (int)strlen(salt_in);
  if (k > 8) k = 8;
  memcpy(salt, salt_in, k);
  salt[k] = 0;
  /* Phase 1: alt = MD5(pw + salt + pw) */
  {
    MD5 m;
    md5_init(&m);
    md5_upd(&m, pw, plen); md5_upd(&m, salt, k); md5_upd(&m, pw, plen);
    md5_fin(&m, alt);
  }
  /* Phase 2: base = MD5(pw + "$1$" + salt + alt[i%16]×plen) + parity 循环 */
  {
    MD5 m;
    md5_init(&m);
    md5_upd(&m, pw, plen); md5_upd(&m, "$1$", 3); md5_upd(&m, salt, k);
    for (i = 0; i < plen; i++) { one = alt[i % 16]; md5_upd(&m, &one, 1); }
    alt[0] = 0;
    for (cnt = plen; cnt > 0; cnt >>= 1) {
      one = (cnt & 1) ? (unsigned char)0 : (unsigned char)pw[0];
      md5_upd(&m, &one, 1);
    }
    md5_fin(&m, res);
  }
  /* Phase 3: 1000 轮混合 */
  for (r = 0; r < 1000; r++) {
    MD5 m;
    md5_init(&m);
    if (r & 1) md5_upd(&m, pw, plen); else md5_upd(&m, res, 16);
    if (r % 3) md5_upd(&m, salt, k);
    if (r % 7) md5_upd(&m, pw, plen);
    if (r & 1) md5_upd(&m, res, 16); else md5_upd(&m, pw, plen);
    md5_fin(&m, res);
  }
  sprintf(out, "$1$%s$", salt);
  {
    int o = (int)strlen(out);
    b64out32((res[0]<<16)|(res[6]<<8)|res[12], 4, out + o); o += 4;
    b64out32((res[1]<<16)|(res[7]<<8)|res[13], 4, out + o); o += 4;
    b64out32((res[2]<<16)|(res[8]<<8)|res[14], 4, out + o); o += 4;
    b64out32((res[3]<<16)|(res[9]<<8)|res[15], 4, out + o); o += 4;
    b64out32((res[4]<<16)|(res[10]<<8)|res[5], 4, out + o); o += 4;
    b64out32(res[11], 2, out + o); o += 2;
    out[o] = 0;
  }
}

/* ============================ 工具 ============================ */
static int rand_bytes(unsigned char *buf, size_t n) {
  FILE *f = fopen("/dev/urandom", "rb");
  size_t got;
  if (!f) return -1;
  got = fread(buf, 1, n, f);
  fclose(f);
  return got == n ? 0 : -1;
}
/* 随机 salt：8 个 crypt base64 字母表字符（与 md5_crypt.go randomSalt 同分布） */
static void rand_salt(char *out) {
  unsigned char b[8];
  int i;
  if (rand_bytes(b, 8) != 0) {
    time_t t = time(0);
    for (i = 0; i < 8; i++) b[i] = (unsigned char)(t >> (i * 8));
  }
  for (i = 0; i < 8; i++) out[i] = B64T[b[i] & 0x3f];
  out[8] = 0;
}
static int file_read_all(const char *path, char **out, size_t *outlen) {
  FILE *f = fopen(path, "rb");
  long sz;
  size_t got;
  if (!f) return -1;
  fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz < 0 || sz > (12 * 1024 * 1024)) { fclose(f); return -1; }
  *out = (char *)malloc((size_t)sz + 1);
  if (!*out) { fclose(f); return -1; }
  got = fread(*out, 1, (size_t)sz, f);
  fclose(f);
  (*out)[got] = 0;
  *outlen = got;
  return 0;
}
static long file_size(const char *p) {
  /* 普通日志优先 lseek；其余查询使用与目标 libc 匹配的 stat64 桥。 */
  int fd = open(p, O_RDONLY);
  if (fd >= 0) {
    long sz = lseek(fd, 0, SEEK_END);
    close(fd);
    if (sz >= 0) return sz;
  }
  LX06_STATBUF st;
  if (LX06_STAT(p, &st) != 0) return -1;
  return (long)st.st_size;
}
/* 从 key=value 行文件读一个 key 的值（第一个出现的行） */
static int conf_get(const char *path, const char *key, char *out, int outsz) {
  char *s = NULL;
  size_t n, i, L;
  if (file_read_all(path, &s, &n) != 0) return -1;
  L = n;
  for (i = 0; i < L; i++) {
    if (i > 0 && s[i-1] != '\n') continue;
    if (strncmp(s + i, key, strlen(key)) == 0 && s[i + strlen(key)] == '=') {
      const char *v = s + i + strlen(key) + 1;
      const char *e2 = strchr(v, '\n');
      size_t l = e2 ? (size_t)(e2 - v) : L - (i + strlen(key) + 1);
      if (l >= (size_t)outsz) l = (size_t)outsz - 1;
      memcpy(out, v, l);
      out[l] = 0;
      free(s);
      return 0;
    }
  }
  free(s);
  return -1;
}
/* 校验密码对 $1$ hash 是否成立 */
static int hash_match(const char *hash, const char *pw) {
  char buf[128], salt[16];
  const char *s, *e;
  int l;
  if (strncmp(hash, "$1$", 3) != 0) return 0;
  s = hash + 3;
  e = strchr(s, '$');
  if (!e) return 0;
  l = (int)(e - s);
  if (l < 1 || l > 8) return 0;
  memcpy(salt, s, l);
  salt[l] = 0;
  md5crypt(pw, salt, buf);
  return strcmp(buf, hash) == 0;
}
static int write_all(int fd, const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) { if (errno == EINTR) continue; return -1; }
    s += w; n -= (size_t)w;
  }
  return 0;
}

/* webd 是 select 单线程模型（route 在 select 主线程内串行），gmtime_r 足够。 */
static struct tm *lx06_gmtime_r(const time_t *t, struct tm *o);
static struct tm *lx06_gmtime_r(const time_t *t, struct tm *o) {
  return gmtime_r(t, o);
}
/* fix15 日志管理函数前置声明（定义见 h_apps_log 之前，供 reqlog 早用） */
static void log_rotate(const char *p);
static void log_sweep(void);
static void log_set_off(const char *p);
static void log_clear_file(const char *p);
static int webd_log_sink(const char *key);
static void log_append_line(const char *path, const char *line) {
  (void)lx06_log_append(AT_FDCWD, path, line, strlen(line));
}
/* ---- 请求日志：fix15 恢复，受 web 日志开关控制（conf log_web=0 关闭）。
   行格式：<epoch> <method> <path> <ip> <code>。写后按 64K 硬上限轮转。 ---- */
static void reqlog(const char *ip, const char *method, const char *path, int code) {
  char dv[8], buf[300];
  /* 开关缺省=开（日志查看功能需要默认有内容）；显式 log_web=0 才关 */
  if (conf_get(CONF, "log_web", dv, sizeof(dv)) == 0 && dv[0] == '0')
    return;
  snprintf(buf, sizeof(buf), "%ld %s %s %s %d\n",
           (long)time(NULL), method, path, ip, code);
  log_append_line(REQLOG, buf);
}
/* conf 写单键：保留其他行，替换/追加 key=value（S3 diag_push 开关用） */
static int conf_set_key(const char *key, const char *val) {
  char *s = NULL;
  size_t n, i, L;
  char outb[1600];
  int w = 0, found = 0;
  if (file_read_all(CONF, &s, &n) == 0) {
    L = n;
    for (i = 0; i < L; i++) {
      if (i > 0 && s[i-1] != '\n') continue;
      if (strncmp(s + i, key, strlen(key)) == 0 && s[i + strlen(key)] == '=') {
        /* 只 continue（for 的 i++ 逐字符走到下一行行首）。
           若在此把 i 预跳到下一行行首再 continue，for 的 i++ 会再多跳 1 字符，
           使下一行的行首判定 s[i-1]=='\n' 失配 → 下一整行被丢。
           真机钉死（2026-10-11 受控复现）：diag 写 diag_push（倒数第2行）把
           最后一行 ssh_root_hash 丢掉 → HTTP 登录/改密（依赖该行）全 401，
           SSH（读 shadow）不受影响 → 双通道失同步。改法与 h_password 跳过分支
           一致（简单 continue，已验证不丢末行）。 */
        found = 1;
        continue;
      }
      {
        const char *e2 = strchr(s + i, '\n');
        size_t l = e2 ? (size_t)(e2 - (s + i)) : L - i;
        if ((size_t)(w + (int)l + 2) > sizeof(outb)) break;
        memcpy(outb + w, s + i, l);
        w += (int)l;
        outb[w++] = '\n';
      }
    }
    free(s);
  }
  if ((size_t)(w + (int)strlen(key) + (int)strlen(val) + 3) > sizeof(outb)) return -1;
  w += sprintf(outb + w, "%s=%s\n", key, val);
  {
    FILE *f = fopen(CONF, "w");
    if (!f) return -1;
    fwrite(outb, 1, (size_t)w, f);
    fclose(f);
  }
  (void)found;
  return 0;
}

/* ============================ HTTP 连接 ============================ */
#include "../upgrade/ota-stream.h"
typedef struct {
  int fd;
  char *buf;            /* head + body 累积缓冲 */
  size_t buflen;
  size_t bufcap;        /* buf 可用容量（V13：按需扩容，普通请求只占 64KB） */
  int have_head;
  size_t head_end;      /* buf 中行末空行后第一字节下标 */
  long bodywant;        /* 客户端声明的 Content-Length 等待量 */
  long bodyorig;        /* 客户端声明的原始 Content-Length */
  long last_act;        /* 最近活动时刻（accept/读到数据时刷新；空闲超时回收用） */
  char cip[16];         /* 对端 IP（请求日志用） */
  ota_stream *ota;      /* verifier only: bounded state, no staged image */
  size_t ota_received;
  char ota_cookie[256];
  int ota_install;      /* 1 = /api/upgrade/install（A/B 槽位写入），0 = verify-only */
  int ota_prepare;      /* verify a package for a subsequent bounded installation */
  int ota_wipe;         /* X-LX06-Wipe: 0|1（仅 install；升级后清空共享 DATA） */
  int ota_fd;           /* 落盘文件句柄（仅 install），conn_reset 关闭 */
  int ota_lock_fd;      /* /tmp/lx06-ota.lock 句柄；fork 后由 worker 持有 */
  int ota_file_handed;  /* 1 = 包文件已交给安装 worker，reset 时不删除 */
  char ota_file[128];   /* /tmp/lx06-uploads/<file>.lx06ota（仅 install） */
} conn_t;
static conn_t conns[MAXCONN];
static conn_t *ota_active; /* one upload; reset on EOF/error/timeout/disconnect */
#define CONN_IDLE_TIMEOUT 60  /* 秒：连接槽空闲超时（回收对端已 RST/断网的死连接，
                               防 4 槽被死连接占满 → accept 无空槽 → 88 全挂，
                               即「登录响应异常、要刷新才进」的根因）。
                               只按空闲计：10MB 上传持续有数据到达，不会被误杀。 */

/* 64MiB 不再封顶：共享接收预算按实机内存动态适配（256MB 版与更大内存版通用），
 * 启动时一次取 min(total/16, available/8)，所有连接共享；GET 不改限。
 * /proc 不可读/无可用内存时 fail closed（预算=0 → 上传一律 503），不能猜测机型后开大缓冲。 */
static size_t http_buffer_budget;
static size_t http_budget_from_kb(uint64_t total, uint64_t available) {
  uint64_t kb;
  if (!total || !available || available > total) return 0;
  /* 旧版 total/16 再被 available/8 压顶：开机后 MemAvailable≈51M 时预算仅 ~6M，
   * 6M+ 上传一律 413，而 /data 尚有空间 → 用户看到「空间明明够却报错」。
   * 现 total/8、available/4 封顶、2M 保底、32M 上限：118M 板 ≈12.6M >
   * /data 最大可用（~8.6M），空间真不够时由 507 如实报「data空间不足」。 */
  kb = total / 8;
  if (kb > available / 4) kb = available / 4;
  if (kb < 2048) kb = 2048;
  if (kb > 32768) kb = 32768;
  return (size_t)kb * 1024;
}
static void http_budget_init(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  char b[256];
  unsigned long long total = 0, available = 0, value;
  if (f) {
    while (fgets(b, sizeof(b), f)) {
      if (sscanf(b, "MemTotal: %llu kB", &value) == 1) total = value;
      else if (sscanf(b, "MemAvailable: %llu kB", &value) == 1) available = value;
    }
    if (ferror(f)) total = available = 0;
    fclose(f);
  }
  http_buffer_budget = http_budget_from_kb(total, available);
}
static int conn_grow(conn_t *c, size_t wanted) {
  size_t used = 0, old = c->buf ? c->bufcap + 1 : 0;
  char *next;
  int i;
  if (wanted <= c->bufcap && c->buf) return 0;
  /* 单连接上界=动态共享预算本身（不设固定 64MiB）；全部连接累计仍不得超预算。 */
  if (wanted + 1 > http_buffer_budget) { errno = E2BIG; return -1; }
  for (i = 0; i < MAXCONN; i++) if (conns[i].buf) used += conns[i].bufcap + 1;
  if (used < old || used - old > http_buffer_budget ||
      wanted + 1 > http_buffer_budget - (used - old)) { errno = ENOMEM; return -1; }
  next = realloc(c->buf, wanted + 1);
  if (!next) return -1;
  c->buf = next; c->bufcap = wanted;
  return 0;
}
/* 不用 atol/hdr_get 截断长度：拒绝负号、溢出、重复长度、尾随垃圾和 TE。
 * 只读首个完整 header；multipart 附加字节仍受 MAXHDR 编译边界约束。 */
static int http_content_length(const char *head, size_t *length) {
  const char *p = strstr(head, "\r\n"), *end = strstr(head, "\r\n\r\n");
  int seen = 0;
  *length = 0;
  if (!p || !end || (size_t)(end + 4 - head) > MAXHDR) { errno = EINVAL; return -1; }
  p += 2;
  while (p < end) {
    const char *nl = strstr(p, "\r\n"), *colon;
    size_t keylen;
    if (!nl || nl > end || !(colon = memchr(p, ':', (size_t)(nl - p)))) { errno = EINVAL; return -1; }
    keylen = (size_t)(colon - p);
    if (keylen == 17 && !strncasecmp(p, "Transfer-Encoding", keylen)) { errno = EINVAL; return -1; }
    if (keylen == 14 && !strncasecmp(p, "Content-Length", keylen)) {
      const char *v = colon + 1;
      size_t n = 0;
      if (seen++) { errno = EINVAL; return -1; }
      while (v < nl && (*v == ' ' || *v == '\t')) v++;
      if (v == nl || *v < '0' || *v > '9') { errno = EINVAL; return -1; }
      while (v < nl && *v >= '0' && *v <= '9') {
        unsigned digit = (unsigned)(*v++ - '0');
        /* 防溢出上界 1GiB（long 可容纳）；真实超限由预算检查报「超过共享接收内存预算」。 */
        if (n > ((size_t)0x40000000u - digit) / 10) { errno = E2BIG; return -1; }
        n = n * 10 + digit;
      }
      while (v < nl && (*v == ' ' || *v == '\t')) v++;
      if (v != nl) { errno = EINVAL; return -1; }
      *length = n;
    }
    p = nl + 2;
  }
  return 0;
}

static void conn_reset(conn_t *c) {
  if (ota_active == c) ota_active = NULL;
  ota_stream_free(c->ota); c->ota = NULL;
  c->ota_received = 0; c->ota_cookie[0] = 0;
  if (c->ota_fd >= 0) { close(c->ota_fd); c->ota_fd = -1; }
  if (c->ota_lock_fd >= 0) { close(c->ota_lock_fd); c->ota_lock_fd = -1; }
  if (c->ota_file[0]) {
    if (!c->ota_file_handed) unlink(c->ota_file);
    c->ota_file[0] = 0; c->ota_file_handed = 0;
  }
  c->ota_install = 0; c->ota_wipe = 0; c->ota_prepare = 0;
  if (c->buf) { free(c->buf); c->buf = 0; }
  c->fd = -1; c->buflen = 0; c->bufcap = 0; c->have_head = 0; c->head_end = 0;
  c->bodywant = 0; c->bodyorig = 0; c->last_act = 0; c->cip[0] = 0;
}
static void conn_close(conn_t *c) {
  if (c->fd >= 0) close(c->fd);
  conn_reset(c);
}

/* --- 会话 --- */
#define MAXSESS 4  /* 多会话：最多 4 个并存令牌（手机+电脑+脚本可同时在线），
                    登录只追加不覆盖 → 后登录不踢先登录；满 4 个时挤掉最旧的
                    （LRU）。/tmp/lx06-logs/.webd_session 每行一个 64 位 hex 令牌。 */
static int sess_token_from_cookie(const char *cookie, char *out, size_t outsz) {
  const char *c;
  int i;
  if (!cookie) return -1;
  c = strstr(cookie, "webd=");
  if (!c) return -1;
  c += 5;
  i = 0;
  while (c[i] && c[i] != ';' && c[i] != ' ' && i < (int)outsz - 1) { out[i] = c[i]; i++; }
  out[i] = 0;
  return out[0] ? 0 : -1;
}
static int sess_ok(const char *cookie) {
  char got[128], line[128];
  FILE *f;
  if (sess_token_from_cookie(cookie, got, sizeof(got)) != 0) return 0;
  f = fopen(SESSFILE, "r");
  if (!f) return 0;
  while (fgets(line, sizeof(line), f)) {
    size_t w = strlen(line);
    while (w && (line[w-1]=='\n' || line[w-1]==' ' || line[w-1]=='\r')) line[--w] = 0;
    if (line[0] && strcmp(line, got) == 0) { fclose(f); return 1; }
  }
  fclose(f);
  return 0;
}
static void sess_rand_tok(char *tok, size_t n) {
  unsigned char rb[32];
  int i;
  (void)n;
  if (rand_bytes(rb, 32) != 0) {
    time_t t = time(0);
    for (i = 0; i < 32; i++) rb[i] = (unsigned char)(t >> ((i * 3) & 31));
  }
  for (i = 0; i < 32; i++) {
    tok[i*2]   = "0123456789abcdef"[rb[i] >> 4];
    tok[i*2+1] = "0123456789abcdef"[rb[i] & 15];
  }
  tok[64] = 0;
}
static void sess_add(const char *tok) {
  char toklist[MAXSESS][65];
  int n = 0, i;
  FILE *f = fopen(SESSFILE, "r");
  if (f) {
    char line[80];
    while (n < MAXSESS && fgets(line, sizeof(line), f)) {
      size_t w = strlen(line);
      while (w && (line[w-1]=='\n' || line[w-1]==' ' || line[w-1]=='\r')) line[--w] = 0;
      if (line[0]) { strncpy(toklist[n], line, 64); toklist[n][64] = 0; n++; }
    }
    fclose(f);
  }
  for (i = 0; i < n; i++)
    if (strcmp(toklist[i], tok) == 0) return;   /* 去重（重复登录：令牌已在池，原样有效） */
  if (n == MAXSESS)                                    /* 满则挤掉最旧 */
    for (i = 1; i < MAXSESS; i++) strcpy(toklist[i-1], toklist[i]);
  strcpy(toklist[MAXSESS-1], tok);
  f = fopen(SESSFILE, "w");
  if (f) {
    for (i = 0; i < MAXSESS; i++) {
      if (toklist[i][0]) { fwrite(toklist[i], 1, strlen(toklist[i]), f); fputc('\n', f); }
    }
    fclose(f);
  }
  chmod(SESSFILE, 0600);
}
static void sess_remove(const char *tok) {   /* logout：只删本会话，不踢别人 */
  char toklist[MAXSESS][65];
  int n = 0, i;
  FILE *f = fopen(SESSFILE, "r");
  if (f) {
    char line[80];
    /* 收集时即跳过被删令牌（勿先读全再删——删后元素必须下移，否则写回错位） */
    while (n < MAXSESS && fgets(line, sizeof(line), f)) {
      size_t w = strlen(line);
      while (w && (line[w-1]=='\n' || line[w-1]==' ' || line[w-1]=='\r')) line[--w] = 0;
      if (line[0] && strcmp(line, tok) != 0) { strncpy(toklist[n], line, 64); toklist[n][64] = 0; n++; }
    }
    fclose(f);
  }
  if (n == 0) { remove(SESSFILE); return; }
  f = fopen(SESSFILE, "w");
  if (f) {
    for (i = 0; i < n; i++) { fwrite(toklist[i], 1, strlen(toklist[i]), f); fputc('\n', f); }
    fclose(f);
  }
  chmod(SESSFILE, 0600);
}

/* --- 应答 --- */
/* 请求日志上下文（单线程 select 模型：route 在分派前设置；主循环 413/431 用默认 "-"） */
static const char *req_ctx_ip = "-";
static const char *req_ctx_method = "-";
static const char *req_ctx_path = "-";
static void send_simple(int fd, int code, const char *msg, const char *ctype,
                        const char *body, size_t blen, const char *extra_hdr) {
  char h[600];
  int n = snprintf(h, sizeof(h),
    "HTTP/1.1 %d %s\r\nServer: lx06webd/1.0\r\nContent-Type: %s\r\n"
    "Content-Length: %lu\r\n%sConnection: close\r\n\r\n",
    code, msg, ctype, (unsigned long)blen, extra_hdr ? extra_hdr : "");
  if (write_all(fd, h, (size_t)n) != 0) return;
  if (blen && write_all(fd, body, blen) != 0) return;
  reqlog(req_ctx_ip, req_ctx_method, req_ctx_path, code);
}
static void send_json(int fd, const char *json) {
  send_simple(fd, 200, "OK", "application/json; charset=utf-8", json, strlen(json), NULL);
}
static void send_err(int fd, int code, const char *msg, const char *errjson) {
  send_simple(fd, code, msg, "application/json; charset=utf-8", errjson, strlen(errjson), NULL);
}
static void send_index(int fd) {
  char *s = NULL;
  size_t n = 0;
  if (file_read_all(WEBDIR "/index.html", &s, &n) != 0 || n == 0) {
    if (s) free(s);
    send_err(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"页面缺失\"}");
    return;
  }
  send_simple(fd, 200, "OK", "text/html; charset=utf-8", s, n, "Cache-Control: no-cache\r\n");
  free(s);
}
/* 地址栏图标。优先 /data/lx06/web/favicon.ico（热更），没有再回退固件内置。 */
static void send_favicon(int fd) {
  char *s = NULL;
  size_t n = 0;
  if (file_read_all(WEBDIR "/favicon.ico", &s, &n) != 0 || n == 0) {
    if (s) free(s);
    send_simple(fd, 204, "No Content", "text/plain", "", 0, NULL);
    return;
  }
  send_simple(fd, 200, "OK", "image/x-icon", s, n,
              "Cache-Control: no-cache\r\n");
  free(s);
}

/* --- 固件版本（V20261019.5：页脚动态显示，刷机成功与否一眼可辨）---
 * 版本唯一来源 = 镜像根 /lx06-firmware-version 文件（build_ssh_base.sh 按
 * $LX06_FW_VERSION 烧入，只读 squashfs，不可被用户态改动）。文件缺失时
 * 回退编译期常量 LX06_FW_VERSION（build_unified.sh -D 注入），两者都
 * 没有才显示 unknown。免登录可读（与 /api/health 同档）。 */
#ifndef LX06_FW_VERSION
#define LX06_FW_VERSION "unknown"
#endif
static int ota_read_boot_id(char out[37]);
static const char *ota_running_slot(void);
static void h_version(int fd) {
  char *s = NULL;
  size_t n = 0, w = 0, a;
  int from_image = 0;
  char v[128] = "";
  if (file_read_all("/lx06-firmware-version", &s, &n) == 0 && n > 0) {
    for (a = 0; a < n && w < sizeof(v) - 1; a++) {
      char ch = s[a];
      /* 只留安全字符（alnum .-_+）：版本号由构建方写入，天然无空格/引号 */
      if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
          (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '_' || ch == '+')
        v[w++] = ch;
    }
    v[w] = 0;
    if (w > 0) from_image = 1;
  }
  if (s) free(s);
  if (!from_image) snprintf(v, sizeof(v), "%s", LX06_FW_VERSION);
  {
    char out[320], boot_id[37];
    ota_read_boot_id(boot_id);
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"version\":\"%s\",\"source\":\"%s\","
             "\"boot_id\":\"%s\",\"running_slot\":\"%s\"}",
             v, from_image ? "image" : "build", boot_id, ota_running_slot());
    send_simple(fd, 200, "OK", "application/json; charset=utf-8", out, strlen(out),
                "Cache-Control: no-store\r\n");
  }
}

/* --- 请求头 --- */
static const char *hdr_get(const char *head, const char *name, char *out, size_t outsz) {
  const char *p = head, *e = head + strlen(head);
  size_t n = strlen(name);
  out[0] = 0;
  while (p < e) {
    const char *nl = memchr(p, '\n', (size_t)(e - p));
    if (!nl) break;
    if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
      const char *v = p + n + 1;
      while (v < nl && (*v == ' ' || *v == '\t')) v++;
      {
        size_t l = (size_t)(nl - v);
        while (l > 0 && (v[l-1] == '\r' || v[l-1] == ' ')) l--;
        if (l >= outsz) l = outsz - 1;
        memcpy(out, v, l);
        out[l] = 0;
        return out;
      }
    }
    p = nl + 1;
  }
  return out;
}

/* --- 用户应用只通过 keeper；不存在 root 执行或旧 .pid 降级路径。 --- */
static char app_path[512];
static int app_name_safe(const char *name) {
  size_t i;
  if (!name || !*name || name[0] == '.' || strstr(name, "..")) return 0;
  for (i = 0; name[i]; i++) {
    unsigned char c = (unsigned char)name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    if (i >= 64) return 0;
  }
  return 1; /* 编译后的 C/Go/Rust ELF 可没有扩展名。 */
}
static void log_path_for(const char *name, char *out, size_t outsz) {
  snprintf(out, outsz, "%s/%s.log", APPLOGDIR, name);
}
/* --- runlog 链接（V20261018 通用日志入口）：在部署脚本里声明「日志按钮打开哪个文件」。
   约定：部署脚本（sh）任意位置写一行：
       logfile=/data/lx06/logs/xiaozhi.talk.log
   - 值以 / 开头 → 直接用该绝对路径（任意日志文件）；
   - 否则 → APPLOGDIR/<值>.log（约定式 app 日志名）；
   - 未声明 → 默认 APPLOGDIR/<name>.log（该脚本自己的运行日志）。
   webd 直接解析已部署脚本 APPPERSIST/<name>（取第一个合法 logfile= 行），
   前端零改动：豆包/小智/任何新脚本一行即可链接任意日志；脚本把运行输出
   写进该文件时，「日志」按钮即可实时看到。 --- */
#include "app-log-declaration.h"
#include "app-extension.h"
#include "../../runtime/common/script-settings-store.h"
static int app_runlog_path(const char *name, char *out, size_t outsz) {
  char app[340], val[512];
  log_path_for(name, out, outsz);
  snprintf(app, sizeof(app), "%s/%s", APPPERSIST, name);
  FILE *f=fopen(app,"rb");
  if (!f) return 0;
  int rc=app_log_declaration(f,val,sizeof(val));
  fclose(f);
  if (rc<=0) return rc;
  if (strstr(val,"..") || strchr(val,'$')) return -1;
  if (val[0]=='/') {
    if (strlen(val)>=outsz) return -1;
    strcpy(out,val);
  } else {
    if (!app_name_safe(val)) return -1;
    log_path_for(val,out,outsz);
  }
  return 1;
}
/* 卸载清扫（V20261019.8 补）：脚本 logfile= 声明的链接日志文件，若指向
   受管日志目录（LOGDIR / APPLOGDIR）内部，随应用一并删除——否则卸载后
   留下属主为已释放 job UID 的孤儿文件（.166 实证：swtest-linked.log /
   t3-custom.log 卸载后残留）。指向受管目录之外的路径视为业务数据不碰。
   未声明时 app_runlog_path 回落到自身 <name>.log（同样在受管目录，幂等）。
   best-effort：任何失败都不阻塞卸载主流程。 */
static void app_remove_linklog(const char *path) {
  if (!path || !path[0]) return;
  {
    const char *bases[2] = {LOGDIR, APPLOGDIR};
    int i;
    for (i = 0; i < 2; i++) {
      size_t bl = strlen(bases[i]);
      if (strncmp(path, bases[i], bl) == 0 && path[bl] == '/') break;
    }
    if (i >= 2) return; /* 不在受管日志目录内，不碰 */
  }
  {
    LX06_STATBUF st;
    if (LX06_STAT(path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (unlink(path) != 0 && errno != ENOENT) return; /* best-effort */
  }
}
/* 逐级打开可信目录。仅允许 /tmp 作为可写的祖先，不能跟任何符号链接。
 * 不把被普通用户抢占的目录 chown 成可信目录；旧 root .bk 可收紧到 0700。 */
static int app_trusted_dir(const char *path, mode_t mode, int create) {
  char b[700], *save = NULL, *part;
  int fd, next, first = 1;
  if (!path || path[0] != '/' || strlen(path) >= sizeof(b)) { errno = EINVAL; return -1; }
  strcpy(b, path);
  fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  part = strtok_r(b, "/", &save);
  while (part) {
    LX06_STATBUF st;
    char *after = strtok_r(NULL, "/", &save);
    int tmp_ancestor = first && after && strcmp(part, "tmp") == 0;
    if (!strcmp(part, ".") || !strcmp(part, "..")) { close(fd); errno = EINVAL; return -1; }
    next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0 && errno == ENOENT && create) {
      if (mkdirat(fd, part, after ? 0755 : mode) != 0 && errno != EEXIST) { close(fd); return -1; }
      next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    close(fd);
    if (next < 0) return -1;
    fd = next;
    if (LX06_FSTAT(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
        (tmp_ancestor ? (st.st_mode & 07777) != 01777 : (st.st_mode & 0022) != 0)) { close(fd); errno = EPERM; return -1; }
    if (!after && create && fchmod(fd, mode) != 0) { close(fd); return -1; }
    if (!after && !create && (st.st_mode & 0777) != mode) { close(fd); errno = EPERM; return -1; }
    first = 0;
    part = after;
  }
  return fd;
}
static int app_regular(int fd) {
  LX06_STATBUF st;
  if (LX06_FSTAT(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
      st.st_nlink != 1 || (st.st_mode & 0022)) { errno = EPERM; return -1; }
  return 0;
}
static int app_open_file(int dir, const char *name, int flags) {
  int fd = openat(dir, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return -1;
  if (app_regular(fd) != 0) { close(fd); return -1; }
  return fd;
}
/* Management lock is always acquired first, then the existing settings
 * lock. Never call a settings helper or execute an application while holding
 * this borrowed lock. The deployment lock stays held across helper handoff,
 * so another cooperating upload cannot substitute a different script. */
static int app_settings_store=-1,app_settings_lock=-1;
static void app_settings_end(void) {
  int e=errno;
  if(app_settings_lock>=0)close(app_settings_lock);
  if(app_settings_store>=0)close(app_settings_store);
  app_settings_lock=app_settings_store=-1;
  errno=e;
}
static int app_settings_begin(void) {
  if(app_settings_store>=0||app_settings_lock>=0){errno=EBUSY;return -1;}
  app_settings_store=settings_dir(1);
  if(app_settings_store<0)return -1;
  app_settings_lock=settings_lock_acquire(app_settings_store);
  if(app_settings_lock<0){app_settings_end();return -1;}
  return 0;
}
static int app_settings_current(void) {
  return app_settings_store<0?0:settings_lock_current(app_settings_store,app_settings_lock);
}
/* 文件 fsync -> renameat -> 父目录 fsync；失败不承认上传成功。
 * 旧目标也先查 lstat，拒绝 symlink/hardlink/FIFO，临时文件从不复用。 */
static int app_atomic(int dir, const char *name, const char *data, size_t len, mode_t mode) {
  static unsigned seq;
  char tmp[100];
  LX06_STATBUF st;
  int fd = -1, e;
  if (LX06_STATAT(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 || (st.st_mode & 0022)) {
      errno = EPERM; return -1;
    }
  } else if (errno != ENOENT) return -1;
  snprintf(tmp, sizeof(tmp), ".webd-%ld-%u", (long)getpid(), ++seq);
  fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd < 0) return -1;
  if (write_all(fd, data, len) != 0 || fchmod(fd, mode) != 0 || fsync(fd) != 0) goto fail;
  if (close(fd) != 0) { fd = -1; goto fail; }
  fd = -1;
  if (app_settings_current() != 0 || renameat(dir, tmp, dir, name) != 0 || fsync(dir) != 0) goto fail;
  return 0;
fail:
  e = errno;
  if (fd >= 0) close(fd);
  unlinkat(dir, tmp, 0);
  errno = e;
  return -1;
}
/* 部署：从 /tmp 运行区把上传文件分块拷到 /data 持久区，fsync 后原子 rename。
 * 失败清理 /data 内临时文件并保留 errno；tmp 里的上传文件不动（留供排查/重试）。 */
static int app_copy_from_tmp(int dir, const char *name, size_t expect) {
  static unsigned cseq;
  char tmp[100], src[128], chunk[65536];
  LX06_STATBUF st;
  int dsrc, fd = -1, e;
  size_t got = 0;
  if (LX06_STATAT(dir, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 || (st.st_mode & 0022)) {
      errno = EPERM; return -1;
    }
  } else if (errno != ENOENT) return -1;
  snprintf(src, sizeof(src), APPDIR "/%s", name);
  dsrc = open(src, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (dsrc < 0) return -1;
  if (app_regular(dsrc) != 0) { close(dsrc); return -1; }
  snprintf(tmp, sizeof(tmp), ".webd-deploy-%ld-%u", (long)getpid(), ++cseq);
  fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0755);
  if (fd < 0) { e = errno; close(dsrc); errno = e; return -1; }
  for (;;) {
    ssize_t r = read(dsrc, chunk, sizeof(chunk));
    if (r < 0) { e = errno; close(dsrc); goto fail; }
    if (r == 0) break;
    if (got + (size_t)r > expect) { e = EPROTO; close(dsrc); errno = e; goto fail; }
    if (write_all(fd, chunk, (size_t)r) != 0) { e = errno; close(dsrc); goto fail; }
    got += (size_t)r;
  }
  close(dsrc);
  if (got != expect) { e = EPROTO; errno = e; goto fail; }
  if (fchmod(fd, 0755) != 0 || fsync(fd) != 0) { e = errno; close(fd); goto fail; }
  if (close(fd) != 0) { fd = -1; e = errno; goto fail; }
  fd = -1;
  if (app_settings_current() != 0 || renameat(dir, tmp, dir, name) != 0 || fsync(dir) != 0) { e = errno; goto fail; }
  return 0;
fail:
  if (fd >= 0) close(fd);
  unlinkat(dir, tmp, 0);
  errno = e;
  return -1;
}
static int app_meta_dir(const char *name, int create) {
  int root, fd;
  LX06_STATBUF st;
  if (!app_name_safe(name)) { errno = EINVAL; return -1; }
  root = app_trusted_dir(BKDIR, 0700, create);
  if (root < 0) return -1;
  if (create && mkdirat(root, name, 0700) != 0 && errno != EEXIST) { close(root); return -1; }
  fd = openat(root, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (create && fd >= 0 && fsync(root) != 0) { int e = errno; close(fd); close(root); errno = e; return -1; }
  close(root);
  if (fd < 0) return -1;
  if (LX06_FSTAT(fd, &st) != 0 || st.st_uid != 0 || (st.st_mode & 0022) ||
      (create ? fchmod(fd, 0700) != 0 : (st.st_mode & 0777) != 0700)) {
    close(fd); errno = EPERM; return -1;
  }
  return fd;
}
static int app_read_number(int dir, const char *name, long *value) {
  char b[80], *end;
  ssize_t n;
  int fd = app_open_file(dir, name, O_RDONLY);
  if (fd < 0) return -1;
  n = read(fd, b, sizeof(b) - 1);
  close(fd);
  if (n <= 0 || n >= (ssize_t)sizeof(b) - 1 || memchr(b, 0, (size_t)n)) { errno = EINVAL; return -1; }
  b[n] = 0;
  errno = 0;
  *value = strtol(b, &end, 10);
  if (errno || end == b) { errno = EINVAL; return -1; }
  while (*end && isspace((unsigned char)*end)) end++;
  if (*end) { errno = EINVAL; return -1; }
  return 0;
}
/* 不回退 .bk/deploy.sh 或 root 安装器。旧安装须人工迁移；不能靠失败后放宽。 */
static int app_path_ensure(const char *name) {
  int d, fd;
  if (!app_name_safe(name)) { errno = EINVAL; return -1; }
  d = app_trusted_dir(APPPERSIST, 0755, 0);
  if (d < 0) return -1;
  fd = app_open_file(d, name, O_RDONLY);
  close(d);
  if (fd < 0) return -1;
  close(fd);
  snprintf(app_path, sizeof(app_path), "%s/%s", APPPERSIST, name);
  return 0;
}
static struct lk_reply keeper_cached;
static struct lk_reply keeper_last;   /* 最近一次成功快照：daemon 瞬断时 UI 沿用，不误报安全模式 */
static int keeper_ever_ok;
static int64_t keeper_cached_until;
static int keeper_cached_ok, keeper_batch;
static void keeper_invalidate(void) { keeper_cached_until = 0; keeper_cached_ok = 0; }
/* 成功和不可用都缓存最多 250ms；一次列表共享一份快照，缺失不沿用旧 running。
 * 瞬断（如 load 尖峰/lk_call 500ms 超时）时回退 keeper_last 的 safe/reason，
 * 避免 UI 误显 safe_mode:1（曾致用户误判「又进安全模式」）。从未成功过才报 1（failsafe）。 */
static int keeper_status_read(int force) {
  struct lk_request q;
  int64_t now = lk_now_ms();
  if (!force && (keeper_batch || (now >= 0 && now < keeper_cached_until))) return keeper_cached_ok;
  memset(&q, 0, sizeof(q));
  q.magic = LK_MAGIC; q.action = LK_STATUS;
  if (lk_call(&q, &keeper_cached, 500) == 0) {
    keeper_cached_ok = 1;
    keeper_last = keeper_cached;
    keeper_ever_ok = 1;
  } else {
    keeper_cached_ok = 0;
    keeper_cached = keeper_last;
  }
  now = lk_now_ms();
  keeper_cached_until = now < 0 ? 0 : now + 250;
  return keeper_cached_ok;
}
/* 1=keeper 仍保有活动/停止中的 job，首轮扫描前 members=0 也不能误报退出；
 * 包括 leader 已退、setsid 孙进程仍活，只有 daemon 停净后移除 job 才算停止。
 * 0=daemon 记录 rc=0；2=异常/未知退出；-1=从未受管运行。旧 .pid/.st 不参与。 */
static int app_status(const char *name) {
  unsigned i;
  int d, fd, rc_ok;
  long rc;
  if (!app_name_safe(name)) return -1;
  if (keeper_status_read(0)) {
    for (i = 0; i < keeper_cached.jobs; i++)
      if (strncmp(keeper_cached.job[i].name, name, sizeof(keeper_cached.job[i].name)) == 0) return 1;
  }
  d = app_meta_dir(name, 0);
  if (d < 0) return -1;
  rc_ok = app_read_number(d, ".runrc", &rc) == 0;
  fd = app_open_file(d, "runid", O_RDONLY);
  close(d);
  if (fd >= 0) close(fd);
  if (rc_ok) return rc == 0 ? 0 : 2;
  return fd >= 0 ? 2 : -1;
}
static int app_running(const char *name) { return app_status(name) == 1; }
#define APP_RESIDENT (1U << 16) /* 仅登录后显式 profile；不进入 wire flags。 */
#define APP_RUN_ROOT (1U << 17)   /* 脚本标记 # LX06-RUN: root（或 isroot=1）→ root 运行 */
#define APP_INSTALL_ROOT (1U << 18) /* 脚本标记 # LX06-INSTALL: root → root 安装（部署执行） */
#define APP_LOCAL_WAKEWORD (1U << 19) /* 脚本标记 # LX06-LOCALWAKEUP: on → 可被唤醒词直连（--wakeup 参数） */
#define APP_DECLARED_RESIDENT (1U << 21) /* # LX06-RESIDENT: on; display metadata, never starts a job */
#define APP_SESSION_MANAGED (1U << 20) /* session-run owns deadline and cleanup */
#define APP_COPY_CODES (1U << 22) /* # LX06-COPY-CODES: on: script opts into copy UI */
#define APP_OFFICIAL_WAKEUP (1U << 23) /* # LX06-OFFICIALWAKEUP: on → 允许被「小爱同学」绑定 */
#define APP_LOCAL_ADMISSION (1U << 24) /* Script passes local admission to session-run. */
#include "../xiaoai/script-audio-mode.h"
/* 部署脚本自声明 root 身份（通用，不针对任何具体应用）：扫描上传内容前 8KB，
 * 整行匹配（允许行尾空白/\r）：
 *   # LX06-RUN: root      → APP_RUN_ROOT（运行不独立身份，keeper LK_ROOT）
 *   # LX06-INSTALL: root  → APP_INSTALL_ROOT（安装/部署执行以 root）
 *   isroot=1（可带 # 前缀）→ 两者（用户定案的简写参数）
 * 置位语义（四模式正交，用户定案）：上传 run=1（首次=安装阶段）任一置位
 * → LK_ROOT；手动运行/开机常驻行仅 RUN 置位 → LK_ROOT（INSTALL 单独置位
 * = root 安装 + 非 root 运行）；无标记 → 独立保留 UID（原行为，全兼容）。 */
static unsigned app_marker_flags(const unsigned char *p, size_t n) {
  unsigned f = 0;
  size_t i = 0, L, k;
  if (n > 8192) n = 8192;
  while (i < n) {
    /* pread's header need not have a trailing NUL. Bound the scan before
     * examining markers; a full 8 KiB header must never read past its buffer. */
    L = 0;
    while (i + L < n && p[i + L] && p[i + L] != '\n') L++;
    k = L;
    while (k > 0 && (p[i + k - 1] == ' ' || p[i + k - 1] == '\t' ||
                     p[i + k - 1] == '\r'))
      k--;
    if (k >= 2 && p[i] == '#' && p[i + 1] == ' ') {
      size_t s = i + 2, v = k - 2;
      while (v > 0 && p[s] == ' ') { s++; v--; }
      if (v == 18 && !memcmp(p + s, "LX06-INSTALL: root", 18))
        f |= APP_INSTALL_ROOT;
      else if (v == 20 && !memcmp(p + s, "LX06-LOCALWAKEUP: on", 20))
        f |= APP_LOCAL_WAKEWORD;
      else if (v == 23 && !memcmp(p + s, "LX06-OFFICIALWAKEUP: on", 23))
        f |= APP_OFFICIAL_WAKEUP;
      else if (v == sizeof("LX06-RESIDENT: on")-1 && !memcmp(p + s, "LX06-RESIDENT: on", sizeof("LX06-RESIDENT: on")-1))
        f |= APP_DECLARED_RESIDENT;
      else if (v == sizeof("LX06-COPY-CODES: on")-1 && !memcmp(p+s,"LX06-COPY-CODES: on",sizeof("LX06-COPY-CODES: on")-1))
        f |= APP_COPY_CODES;
      else if (v == 24 && !memcmp(p + s, "LX06-SESSION-MANAGED: on", 24))
        f |= APP_SESSION_MANAGED;
      else if (v == sizeof("LX06-LOCAL-ADMISSION: epoch-v1")-1 &&
               !memcmp(p + s, "LX06-LOCAL-ADMISSION: epoch-v1", v))
        f |= APP_LOCAL_ADMISSION;
      else if (v == 14 && !memcmp(p + s, "LX06-RUN: root", 14))
        f |= APP_RUN_ROOT;
      else if (v == 8 && !memcmp(p + s, "isroot=1", 8))
        f |= APP_RUN_ROOT | APP_INSTALL_ROOT;
    } else if (k == 8 && !memcmp(p + i, "isroot=1", 8)) {
      f |= APP_RUN_ROOT | APP_INSTALL_ROOT;
    }
    i += L;
    if (i >= n) break;
    if (p[i] == '\0') break;   /* NUL=数据边界（调用方可能传过大的 n） */
    i++;                       /* 跳过 '\n' */
  }
  return f;
}
/* 读已部署应用文件头 → root 标记（文件在 = 已部署；读失败按无标记）。 */
static unsigned app_file_marker_flags(const char *name) {
  unsigned char hd[8192];
  int d, f;
  ssize_t n;
  d = app_trusted_dir(APPPERSIST, 0755, 0);
  if (d < 0) return 0;
  f = app_open_file(d, name, O_RDONLY);
  close(d);
  if (f < 0) return 0;
  n = pread(f, hd, sizeof(hd), 0);
  close(f);
  if (n <= 0) return 0;
  return app_marker_flags(hd, (size_t)n);
}
/* Phone is an explicit saved-script mode, never inferred from --wakeup.
 * This is routing/UI metadata only; the normal native receiver rematches it. */
static int app_phone_sender(const char *name) {
  unsigned char hd[8192];
  int d=app_trusted_dir(APPPERSIST,0755,0);
  if(d<0)return -1;
  int f=app_open_file(d,name,O_RDONLY);close(d);
  if(f<0)return -1;
  ssize_t n=pread(f,hd,sizeof hd,0);close(f);
  if(n<=0)return -1;
  struct lx06_script_audio_metadata m=lx06_script_audio_parse(hd,(size_t)n);
  if(m.mode==LX06_AUDIO_INVALID)return -1;
  if(m.mode!=LX06_AUDIO_PHONE_BRIDGE)return 0;
  return m.phone_app==LX06_PHONE_DOUBAO?1:-1;
}
/* run 失败埋点：全部失败分支落 webd.log（unified 父进程 fd1/2 → webd.log），
 * 现场可经 :88 /api/logs?type=webd 读回。13:31 四连 errno=2 无日志可查，故加。 */
#define RUNLOG(nm, st) do { fprintf(stderr, "webd run %s @%s: %s\n", (nm), (st), strerror(errno)); fflush(stderr); } while (0)
static long run_app_profile(const char *name, const char *args, unsigned flags) {
  if(!lx06_scripts_allowed()) {errno=EPERM;return -1;}
  struct lk_request q;
  struct lk_reply r;
  char key[90], dv[8];
  int d, fd;
  unsigned char magic[4];
  size_t n;
  if (flags & ~(LK_WIDE_VM | LK_ROOT | APP_RESIDENT | APP_LOCAL_ADMISSION)) { errno = EINVAL; RUNLOG(name, "bad_flags"); return -1; }
  if (app_path_ensure(name) != 0) { RUNLOG(name, "path_ensure"); return -1; }
  memset(&q, 0, sizeof(q));
  q.magic = LK_MAGIC; q.action = LK_START; q.flags = flags & (LK_WIDE_VM | LK_ROOT);
  q.timeout_sec = ((flags & APP_RESIDENT) || (app_file_marker_flags(name) & APP_SESSION_MANAGED)) ? 0 : 600;
  strcpy(q.name, name);
  n = strlen(name);
  /* 统一启动（与应用形态无关，名称不限 .sh）：读文件头判定执行方式——
   * ELF / "#!" shebang 脚本 → 直接执行（keeper 以该 job UID exec，内核解析
   * shebang）；其余文本 → /bin/sh 解释。与 init-launch.sh 开机常驻同一规则。 */
  d = app_trusted_dir(APPPERSIST, 0755, 0);
  if (d < 0) { RUNLOG(name, "trusted_dir"); return -1; }
  fd = app_open_file(d, name, O_RDONLY);
  if (fd < 0) { RUNLOG(name, "open_file"); close(d); return -1; }
  n = (size_t)pread(fd, magic, sizeof(magic), 0);
  close(fd);  /* 旧版只关了目录泄漏文件 fd（/proc/<pid>/fd 实测累计增长） */
  close(d);
  if (n != sizeof(magic)) { errno = ENOEXEC; RUNLOG(name, "magic_short"); return -1; }
  if (!(memcmp(magic, "\177ELF", sizeof(magic)) == 0 || memcmp(magic, "#!", 2) == 0))
    strcpy(q.argv[q.argc++], "/bin/sh");
  if (strlen(app_path) >= LK_ARG_LEN) { errno = ENAMETOOLONG; RUNLOG(name, "nametoolong"); return -1; }
  strcpy(q.argv[q.argc++], app_path);
  if (lk_split_args(args, &q) != 0) { RUNLOG(name, "split_args"); return -1; }
  snprintf(key, sizeof(key), "log_app_%s", name);
  if (conf_get(CONF, key, dv, sizeof(dv)) == 0 && dv[0] == '0') q.flags |= LK_LOG_OFF;
  /* 开始新一轮：若同名 job 仍活动（常驻栈：daemon 以该 job 成员常驻，开机由
   * init-launch.sh 直接以同名 job 启动），先停净其整个 UID 树再开新一轮。
   * 新旧 job 同名 → 同槽位同 UID → 新 job 可更新旧文件、停旧进程、清旧锁。 */
  if (!(flags & APP_LOCAL_ADMISSION) && keeper_status_read(1)) {
    unsigned i;
    for (i = 0; i < keeper_cached.jobs; i++)
      if (!strncmp(keeper_cached.job[i].name, name, sizeof(keeper_cached.job[i].name))) {
        struct lk_request qs; struct lk_reply rs;
        int k, busy, m2;
        memset(&qs, 0, sizeof(qs));
        qs.magic = LK_MAGIC; qs.action = LK_STOP;
        strcpy(qs.name, name);
        keeper_invalidate();
        if (lk_call(&qs, &rs, 500) != 0) { errno = EPROTO; RUNLOG(name, "stop_old_call"); return -1; }
        busy = 1;
        for (k = 0; k < 200 && busy; k++) {
          usleep(50 * 1000);
          keeper_invalidate();
          if (!keeper_status_read(1)) { errno = EPROTO; RUNLOG(name, "status_after_stop"); return -1; }
          busy = 0;
          for (m2 = 0; m2 < keeper_cached.jobs; m2++)
            if (!strncmp(keeper_cached.job[m2].name, name,
                        sizeof(keeper_cached.job[m2].name))) { busy = 1; break; }
        }
        if (busy) { errno = ETIMEDOUT; RUNLOG(name, "stop_timeout"); return -1; }
        break;
      }
  }
  if (!keeper_status_read(1)) { RUNLOG(name, "status_pre_start"); return -1; } /* 停完旧 job 后必须能读到状态再放行新一轮 */
  {
    unsigned i;
    char id[80];
    for (i = 0; i < keeper_cached.jobs; i++)
      if (!strncmp(keeper_cached.job[i].name, name, sizeof(keeper_cached.job[i].name))) { errno = EBUSY; RUNLOG(name, "already_active"); return -1; }
    d = app_meta_dir(name, 1);
    if (d < 0) { RUNLOG(name, "meta_dir"); return -1; }
    snprintf(id, sizeof(id), "%ld-%lld\n", (long)getpid(), (long long)lk_now_ms());
    if (app_atomic(d, "runid", id, strlen(id), 0600) != 0 ||
        (unlinkat(d, ".runrc", 0) != 0 && errno != ENOENT) || fsync(d) != 0) {
      int e = errno; RUNLOG(name, "runid_write"); close(d); errno = e; return -1;
    }
    close(d);
  }
  keeper_invalidate();
  if (lk_call(&q, &r, 500) != 0) { RUNLOG(name, "lk_start_call"); return -1; }
  if (r.pid <= 0) { errno = EPROTO; RUNLOG(name, "no_pid"); return -1; }
  return (long)r.pid;
}
static int kill_app(const char *name) {
  struct lk_request q;
  struct lk_reply r;
  if (!app_name_safe(name)) { errno = EINVAL; return -1; }
  memset(&q, 0, sizeof(q));
  q.magic = LK_MAGIC; q.action = LK_STOP;
  strcpy(q.name, name);
  keeper_invalidate();
  return lk_call(&q, &r, 500); /* EBUSY/超时保留全部记录，由前端重试。 */
}

/* ---- 部署顺序表：/data/lx06/.appmeta（每行 "name|epoch"，行序=部署序）。
   持久（重启/恢复出厂后随 /data 生命周期）；「部署日期」显示与 LIFO 卸载
   （只能从最后部署的往前）都以此为序。不用文件 mtime：glibc 头→2.25 库的
   stat 布局桥只验证过 st_size，mtime 风险不可控；顺序表自维护、零风险。 */
#define APPMETA "/data/lx06/.appmeta"
/* 读整表（≤16KB）。返回长度；-1 无文件 */
static long appmeta_read_all(char *buf, size_t bufsz) {
  int d = app_trusted_dir("/data/lx06", 0755, 0), fd;
  ssize_t n;
  if (d < 0) return -1;
  fd = app_open_file(d, ".appmeta", O_RDONLY); close(d);
  if (fd < 0) return -1;
  n = read(fd, buf, bufsz - 1); close(fd);
  if (n < 0) return -1;
  if ((size_t)n == bufsz - 1) { errno = E2BIG; return -1; }
  buf[n] = 0;
  return (long)n;
}
/* 表内条目数（有效行） */
static int appmeta_count(void) {
  char buf[16384];
  long n = appmeta_read_all(buf, sizeof(buf));
  int c = 0;
  long i;
  if (n < 0) return 0;
  for (i = 0; i < n; i++) {
    if (i > 0 && buf[i - 1] != '\n') continue;
    if (buf[i] == 0 || buf[i] == '\n') continue;
    c++;
  }
  return c;
}
/* 取第 idx 行：name / epoch。0=成功 */
static int appmeta_get(int idx, char *name, size_t namesz, long *mt) {
  char buf[16384];
  long n = appmeta_read_all(buf, sizeof(buf));
  int c = -1;
  size_t i, L;
  if (n < 0) return -1;
  for (i = 0; i < (size_t)n; i++) {
    if (i > 0 && buf[i - 1] != '\n') continue;
    if (buf[i] == 0 || buf[i] == '\n') continue;
    c++;
    if (c != idx) continue;
    L = strcspn(buf + i, "\n");
    {
      size_t bar = strcspn(buf + i, "|");
      size_t nl = bar < L ? bar : L;
      size_t k = nl;
      if (k >= namesz) { errno = E2BIG; return -1; }
      memcpy(name, buf + i, k);
      name[k] = 0;
      if (nl < L) *mt = atol(buf + i + nl + 1);
      else *mt = 0;
    }
    return 0;
  }
  return -1;
}
static int appmeta_write(const char *data, size_t n) {
  int d = app_trusted_dir("/data/lx06", 0755, 1), result;
  if (d < 0) return -1;
  result = app_atomic(d, ".appmeta", data, n, 0600);
  close(d); return result;
}
/* 追加（同名先删旧行 → 重部署刷新日期与位置到末尾） */
static int appmeta_append(const char *name) {
  char buf[16384], out[16384];
  long n = appmeta_read_all(buf, sizeof(buf));
  size_t i, L, w = 0;
  long now = (long)time(NULL);
  if (n < 0) { if (errno != ENOENT) return -1; n = 0; }
  L = (size_t)n;
  while (L > 0 && (buf[L - 1] == '\n' || buf[L - 1] == 0)) L--;
  /* 行首遍历（i==0 或前一是 \n），strcspn 从行首算 | 位置。
     旧 bug：i 停在换行符处（行尾），strcspn 恒 0 → 旧行内容全丢、日期变 0。 */
  for (i = 0; i < L; i++) {
    if (i > 0 && buf[i - 1] != '\n') continue;
    {
      size_t ls = strcspn(buf + i, "\n");
      size_t bar = strcspn(buf + i, "|");
      if (bar < ls && bar == strlen(name) &&
          strncmp(buf + i, name, bar) == 0)
        continue;                       /* 同名旧行：跳过（刷新到末尾） */
      if (w + ls + 2 > sizeof(out) - 128) { errno = E2BIG; return -1; }
      memcpy(out + w, buf + i, ls);
      w += ls;
      out[w++] = '\n';
    }
  }
  {
    int wlen = snprintf(out + w, sizeof(out) - w, "%s|%ld\n", name, now);
    if (wlen < 0 || (size_t)wlen >= sizeof(out) - w) return -1;
    w += (size_t)wlen;
    if (appmeta_write(out, w) != 0) return -1;
  }
  return 0;
}
/* 删一行（卸载时） */
static int appmeta_remove(const char *name) {
  char buf[16384], out[16384];
  long n = appmeta_read_all(buf, sizeof(buf));
  size_t i, L, w = 0;
  if (n < 0) return errno == ENOENT ? 0 : -1;
  L = (size_t)n;
  while (L > 0 && (buf[L - 1] == '\n' || buf[L - 1] == 0)) L--;
  /* 行首遍历（i==0 或前一是 \n）。旧 bug：i 停在换行符处，strcspn 恒 0
     → 保留的行内容全丢成空行。 */
  for (i = 0; i < L; i++) {
    if (i > 0 && buf[i - 1] != '\n') continue;
    {
      size_t ls = strcspn(buf + i, "\n");
      size_t bar = strcspn(buf + i, "|");
      if (bar < ls && bar == strlen(name) &&
          strncmp(buf + i, name, bar) == 0)
        continue;                       /* 命中行：删掉 */
      if (w + ls + 2 > sizeof(out) - 16) { errno = E2BIG; return -1; }
      memcpy(out + w, buf + i, ls);
      w += ls;
      out[w++] = '\n';
    }
  }
  {
    if (appmeta_write(out, w) != 0) return -1;
  }
  return 0;
}

/* 新模式没有共享 /data 安装事务。锁仅保护管理端登记，不按时间猜测成功。
 * keeper 不可读时保留锁；不自动恢复、回滚、删除故障脚本或执行用户清理命令。 */
#define LOCKFILE "/data/lx06/.deploy.lock"
static int deploy_lock_active(char *holder, size_t hs) {
  int d, fd;
  char b[128];
  ssize_t n;
  d = app_trusted_dir(APPDATA, 0755, 0);
  if (d < 0) { snprintf(holder, hs, "metadata-unavailable"); return 1; }
  fd = app_open_file(d, ".deploy.lock", O_RDONLY); close(d);
  if (fd < 0) {
    if (errno == ENOENT) return 0;
    snprintf(holder, hs, "unsafe-lock"); return 1;
  }
  n = read(fd, b, sizeof(b) - 1); close(fd);
  if (n <= 0) { snprintf(holder, hs, "invalid-lock"); return 1; }
  b[n] = 0; b[strcspn(b, "\r\n")] = 0;
  if (!app_name_safe(b)) { snprintf(holder, hs, "invalid-lock"); return 1; }
  snprintf(holder, hs, "%s", b);
  return 1;
}
static int deploy_lock_take(const char *name) {
  int d, fd, e = 0;
  char b[80];
  d = app_trusted_dir("/data/lx06", 0755, 1);
  if (d < 0) return -1;
  fd = openat(d, ".deploy.lock", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) { close(d); return -1; }
  snprintf(b, sizeof(b), "%s\n", name);
  if (write_all(fd, b, strlen(b)) != 0 || fsync(fd) != 0) e = errno;
  if (close(fd) != 0 && !e) e = errno;
  if (!e && fsync(d) != 0) e = errno;
  if (e) unlinkat(d, ".deploy.lock", 0);
  close(d);
  if (e) { errno = e; return -1; }
  return 0;
}
static void deploy_lock_release(const char *name) {
  char holder[128];
  int d;
  if (!deploy_lock_active(holder, sizeof(holder)) || strcmp(holder, name) != 0) return;
  d = app_trusted_dir("/data/lx06", 0755, 0);
  if (d >= 0) { unlinkat(d, ".deploy.lock", 0); (void)fsync(d); close(d); }
}
static int deploy_rollback(const char *name, const char *reason) {
  (void)reason;
  if (kill_app(name) != 0) return -1;
  deploy_lock_release(name);
  return 0; /* 仅停止：保留脚本/日志供排查，不还原全局业务文件。 */
}
static void deploy_watchdog(void) {
  char holder[128];
  unsigned i;
  if (!deploy_lock_active(holder, sizeof(holder)) || !app_name_safe(holder)) return;
  if (!keeper_status_read(0)) return;
  for (i = 0; i < keeper_cached.jobs; i++)
    if (!strncmp(keeper_cached.job[i].name, holder, sizeof(keeper_cached.job[i].name))) return;
  deploy_lock_release(holder); /* 只收尾放锁；绝不按 runid 年龄猜重启/成功。 */
}

/* --- JSON 字段小工具：取 body 里 "key" 后的字符串值（受控客户端） --- */
static void json_str(const char *body, const char *key, char *out, int outsz) {
  const char *p;
  char kp[40];
  size_t kl = strlen(key);
  int i;
  out[0] = 0;
  if (kl >= sizeof(kp)) return;
  kp[0] = '"'; memcpy(kp + 1, key, kl); kp[kl+1] = '"'; kp[kl+2] = 0;
  p = strstr(body, kp);
  if (!p) return;
  p += kl + 2;
  while (*p && *p != ':') p++;
  if (*p != ':') return;
  p++;
  while (*p && (*p == ' ' || *p == '\t')) p++;
  if (*p != '"') return;
  p++;
  for (i = 0; p[i] && p[i] != '"' && p[i] != '\\' && i < outsz - 1; i++) out[i] = p[i];
  out[i] = 0;
}
static int app_json_hex(const char **p, unsigned *value) {
  unsigned v = 0, i;
  for (i = 0; i < 4; i++) {
    unsigned char c = (unsigned char)*(*p)++;
    if (c >= '0' && c <= '9') v = v * 16 + c - '0';
    else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
    else return -1;
  }
  *value = v; return 0;
}
static int app_json_value(const char **input, char *out, size_t cap) {
  const char *p = *input;
  size_t n = 0;
  /* 数字字面量（rec-del n / record test 等前端以 JSON 数字发送的字段）：
   * 原样拷贝到 out；字符串解析逻辑不变。 */
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    const char *q = p;
    if (*q == '-') q++;
    if (!(*q >= '0' && *q <= '9')) return -1;
    while (*q >= '0' && *q <= '9') q++;
    if (*q == '.') {
      q++;
      if (!(*q >= '0' && *q <= '9')) return -1;
      while (*q >= '0' && *q <= '9') q++;
    }
    {
      size_t tl = (size_t)(q - p);
      if (out && tl + 1 > cap) { errno = E2BIG; return -1; }
      if (out) { memcpy(out, p, tl); out[tl] = 0; }
    }
    *input = q;
    return 0;
  }
  if (*p++ != '"') return -1;
  while (*p && *p != '"') {
    unsigned u = (unsigned char)*p++;
    char bytes[4];
    size_t count = 1, i;
    if (u < 32) return -1;
    if (u == '\\') {
      char c = *p++;
      if (!c) return -1;
      switch (c) {
        case '"': case '\\': case '/': u = (unsigned char)c; break;
        case 'b': u = '\b'; break;
        case 'f': u = '\f'; break;
        case 'n': u = '\n'; break;
        case 'r': u = '\r'; break;
        case 't': u = '\t'; break;
        case 'u': {
          unsigned low;
          if (strlen(p) < 4 || app_json_hex(&p, &u) != 0 || !u) return -1;
          if (u >= 0xd800 && u <= 0xdbff) {
            if (p[0] != '\\' || p[1] != 'u') return -1;
            p += 2;
            if (strlen(p) < 4 || app_json_hex(&p, &low) != 0 || low < 0xdc00 || low > 0xdfff) return -1;
            u = 0x10000 + ((u - 0xd800) << 10) + low - 0xdc00;
          } else if (u >= 0xdc00 && u <= 0xdfff) return -1;
          if (u >= 0x10000) { bytes[0] = (char)(0xf0 | (u >> 18)); count = 4; }
          else if (u >= 0x800) { bytes[0] = (char)(0xe0 | (u >> 12)); count = 3; }
          else if (u >= 0x80) { bytes[0] = (char)(0xc0 | (u >> 6)); count = 2; }
          if (count > 1) {
            for (i = 1; i < count; i++) bytes[i] = (char)(0x80 | ((u >> (6 * (count - 1 - i))) & 63));
          }
          break;
        }
        default: return -1;
      }
    }
    if (count == 1) bytes[0] = (char)u;
    if (out) {
      if (n + count >= cap) { errno = E2BIG; return -1; }
      memcpy(out + n, bytes, count);
    }
    n += count;
  }
  if (*p != '"') return -1;
  if (out) out[n] = 0;
  *input = p + 1; return 0;
}
/* 只接受完整的字符串字段对象；重复字段/非字符串/非法转义/超长均拒绝。
 * 忽略未知字符串字段，以保留客户端扩展兼容；profile 的值另作白名单。 */
static int app_json_strx(const char *body, const char *key, char *out, size_t cap, int *found) {
  const char *p = body;
  int f = 0;
  char k[80];
  out[0] = 0;
  if (found) *found = 0;
  while (isspace((unsigned char)*p)) p++;
  if (*p++ != '{') return -1;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '}') for (;;) {
    int wanted;
    if (app_json_value(&p, k, sizeof(k)) != 0) return -1;
    wanted = strcmp(k, key) == 0;
    if (wanted && f++) return -1;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != ':') return -1;
    while (isspace((unsigned char)*p)) p++;
    if (app_json_value(&p, wanted ? out : NULL, wanted ? cap : 0) != 0) return -1;
    if (wanted && found) *found = 1;
    while (isspace((unsigned char)*p)) p++;
    if (*p != ',') break;
    p++;
    while (isspace((unsigned char)*p)) p++;
  }
  if (*p++ != '}') return -1;
  while (isspace((unsigned char)*p)) p++;
  return *p ? -1 : 0;
}
/* 原 app_json_str：只报解析成败，不区分键缺失（缺=解析成功但 out 空）。
 * 可选参数必须用 app_json_strx 看 found，否则「缺键」会被当成「空值」。 */
static int app_json_str(const char *body, const char *key, char *out, size_t cap) {
  return app_json_strx(body, key, out, cap, NULL);
}
static void app_http_error(int fd, int code, const char *message) {
  char body[600];
  int e = errno;
  snprintf(body, sizeof(body), "{\"ok\":false,\"running\":0,\"errno\":%d,\"error\":\"%s\"}", e, message);
  send_err(fd, code, code == 409 ? "Conflict" : code == 400 ? "Bad Request" : "Service Unavailable", body);
}

/* 自动启动仅是显式管理开关：缺文件默认开；其他内容只有去尾换行后恰为 "on" 才开。
 * GET 不创建目录/不修状态；写路径与 keeper 一样只接受可信 root 管理目录。
 * 与 /usr/lib/lx06/init-launch.sh 的铁律门控同一语义（去尾换行后恰为 on 才开）：
 * UI 显示恒等于开机行为。2026-10-19 真机教训：历史 2 字节 "on"（无尾换行）
 * 被旧版（要求精确 "on\n"）读成关、init-launch 读成开 → UI 显示未开启但
 * 开机仍拉起用户脚本。旧版文件由 webd 启动自愈/下次 POST 规范化为 "on\n"。 */
static int autostart_read(void) { return lx06_scripts_allowed(); }
/* JSON 只允许一个数值型 on=0/1；不接受字符串、布尔值、浮点数或重复字段。 */
static int autostart_parse(const char *body, size_t len, int *on) {
  const char *p = body;
  char key[8];
  if (!body || strlen(body) != len) return -1;
  while (isspace((unsigned char)*p)) p++;
  if (*p++ != '{') return -1;
  while (isspace((unsigned char)*p)) p++;
  if (app_json_value(&p, key, sizeof(key)) != 0 || strcmp(key, "on")) return -1;
  while (isspace((unsigned char)*p)) p++;
  if (*p++ != ':') return -1;
  while (isspace((unsigned char)*p)) p++;
  if (*p != '0' && *p != '1') return -1;
  *on = *p++ - '0';
  while (isspace((unsigned char)*p)) p++;
  if (*p++ != '}') return -1;
  while (isspace((unsigned char)*p)) p++;
  return *p ? -1 : 0;
}
static int autostart_write(int on) {
  const char *value = on ? "on\n" : "off\n";
  const char *names[] = {"autostart", ".autostart.tmp"};
  int d = -1, f = -1, e, made = 0;
  unsigned i;
  LX06_STATBUF st;
  if (geteuid() != 0) { errno = EPERM; return -1; }
  d = app_trusted_dir("/data/lx06", 0755, 1);
  if (d < 0) return -1;
  if (fchown(d, 0, 0) != 0) goto fail;
  /* 固定临时名也不能跟链接/复用不可信 inode；仅回收可信的掉电残留临时文件。 */
  for (i = 0; i < 2; i++) {
    if (LX06_STATAT(d, names[i], &st, AT_SYMLINK_NOFOLLOW) == 0) {
      if (!S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 || (st.st_mode & 0022)) { errno = EPERM; goto fail; }
      if (i && unlinkat(d, names[i], 0) != 0) goto fail;
    } else if (errno != ENOENT) goto fail;
  }
  f = openat(d, ".autostart.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (f < 0) goto fail;
  made = 1;
  if (fchown(f, 0, 0) != 0 || fchmod(f, 0644) != 0 ||
      write_all(f, value, strlen(value)) != 0 || fsync(f) != 0) goto fail;
  if (close(f) != 0) { f = -1; goto fail; }
  f = -1;
  if (renameat(d, ".autostart.tmp", d, "autostart") != 0 || fsync(d) != 0) goto fail;
  close(d); return 0;
fail:
  e = errno;
  if (f >= 0) close(f);
  if (d >= 0) { if (made) unlinkat(d, ".autostart.tmp", 0); close(d); }
  errno = e; return -1;
}
static void h_autostart(int fd, const char *body, size_t len) {
  int on;
  if (body) {
    if (autostart_parse(body, len, &on) != 0) { errno = EINVAL; app_http_error(fd, 400, "JSON on必须为数值0或1"); return; }
    if (autostart_write(on) != 0) { app_http_error(fd, 503, "自动启动开关持久写入失败，未报告成功"); return; }
    if(!on) {
      struct lk_request q={.magic=LK_MAGIC,.action=LK_STOP}; struct lk_reply r;
      /* Gate is already durable. Stop active jobs; keeper also enforces this
       * continuously, including if the Web request is interrupted. */
      (void)lk_call(&q,&r,800);
    }
  } else on = autostart_read();
  send_json(fd, on ? "{\"ok\":true,\"on\":1}" : "{\"ok\":true,\"on\":0}");
}

/* ---- 日志传送配置 /api/logconf（WebAdmin 隐藏弹窗：X×5 连点，用户定案）。
   与 /api/password 同一原理：普通文件 /data/lx06-log.conf，无守护进程依赖；
   GET 返回当前值（文件缺失时 ip/port 为空 = 未配置），POST 校验后
   tmp+rename+fsync 原子持久化。看门狗 reboot -f 前现读该文件发送系统日志，
   Web 上保存后立即生效（无需重启）。88/89 都正常时不发送（看门狗只在
   双死重启路径上调用）。字段：ip（点分四段，允许空=不发送）、port
   （1-65535）、on（0/1）、kb（8-1024，缺省 128）。 ---- */
static int logconf_ip_valid(const char *s) {
  int octet[4], seg = 0, n = 0;
  size_t i, L = strlen(s);
  if (L == 0) return 1;                 /* 空 = 清除目标（不发送） */
  if (L < 7 || L > 15) return 0;
  octet[0] = octet[1] = octet[2] = octet[3] = 0;
  for (i = 0; i <= L; i++) {
    int last = (i == L);
    char ch = last ? '.' : s[i];
    if (ch >= '0' && ch <= '9') {
      if (seg > 3 || n >= 3) return 0;   /* 段越界 / 每段最多 3 位 */
      octet[seg] = octet[seg] * 10 + (ch - '0');
      n++;
    } else if (ch == '.') {
      if (n == 0) return 0;              /* 首点/空段/双点 */
      if (octet[seg] > 255) return 0;
      if (last) break;                   /* 最后一段收尾 */
      seg++;
      n = 0;
      if (seg > 3) return 0;             /* 超过 4 段 */
    } else return 0;
  }
  return (seg == 3 && n > 0) ? 1 : 0;
}
static int logconf_port_valid(const char *s, int *out) {
  long v;
  const char *p = s;
  if (!*p) return 0;
  v = 0;
  for (; *p; p++) {
    if (*p < '0' || *p > '9') return 0;
    v = v * 10 + (*p - '0');
    if (v > 65535) return 0;
  }
  if (v < 1) return 0;
  *out = (int)v;
  return 1;
}
static void h_logconf(int fd, const char *body, size_t len) {
  char ip[40], port[16], en[8], kb[16], dv[64];
  int on, port_i, kb_i;
  (void)len;
  if (!body || len == 0) {
    char out[512];
    int n;
    on = 1; kb_i = 128;
    ip[0] = 0; port[0] = 0;
    if (conf_get(LOGCONF_PATH, "log_server_ip", ip, sizeof(ip)) != 0) ip[0] = 0;
    if (conf_get(LOGCONF_PATH, "log_server_port", port, sizeof(port)) != 0) port[0] = 0;
    if (conf_get(LOGCONF_PATH, "log_enabled", dv, sizeof(dv)) == 0 && dv[0] == '0') on = 0;
    if (conf_get(LOGCONF_PATH, "log_max_kb", dv, sizeof(dv)) == 0) {
      long x = atol(dv);
      if (x >= 8 && x <= 1024) kb_i = (int)x;
    }
    n = snprintf(out, sizeof(out),
                 "{\"ok\":true,\"ip\":\"%s\",\"port\":\"%s\",\"on\":%d,\"kb\":%d}",
                 ip, port, on, kb_i);
    send_simple(fd, 200, "OK", "application/json; charset=utf-8", out, (size_t)n, NULL);
    return;
  }
  json_str(body, "ip", ip, sizeof(ip));
  json_str(body, "port", port, sizeof(port));
  json_str(body, "on", en, sizeof(en));
  json_str(body, "kb", kb, sizeof(kb));
  if (!logconf_ip_valid(ip)) { app_http_error(fd, 400, "日志服务端 IP 格式无效（点分四段）"); return; }
  if (!logconf_port_valid(port, &port_i)) { app_http_error(fd, 400, "端口必须是 1-65535 的数字"); return; }
  if (strcmp(en, "0") != 0 && strcmp(en, "1") != 0) { app_http_error(fd, 400, "开关必须是 0 或 1"); return; }
  on = (en[0] == '1');
  kb_i = 128;
  if (kb[0]) {
    if (!logconf_port_valid(kb, &kb_i) || kb_i < 8 || kb_i > 1024) {
      app_http_error(fd, 400, "日志大小必须是 8-1024 的数字(K)");
      return;
    }
  }
  {
    char out[300];
    int n, d, rc;
    n = snprintf(out, sizeof(out),
                 "# LX06 log server config (WebAdmin 可编辑；看门狗重启前读取发送)\n"
                 "log_server_ip=%s\nlog_server_port=%d\nlog_enabled=%d\nlog_max_kb=%d\n",
                 ip, port_i, on, kb_i);
    d = app_trusted_dir("/data", 0755, 1);
    if (d < 0) { app_http_error(fd, 503, "打开 /data 目录失败"); return; }
    rc = app_atomic(d, "lx06-log.conf", out, (size_t)n, 0644);
    close(d);
    if (rc != 0) { app_http_error(fd, 503, "日志传送配置持久化失败，未报告成功"); return; }
    send_json(fd, "{\"ok\":true}");
  }
}

/* 日志内容 → JSON 转义 */
static size_t json_escape(const char *src, size_t slen, char *dst, size_t dstsz) {
  size_t e = 0, k;
  for (k = 0; k < slen && e + 8 < dstsz; k++) {
    unsigned char c = (unsigned char)src[k];
    switch (c) {
      case '"':  dst[e++] = '\\'; dst[e++] = '"'; break;
      case '\\': dst[e++] = '\\'; dst[e++] = '\\'; break;
      case '\n': dst[e++] = '\\'; dst[e++] = 'n'; break;
      case '\r': break;
      case '\t': dst[e++] = '\\'; dst[e++] = 't'; break;
      default:
        if (c < 0x20) e += (size_t)snprintf(dst + e, 7, "\\u%04x", c);
        else dst[e++] = (char)c;
    }
  }
  dst[e] = 0;
  return e;
}

/* --- 路由处理 --- */
static void h_login(int fd, const char *body) {
  char pw[256], hash[160], tok[80];
  char hdr[256];
  json_str(body, "password", pw, sizeof(pw));
  if (!pw[0] || conf_get(CONF, "ssh_root_hash", hash, sizeof(hash)) != 0) {
    send_err(fd, 401, "Unauthorized", "{\"ok\":false,\"error\":\"密码错误\"}");
    return;
  }
  if (!hash_match(hash, pw)) {
    send_err(fd, 401, "Unauthorized", "{\"ok\":false,\"error\":\"密码错误\"}");
    return;
  }
  /* 多会话：新令牌追加进令牌池（最多 4 个，满则挤掉最旧），后登录不踢先登录 */
  sess_rand_tok(tok, sizeof(tok));
  sess_add(tok);
  snprintf(hdr, sizeof(hdr), "Set-Cookie: webd=%s; Path=/; HttpOnly\r\n", tok);
  /* blen 必须 strlen（=11）：硬编码 12 会多写 1 字节 NUL 进 body，
   * 前端 r.json() 解析 "{"ok":true}\x00" 抛错 → 登录报「响应异常」要刷新。
   * （2026-10-11 真机 raw 字节定位：body hex 7b226f6b223a747275657d00） */
  send_simple(fd, 200, "OK", "application/json; charset=utf-8", "{\"ok\":true}",
              strlen("{\"ok\":true}"), hdr);
}
/* --- 文件系统空间：dir = "/data" 或 "/tmp"。
   直接用设备自带 glibc 2.25 的 statvfs()（本二进制动态链 2.25，头/库同源）。
   编译期 -D_FILE_OFFSET_BITS=32 → struct statvfs 字段 32 位，与 2.25 的
   32 位 __statvfs 调用布局一致（QEMU gsv.c 实测 total/free 均为正确正数）。
   之前手拼 syscall(187)+自定 64 位结构体反而因布局错位读到负数，已废弃。 --- */
/* 返回 0=成功；total/used/free 单位 KB */
static int fs_space_kb(const char *dir, long *total_kb, long *used_kb, long *free_kb) {
  struct statvfs v;
  if (statvfs(dir, &v) != 0) return -1;
  unsigned long long bsz = (unsigned long long)v.f_frsize;
  if (bsz <= 0) bsz = (unsigned long long)v.f_bsize;
  if (bsz <= 0) return -1;
  unsigned long long tot = (unsigned long long)v.f_blocks * bsz / 1024ULL;
  unsigned long long fre = (unsigned long long)v.f_bavail * bsz / 1024ULL;
  unsigned long long usr = (unsigned long long)v.f_bfree  * bsz / 1024ULL;
  if (usr > tot) usr = tot;
  /* V14fix：允许任一出参为 NULL（只取 free 的调用方，如 h_apps_upload 的
     /tmp 检查）——旧版无条件解引用，传 NULL 必段错误（2026-10-16 .166
     webd 子进程崩溃循环根因，搞死 88/89 的源头）。 */
  if (total_kb) *total_kb = (long)tot;
  if (free_kb)  *free_kb  = (long)fre;
  if (used_kb) {
    long uk = (long)(tot - usr);
    *used_kb = uk < 0 ? 0 : uk;
  }
  return 0;
}
static long data_free_kb(void) {
  long t, u, f;
  const char *ov;
  if (fs_space_kb("/data", &t, &u, &f) != 0) return -1;
  ov = getenv("WEBD_MIN_FREE_KB");   /* 仅 QEMU 冒烟注入用（真机不设置） */
  if (ov && *ov) {
    long x = atol(ov);
    if (x >= 0) return x;
  }
  return f;
}
/* --- 实时 CPU/内存占用（前端「CPU资源/内存资源」卡片，3s 轮询 /api/system-stats）。
   CPU 多核兼容：/proc/stat 首行 "cpu " 是【全部核心汇总】（1/2/4/8 核都只有一行，
   内核已把各核累加），直接对两次采样求差：
     usage% = Δtotal - Δ(idle+iowait) / Δtotal
   static 基线：webd 启动后第一次调用只存基线返回 0，第二次起给出实时值。
   内存：MemAvailable（kernel≥3.14）；老内核无该字段时回退
   MemFree+Buffers+Cached+SReclaimable（与 Node freemem 口径一致）。 --- */
static unsigned long long cpu_prev_total = 0, cpu_prev_idle = 0;
static int cpu_sample(unsigned long long *total, unsigned long long *idle) {
  FILE *f = fopen("/proc/stat", "r");
  char line[256];
  unsigned long long v[10];
  int i, n;
  if (!f) return -1;
  if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
  fclose(f);
  /* "cpu  user nice system idle iowait irq softirq steal guest guest_nice"
     （老内核列数可能 <10；至少要有 user nice system idle 四列） */
  if (strncmp(line, "cpu ", 4) != 0) return -1;
  v[0] = v[1] = v[2] = v[3] = v[4] = v[5] = v[6] = v[7] = v[8] = v[9] = 0;
  n = sscanf(line + 4,
             "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
             &v[6], &v[7], &v[8], &v[9]);
  if (n < 4) return -1;
  {
    unsigned long long tot = 0, idl = v[3];
    for (i = 0; i < n && i < 10; i++) tot += v[i];
    if (n > 4) idl += v[4];              /* iowait 也按空闲计（无该列的老内核跳过） */
    *total = tot;
    *idle = idl;
  }
  return 0;
}
static double cpu_usage_pct(void) {
  unsigned long long tot = 0, idl = 0, dt, di;
  if (cpu_sample(&tot, &idl) != 0) return 0;
  if (cpu_prev_total == 0) {             /* 首次采样：只建基线 */
    cpu_prev_total = tot;
    cpu_prev_idle = idl;
    return 0;
  }
  if (tot < cpu_prev_total) {            /* 计数器复位（重启/异常）：重建基线 */
    cpu_prev_total = tot;
    cpu_prev_idle = idl;
    return 0;
  }
  dt = tot - cpu_prev_total;
  di = (idl > cpu_prev_idle) ? idl - cpu_prev_idle : 0;
  cpu_prev_total = tot;
  cpu_prev_idle = idl;
  if (dt <= 0) return 0;                 /* 计数器异常（复位/回绕）保护 */
  if (di > dt) di = dt;
  return (double)(dt - di) * 100.0 / (double)dt;
}
static void h_systemstats(int fd) {
  double cpu = cpu_usage_pct(), mem = 0;
  long mtot = -1, mavail = -1, mfree = -1, buffers = 0, cached = 0, srecl = 0;
  char line[256];
  char body[160];
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      if (!strncmp(line, "MemTotal:", 9)) mtot = atol(line + 9);
      else if (!strncmp(line, "MemAvailable:", 13)) mavail = atol(line + 13);
      else if (!strncmp(line, "MemFree:", 8)) mfree = atol(line + 8);
      else if (!strncmp(line, "Buffers:", 8)) buffers = atol(line + 8);
      else if (!strncmp(line, "Cached:", 7)) cached = atol(line + 7);
      else if (!strncmp(line, "SReclaimable:", 13)) srecl = atol(line + 13);
    }
    fclose(f);
  }
  if (mavail < 0 && mfree >= 0) mavail = mfree + buffers + cached + srecl;
  if (mtot > 0 && mavail >= 0) {
    mem = (double)(mtot - mavail) * 100.0 / (double)mtot;
    if (mem < 0) mem = 0;
    if (mem > 100) mem = 100;
  }
  if (cpu < 0) cpu = 0;
  if (cpu > 100) cpu = 100;
  /* V14.1：附内存绝对值（KB，/proc/meminfo 口径）。前端内存卡片底部显示
     「已使用 N MB / 剩余 N MB」真实数据；顶部大百分比不变。 */
  if (mtot > 0 && mavail >= 0) {
    snprintf(body, sizeof(body),
      "{\"ok\":true,\"cpu\":%d,\"memory\":%d,\"mem_total_kb\":%ld,\"mem_avail_kb\":%ld}",
      (int)(cpu + 0.5), (int)(mem + 0.5), mtot, mavail);
  } else {
    snprintf(body, sizeof(body), "{\"ok\":true,\"cpu\":%d,\"memory\":%d}",
             (int)(cpu + 0.5), (int)(mem + 0.5));
  }
  send_json(fd, body);
}
static int resident_line_clean(const char *p, size_t n, char *out, size_t outsz); /* 定义在下方常驻块区 */
static void h_status(int fd) {
  int scripts_callable=lx06_scripts_allowed();
  char port[16] = "89", lan[4] = "1", mline[256];
  char dv[8];
  int sshon = 0, first = 1;
  int log_ssh = 1, log_web = 1, log_webd = 1;
  long up = 0, memtot = -1, memfree = -1;
  long dtot = -1, dused = -1, dfree = -1, ttot = -1, tused = -1, tfree = -1;
  FILE *f;
  DIR *d;
  struct dirent *de;
  /* fix15：日志开关状态（缺省=开）→ 前端 SSH日志/Web日志 弹窗初始态 */
  if (conf_get(CONF, "log_ssh", dv, sizeof(dv)) == 0 && dv[0] == '0') log_ssh = 0;
  if (conf_get(CONF, "log_web", dv, sizeof(dv)) == 0 && dv[0] == '0') log_web = 0;
  if (conf_get(CONF, "log_webd", dv, sizeof(dv)) == 0 && dv[0] == '0') log_webd = 0;
  /* apps JSON 用动态缓冲（原固定 1600 字节，apps 多时 strncat 截断 →
     整个 status JSON 非法）。上限 16000 覆盖数百个脚本。 */
  char *appsj = malloc(16000);
  if (!appsj) { send_err(fd, 500, "Server Error", "{\"ok\":false,\"error\":\"内存不足\"}"); return; }
  appsj[0] = 0;
  conf_get(CONF, "ssh_port", port, sizeof(port));
  conf_get(CONF, "ssh_lan_only", lan, sizeof(lan));
  if (port[0] < '0' || port[0] > '9') { port[0] = '2'; port[1] = '2'; port[2] = 0; }
  if (fs_space_kb("/data", &dtot, &dused, &dfree) != 0) { dtot = dused = dfree = -1; }
  if (fs_space_kb("/tmp", &ttot, &tused, &tfree) != 0) { ttot = tused = tfree = -1; }
  f = fopen("/proc/uptime", "r");
  if (f) { if (fgets(mline, sizeof(mline), f)) up = atol(mline); fclose(f); }
  f = fopen("/proc/meminfo", "r");
  if (f) {
    while (fgets(mline, sizeof(mline), f)) {
      if (!strncmp(mline, "MemTotal:", 9)) memtot = atol(mline + 9);
      else if (!strncmp(mline, "MemFree:", 8)) memfree = atol(mline + 8);
      if (memtot > 0 && memfree >= 0) break;
    }
    fclose(f);
  }
  d = opendir("/proc");
  if (d) {
    while (!sshon && (de = readdir(d)) != NULL) {
      char comm[300], cbuf[64];
      FILE *cf;
      if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
      snprintf(comm, sizeof(comm), "/proc/%s/comm", de->d_name);
      cf = fopen(comm, "r");
      if (!cf) continue;
      if (fgets(cbuf, sizeof(cbuf), cf)) {
        cbuf[strcspn(cbuf, "\n")] = 0;
        if (strcmp(cbuf, "dropbear") == 0 && (long)atoi(de->d_name) != (long)getpid()) sshon = 1;
      }
      fclose(cf);
    }
    closedir(d);
  }
  {
    size_t len = 0;
    char listed[256][65];
    unsigned listed_n = 0;
    int pass, mi, mc = appmeta_count(), failed = 0;
    const char *dirs[] = {APPPERSIST, APPDIR};
    /* 常驻名单（用于应用行 resident 标记；与开机解析同清洗语义，兼容旧版 NUL 空洞文件） */
    char res_names[16][65];
    unsigned res_n = 0;
    {
      int rfd = open("/data/lx06/keeper/resident", O_RDONLY);
      if (rfd >= 0) {
        char rbuf[1024];
        off_t rsz = lseek(rfd, 0, SEEK_END);
        size_t rn = 0;
        int rr;
        if (rsz > 0 && rsz <= (off_t)sizeof(rbuf) - 1) {
          lseek(rfd, 0, SEEK_SET);
          while (rn < (size_t)rsz) {
            rr = read(rfd, rbuf + rn, (size_t)rsz - rn);
            if (rr <= 0) break;
            rn += (size_t)rr;
          }
          rbuf[rn] = 0;
          {
            const char *rp = rbuf;
            while (*rp) {
              const char *re = rp;
              char rnm[65];
              unsigned dk;
              int dupk = 0;
              while (*re && *re != '\n') re++;
              if (resident_line_clean(rp, (size_t)(re - rp), rnm, sizeof(rnm))) {
                for (dk = 0; dk < res_n; dk++)
                  if (!strcmp(res_names[dk], rnm)) dupk = 1;
                if (!dupk && res_n < 16) {
                  strcpy(res_names[res_n], rnm);
                  res_n++;
                }
              }
              rp = (*re == '\n') ? re + 1 : re;
            }
          }
        }
        close(rfd);
      }
    }
    /* appmeta 顺序为主，再补登记失败但已保存的原件；不得静默漏掉条目。 */
    keeper_status_read(1); keeper_batch = 1;
    for (pass = 0; pass < 3 && !failed; pass++) {
      int dirfd = -1;
      if (pass) {
        dirfd = app_trusted_dir(dirs[pass - 1], pass == 1 ? 0755 : 0700, 0);
        d = dirfd < 0 ? NULL : fdopendir(dirfd);
        if (!d && dirfd >= 0) close(dirfd);
      }
      for (mi = 0; !failed; mi++) {
        char nm[65], entry[448];
        long mt = 0, sz = -1;
        unsigned k;
        int dup = 0, stt, fdir, af, hasext = 0;
        unsigned markers = 0;
        struct lx06_script_audio_metadata audio_meta = {LX06_AUDIO_UNSPECIFIED, LX06_PHONE_NONE};
        LX06_STATBUF ast;
        if (!pass) {
          if (mi >= mc) break;
          if (appmeta_get(mi, nm, sizeof(nm), &mt) != 0) continue;
        } else {
          if (!d || !(de = readdir(d))) break;
          if (strlen(de->d_name) >= sizeof(nm)) continue;
          strcpy(nm, de->d_name);
        }
        if (!app_name_safe(nm)) continue;
        for (k = 0; k < listed_n; k++) if (!strcmp(listed[k], nm)) dup = 1;
        if (dup) continue;
        fdir = app_trusted_dir(APPPERSIST, 0755, 0);
        af = fdir < 0 ? -1 : app_open_file(fdir, nm, O_RDONLY);
        if (fdir >= 0) close(fdir);
        if (af < 0) {
          fdir = app_trusted_dir(APPDIR, 0700, 0);
          af = fdir < 0 ? -1 : app_open_file(fdir, nm, O_RDONLY);
          if (fdir >= 0) close(fdir);
        }
        if (af >= 0) {
          if (LX06_FSTAT(af, &ast) == 0) sz = (long)ast.st_size;
          unsigned char header[8193]={0};ssize_t hn=pread(af,header,8192,0);
          if(hn>0){markers=app_marker_flags(header,(size_t)hn);audio_meta=lx06_script_audio_parse(header,(size_t)hn);}
          struct ext_schema *schema = calloc(1, sizeof(*schema));
          if (schema) { hasext = ext_read(af, schema) == 1; free(schema); }
          close(af);
        }
        else if (pass) continue;
        stt = app_status(nm);
        {
          int isres = 0;
          int isroot=(markers & APP_RUN_ROOT)!=0;
          unsigned rk;
          if(keeper_cached_ok)for(rk=0;rk<keeper_cached.jobs;rk++)
            if(!strncmp(keeper_cached.job[rk].name,nm,sizeof(keeper_cached.job[rk].name))) {isroot=keeper_cached.job[rk].uid==0;break;}
          for (rk = 0; rk < res_n; rk++)
            if (!strcmp(res_names[rk], nm)) isres = 1;
          snprintf(entry, sizeof(entry),
            "%s{\"name\":\"%s\",\"size\":%ld,\"status\":%d,\"installed\":%ld,\"uninstallable\":1,\"conflict\":0,\"resident\":%d,\"resident_declared\":%d,\"root\":%d,\"extension\":%d,\"callable\":%d,\"audio_mode\":\"%s\"}",
            first ? "" : ",", nm, sz, stt < 0 ? 0 : stt, mt, isres, !!(markers & APP_DECLARED_RESIDENT), isroot, hasext,scripts_callable,lx06_script_audio_name(audio_meta.mode));
        }
        if (listed_n >= 256 || len + strlen(entry) + 4 >= 16000) { failed = 1; break; }
        strcpy(listed[listed_n++], nm);
        memcpy(appsj + len, entry, strlen(entry)); len += strlen(entry);
        first = 0;
      }
      if (pass && d) closedir(d);
    }
    keeper_batch = 0;
    if (failed) {
      free(appsj); send_err(fd, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"应用列表超过响应预算，请清理管理记录\"}"); return;
    }
    appsj[len] = 0;
  }
  if (appsj[0] == 0) {
    snprintf(appsj, 16000, "[]");
  } else {
    /* 外层补 [ ]（前端 j.apps.forEach 需数组）。L=内容长度，先记再移位。 */
    size_t L = strlen(appsj);
    char *tmp2 = realloc(appsj, L + 4);
    if (tmp2) appsj = tmp2;
    memmove(appsj + 1, appsj, L + 1);   /* 内容移到 [1..L] */
    appsj[0] = '[';
    appsj[L + 1] = ']';
    appsj[L + 2] = 0;
  }
  /* 开机自启动入口（/data/init.sh，刷机/恢复出厂后由 init-launch 自动播种）。
     json 用动态缓冲（含 tmp_free_kb 与变长 apps，原固定 2300 会截断）。 */
  {
    int ie = 0;
    char json[18000];
    if (file_size("/data/init.sh") > 0) ie = 1;
    snprintf(json, sizeof(json),
      "{\"ok\":true,\"ssh_port\":%ld,\"http_port\":88,\"lan_only\":%s,"
      "\"ssh_running\":%s,\"uptime\":%ld,\"mem_total\":%ld,\"mem_free\":%ld,"
      "\"data_total_kb\":%ld,\"data_used_kb\":%ld,\"data_free_kb\":%ld,"
      "\"tmp_total_kb\":%ld,\"tmp_used_kb\":%ld,\"tmp_free_kb\":%ld,"
      "\"init_entry\":%d,"
      "\"log_ssh\":%d,\"log_web\":%d,\"log_webd\":%d,"
      "\"apps\":%s}",
      atol(port), (lan[0] == '0' || lan[0] == '1') ? lan : "1",
      sshon ? "true" : "false", up, memtot, memfree,
      dtot, dused, dfree, ttot, tused, tfree, ie, log_ssh, log_web, log_webd, appsj);
    free(appsj);
    send_json(fd, json);
  }
}
static void h_password(int fd, const char *body) {
  char oldp[256] = "", newp[256] = "";
  char curhash[160] = "", newhash[128], salt[9];
  FILE *f;
  json_str(body, "old", oldp, sizeof(oldp));
  json_str(body, "new", newp, sizeof(newp));
  if (strlen(newp) < 4) {
    send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"新密码至少 4 位\"}");
    return;
  }
  if (conf_get(CONF, "ssh_root_hash", curhash, sizeof(curhash)) != 0) {
    send_err(fd, 500, "Server Error", "{\"ok\":false,\"error\":\"配置不可读\"}");
    return;
  }
  if (!hash_match(curhash, oldp)) {
    send_err(fd, 401, "Unauthorized", "{\"ok\":false,\"error\":\"原密码错误\"}");
    return;
  }
  rand_salt(salt);
  md5crypt(newp, salt, newhash);
  /* 1) 写 conf：保留其他行，替换 ssh_root_hash 行（SSH 侧唯一 hash 源） */
  {
    char *s = NULL;
    size_t n, i, L;
    char outb[1400];
    int w = 0;
    if (file_read_all(CONF, &s, &n) == 0) {
      L = n;
      for (i = 0; i < L; i++) {
        if (i > 0 && s[i-1] != '\n') continue;
        if (strncmp(s + i, "ssh_root_hash=", 14) == 0) continue;
        {
          const char *e2 = strchr(s + i, '\n');
          size_t l = e2 ? (size_t)(e2 - (s + i)) : L - i;
          if ((size_t)(w + (int)l + 2) > sizeof(outb)) break;
          memcpy(outb + w, s + i, l);
          w += (int)l;
          outb[w++] = '\n';
        }
      }
      free(s);
    }
    w += sprintf(outb + w, "ssh_root_hash=%s\n", newhash);
    f = fopen(CONF, "w");
    if (f) { fwrite(outb, 1, (size_t)w, f); fclose(f); }
  }
  /* 2) /data/etc/shadow 的 root 行（/etc/shadow 若已 bind，PAM 下次认证即读新值） */
  {
    char line[512], out[2048];
    size_t o = 0;
    f = fopen(SHADOW, "r");
    if (f) {
      while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "root:", 5))
          o += (size_t)snprintf(out + o, sizeof(out) - o, "root:%s:18128:0:99999:7:::\n", newhash);
        else
          o += (size_t)snprintf(out + o, sizeof(out) - o, "%s", line);
        if (o >= sizeof(out) - 100) break;
      }
      fclose(f);
      f = fopen(SHADOW, "w");
      if (f) { fwrite(out, 1, o, f); fclose(f); }
    } else {
      mkdir("/data/etc", 0755);
      f = fopen(SHADOW, "w");
      if (f) { fprintf(f, "root:%s:18128:0:99999:7:::\n", newhash); fclose(f); }
    }
  }
  send_json(fd, "{\"ok\":true,\"note\":\"即时生效：SSH/HTTP 下次登录用新密码，当前连接不受影响\"}");
}
static void h_reboot(int fd) {
  send_json(fd, "{\"ok\":true,\"note\":\"设备正在重启\"}");
  if (fork() == 0) {
    sleep(1);
    execl("/sbin/reboot", "reboot", "-f", (char *)0);
    system("reboot -f");
    _exit(1);
  }
}
/* 旧 root 部署只显示/清理管理记录，不会运行其持久脚本或修改任意 /data。 */
static int app_legacy(const char *name) {
  int d = app_meta_dir(name, 1);
  LX06_STATBUF st;
  int legacy;
  if (d < 0) return -1;
  legacy = LX06_STATAT(d, "deploy.sh", &st, AT_SYMLINK_NOFOLLOW) == 0 ||
           LX06_STATAT(d, "before.txt", &st, AT_SYMLINK_NOFOLLOW) == 0;
  close(d); return legacy;
}
/* 只在 root 私有元数据树内递归 unlink；不跟链接，不执行 rm/shell。
 * workspace 由 keeper 在停净后删除；网页不递归不可信用户目录。 */
static int app_remove_tree(int parent, const char *name, unsigned depth, unsigned *count) {
  int fd, result = 0;
  DIR *d;
  struct dirent *de;
  LX06_STATBUF st;
  if (depth > 16 || ++*count > 4096) { errno = E2BIG; return -1; }
  if (LX06_STATAT(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode)) return unlinkat(parent, name, 0);
  fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  d = fdopendir(fd);
  if (!d) { close(fd); return -1; }
  for (;;) {
    errno = 0; de = readdir(d);
    if (!de) { if (errno) result = -1; break; }
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (app_remove_tree(fd, de->d_name, depth + 1, count) != 0) { result = -1; break; }
  }
  { int e = errno; closedir(d); errno = e; }
  return result ? -1 : unlinkat(parent, name, AT_REMOVEDIR);
}
static int app_remove_at(const char *dir, mode_t mode, const char *name) {
  int d = app_trusted_dir(dir, mode, 0), result;
  if (d < 0) return errno == ENOENT ? 0 : -1;
  result = app_settings_current() ? -1 : unlinkat(d, name, 0);
  if (result != 0 && errno == ENOENT) result = 0;
  if (result == 0 && fsync(d) != 0) result = -1;
  close(d); return result;
}
/* --- 常驻服务名单 + init.sh 托管块（通用机制，每个 job 统一） ---
 * 两份数据、一处真相：
 *   /data/lx06/keeper/resident — webd 机器名单（root 0600，每行一个 job 名）。
 *   /data/init.sh 的托管块      — 开机真正执行的逐应用启动行（root 段，
 *                                 re-exec 进受限 job 之前）。
 * profile=resident 部署/运行成功 → 登记（on=1）；卸载 → 注销（on=0）；
 * 每次登记/注销都整表重写名单并同步重渲染 init.sh 托管块。
 * 开机由 /data/init.sh 的托管块经 keeper 拉起（启动方式按文件形态渲染：
 * shebang 自执行或 /bin/sh 解释，与应用形态无关）。keeper 管理端只收
 * root 客户端 → 块必须在 init.sh 的 root 段；非 root 受限段无法启动应用，
 * 用户脚本因此不能绕过 init.sh 形成第二条开机自启通道：看住 init.sh
 * 就看住了全部开机用户代码（总开关 OFF 时 init-launch 不 exec init.sh）。
 * 整表重写=天然去重；托管块原子 tmp+rename，失败软着陆（下次同步重试）。 */
#define RESIDENT_BLOCK_BEGIN "# ---- lx06-resident begin (web88 托管块：resident 应用部署/卸载时自动增删；勿手工编辑) ----"
#define RESIDENT_BLOCK_END   "# ---- lx06-resident end ----"
#define RESIDENT_INIT_PATH   "/data/init.sh"

/* 名单行清洗：旧版 NUL 空洞文件（2026-10-18 二次同步遗留）里，一行可能是
 * NUL×N + 名字；去 NUL/空白后恰好通过 app_name_safe 才算有效名字。
 * 返回 1=有效名字（写入 out，≤64B），0=无效。 */
static int resident_line_clean(const char *p, size_t n, char *out, size_t outsz) {
  size_t i, j = 0;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c == '\0' || c == ' ' || c == '\t') continue;
    if (j < outsz - 1) out[j++] = (char)c;
  }
  out[j] = 0;
  return j > 0 && app_name_safe(out);
}
/* 渲染单个应用的启动行（按文件形态：shebang 自执行 / sh 解释）。
 * 应用文件缺失/不可读 → -1（该应用不渲染，名单条目保留，文件回来后再同步）。
 * 文件自声明 root（# LX06-RUN: root / # LX06-INSTALL: root / isroot=1）→
 * start 行带 --root（LK_ROOT），与手动运行/上传运行同一语义。 */
static int resident_app_exec_line(const char *name, char *out, size_t outsz) {
  unsigned char hd[8192];
  int d, f, isroot;
  LX06_STATBUF st;
  d = app_trusted_dir(APPPERSIST, 0755, 0);
  if (d < 0) return -1;
  if (LX06_STATAT(d, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
    int e = errno;
    close(d);
    errno = e;
    return -1;
  }
  f = app_open_file(d, name, O_RDONLY);
  close(d);
  if (f < 0) return -1;
  {
    ssize_t rn = read(f, hd, sizeof(hd));
    if (rn < 2) { close(f); return -1; }
    close(f);
    /* 开机常驻=start 运行期：仅 RUN 标记决定 root；INSTALL 标记只管首次
     * （部署）运行，实现「root 安装 + 非 root 运行」正交（用户四模式定案）。
     * 只扫真实读到的 rn 字节：hd 未读区是栈垃圾，带 NUL 会卡死行扫描。 */
    isroot = (app_marker_flags(hd, (size_t)rn) & APP_RUN_ROOT) != 0;
  }
  if (hd[0] == '#' && hd[1] == '!')
    snprintf(out, outsz,
      "  if [ -f " APPPERSIST "/%s ] && [ ! -L " APPPERSIST "/%s ]; then\n"
      "    /usr/bin/lx06-keeper start %s%s -- " APPPERSIST "/%s 2>/dev/null || true\n"
      "  fi\n", name, name, name, isroot ? " --root" : "", name);
  else
    snprintf(out, outsz,
      "  if [ -f " APPPERSIST "/%s ] && [ ! -L " APPPERSIST "/%s ]; then\n"
      "    /usr/bin/lx06-keeper start %s%s -- /bin/sh " APPPERSIST "/%s 2>/dev/null || true\n"
      "  fi\n", name, name, name, isroot ? " --root" : "", name);
  return 0;
}
/* 由当前名单构建整块文本（begin/结束标记 + 逐应用行）。空名单也产出块
 * （内容行=（当前无常驻应用）），保证模板升级后托管块恒存在。 */
static int resident_build_block(char *out, size_t outsz) {
  static char buf[1024];
  char seen[16][65];
  unsigned seen_n = 0, count;
  int fd, r;
  off_t sz;
  size_t n, len = 0;
  const char *p;
#define RB_APPEND(fmt, ...)                                              \
  do {                                                                   \
    r = snprintf(out + len, outsz - len, fmt, ##__VA_ARGS__);            \
    if (r < 0 || (size_t)r >= outsz - len) return -1;                    \
    len += (size_t)r;                                                    \
  } while (0)
  RB_APPEND("%s\n", RESIDENT_BLOCK_BEGIN);
  fd = open("/data/lx06/keeper/resident", O_RDONLY);
  count = 0;
  if (fd >= 0) {
    sz = lseek(fd, 0, SEEK_END);
    if (sz > 0 && sz <= (off_t)sizeof(buf) - 1) {
      lseek(fd, 0, SEEK_SET);
      n = 0;
      while (n < (size_t)sz) {
        r = read(fd, buf + n, (size_t)sz - n);
        if (r <= 0) break;
        n += (size_t)r;
      }
      buf[n] = 0;
      p = buf;
      while (*p) {
        const char *e = p;
        char name[65], line[256];
        unsigned k;
        int dup = 0, erc;
        while (*e && *e != '\n') e++;
        if (resident_line_clean(p, (size_t)(e - p), name, sizeof(name))) {
          for (k = 0; k < seen_n; k++)
            if (!strcmp(seen[k], name)) dup = 1;
          if (!dup) {
            erc = resident_app_exec_line(name, line, sizeof(line));
            if (erc == 0) {
              RB_APPEND("%s", line);
              count++;
              if (seen_n < 16) {
                strcpy(seen[seen_n], name);
                seen_n++;
              }
            }
          }
        }
        p = (*e == '\n') ? e + 1 : e;
      }
    }
    close(fd);
  }
  if (count == 0) RB_APPEND("# （当前无常驻应用）\n");
  RB_APPEND("%s\n", RESIDENT_BLOCK_END);
#undef RB_APPEND
  return 0;
}
/* 把托管块同步进 /data/init.sh：有标记→整块替换；无标记→插到 re-exec 守卫
 * （if [ "$(id -u)" = 0 ] 行）之前；守卫也缺失→追加到文件尾。
 * 只改块本身，用户其他内容不动。原子 tmp+rename（同一 /data 目录内）。
 * 失败软着陆：返回 -1，不动原文件；下次 resident_sync/webd 启动重试。 */
static int resident_apply_block(void) {
  static char block[4096], content[65536], out[65536 + 4096 + 8];
  int fd, tfd, r, dd;
  LX06_STATBUF st;
  size_t n, i, j, k, ins, len;
  const char *anchor;
  if (geteuid() != 0) { errno = EPERM; return -1; }
  dd = open("/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (dd < 0) return -1;
  if (LX06_STATAT(dd, "init.sh", &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(st.st_mode) || st.st_uid != 0 || (size_t)st.st_size >= sizeof(content)) {
    close(dd);
    return -1;
  }
  fd = openat(dd, "init.sh", O_RDONLY);
  if (fd < 0) { close(dd); return -1; }
  n = 0;
  while (n < sizeof(content) - 1) {
    r = read(fd, content + n, sizeof(content) - 1 - n);
    if (r < 0) { if (errno == EINTR) continue; close(fd); close(dd); return -1; }
    if (r == 0) break;
    n += (size_t)r;
  }
  close(fd);
  if (n == sizeof(content) - 1) { close(dd); errno = E2BIG; return -1; }
  content[n] = 0;
  if (resident_build_block(block, sizeof(block)) != 0) { close(dd); return -1; }
  /* 1) 已有托管块 → 整块替换（begin 行首 到 end 行尾，含两标记行）。 */
  {
    char bline[256], eline[256];
    snprintf(bline, sizeof(bline), "%s\n", RESIDENT_BLOCK_BEGIN);
    snprintf(eline, sizeof(eline), "%s\n", RESIDENT_BLOCK_END);
    char *bs = strstr(content, bline);
    char *es = bs ? strstr(bs + strlen(bline), eline) : NULL;
    if (bs && es) {
      size_t pre = (size_t)(bs - content), post = strlen(es + strlen(eline));
      len = 0;
      memcpy(out, content, pre);
      len += pre;
      memcpy(out + len, block, strlen(block));
      len += strlen(block);
      memcpy(out + len, es + strlen(eline), post);
      len += post;
      out[len] = 0;
      goto write;
    }
  }
  /* 2) 无标记 → 插到 re-exec 守卫行之前（root 段内、受限 re-exec 之前）。 */
  anchor = strstr(content, "if [ \"$(id -u)\" = 0 ]; then");
  if (anchor) {
    /* 回退到该行行首 */
    ins = (size_t)(anchor - content);
    while (ins > 0 && content[ins - 1] != '\n') ins--;
    len = 0;
    memcpy(out, content, ins);
    len += ins;
    j = len;
    for (k = 0; block[k]; k++) out[j++] = block[k];
    if (content[ins] != '\n') out[j++] = '\n';
    len = j;
    memcpy(out + len, content + ins, strlen(content + ins));
    len += strlen(content + ins);
    out[len] = 0;
    goto write;
  }
  /* 3) 守卫也缺失（用户大改）→ 追加到文件尾（块内自带 root 判断，不递归）。 */
  len = strlen(content);
  if (len == 0 || content[len - 1] != '\n') out[len++] = '\n';
  out[len++] = '\n';
  j = len;
  for (k = 0; block[k]; k++) out[j++] = block[k];
  out[j] = 0;
write:
  /* /data 目录内原子替换：固定 tmp 名 + O_EXCL 防链接/复用，沿用 autostart 写法。 */
  for (i = 0; i < 2; i++) {
    if (LX06_STATAT(dd, ".init.sh.lx06tmp", &st, AT_SYMLINK_NOFOLLOW) == 0) {
      if (!S_ISREG(st.st_mode) || st.st_uid != 0 || st.st_nlink != 1 ||
          (st.st_mode & 0022)) { close(dd); errno = EPERM; return -1; }
      if (unlinkat(dd, ".init.sh.lx06tmp", 0) != 0) { close(dd); return -1; }
    } else if (errno != ENOENT) { close(dd); return -1; }
  }
  tfd = openat(dd, ".init.sh.lx06tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0755);
  if (tfd < 0) { close(dd); return -1; }
  if (fchown(tfd, 0, 0) != 0 || fchmod(tfd, 0755) != 0 ||
      write_all(tfd, out, strlen(out)) != 0 || fsync(tfd) != 0) {
    int e = errno;
    close(tfd);
    unlinkat(dd, ".init.sh.lx06tmp", 0);
    close(dd);
    errno = e;
    return -1;
  }
  if (close(tfd) != 0) {
    int e = errno;
    unlinkat(dd, ".init.sh.lx06tmp", 0);
    close(dd);
    errno = e;
    return -1;
  }
  if (renameat(dd, ".init.sh.lx06tmp", dd, "init.sh") != 0 || fsync(dd) != 0) {
    int e = errno;
    unlinkat(dd, ".init.sh.lx06tmp", 0);
    close(dd);
    errno = e;
    return -1;
  }
  close(dd);
  return 0;
}
static int resident_sync(const char *name, int on) {
  int fd, r;
  char buf[1024], out[1024];
  off_t sz;
  size_t n, cur;
  if (!app_name_safe(name)) return -1;
  fd = open("/data/lx06/keeper/resident", O_RDWR | O_CREAT, 0600);
  if (fd < 0) return -1;
  sz = lseek(fd, 0, SEEK_END);
  if (sz < 0 || sz > (off_t)sizeof(buf) - 1) { close(fd); errno = E2BIG; return -1; }
  /* Measuring the file leaves the cursor at EOF. Read the existing registry
     from its start, otherwise enrolling one app silently drops all peers. */
  if (lseek(fd, 0, SEEK_SET) != 0) { close(fd); return -1; }
  n = 0;
  while (n < (size_t)sz) {
    r = read(fd, buf + n, (size_t)sz - n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { close(fd); errno=EIO; return -1; }
    n += (size_t)r;
  }
  buf[n] = 0;
  out[0] = 0;
  {
    const char *p = buf, *limit=buf+n;
    while (p<limit) {
      const char *e = p;
      while (e<limit && *e != '\n') e++;
      n = (size_t)(e - p);
      if (n > 0) {
        char cleaned[65];
        /* 清洗后与目标名相同（含旧版 NUL 空洞行）→ 注销/改写：跳过原行，
           整表重写时以干净行落盘（顺带自愈 NUL 空洞文件）。 */
        if (resident_line_clean(p, n, cleaned, sizeof(cleaned)) &&
            strcmp(cleaned, name) != 0) {
          cur=strlen(out);
          r = snprintf(out + cur, sizeof(out) - cur, "%s\n", cleaned);
          if (r < 0 || (size_t)r >= sizeof(out) - cur) {
            close(fd); errno = E2BIG; return -1;
          }
        }
      }
      p = e<limit ? e+1 : e;
    }
  }
  if (on) {
    cur = strlen(out);
    r = snprintf(out + cur, sizeof(out) - cur, "%s\n", name);
    if (r < 0 || cur + (size_t)r >= sizeof(out)) { close(fd); errno = E2BIG; return -1; }
  }
  /* ftruncate 不清文件偏移（POSIX：offset 不变）：读循环后 offset 停在
     旧 EOF，直接 write 会在原位置落盘、文件头留下 NUL 空洞（2026-10-18
     真机: 二次 resident_sync 后文件 = NUL×14 + 名字行，且每次再部署
     追加 14B，至 512B 上限后同步 E2BIG、开机名单失效）。写前必须回 0。 */
  if (ftruncate(fd, 0) != 0 ||
      lseek(fd, 0, SEEK_SET) != 0 ||
      write(fd, out, strlen(out)) != (off_t)strlen(out) ||
      fchmod(fd, 0600) != 0 || fsync(fd) != 0) { close(fd); return -1; }
  close(fd);
  /* 同步重渲染 init.sh 托管块（尽力：失败不影响名单本身，下次同步重试） */
  (void)resident_apply_block();
  return 0;
}
/* webd 启动自愈：按现存名单逐个 resident_sync(name,1) ——
 * 整表重写（NUL 空洞文件自愈）+ 重渲染 init.sh 托管块。
 * 覆盖两种断档：旧版 webd 留下的损坏名单；刷机后 /data 的 init.sh
 * 还是无托管块的旧模板（webd 在 S50 起，早于 S60 播种/exec）。
 * 名单文件缺失/为空 → 不动 init.sh（新刷机由 S10 播种新模板自带空块）。 */
static void resident_heal_boot(void) {
  static char buf[1024];
  int fd, r;
  off_t sz;
  size_t n;
  fd = open("/data/lx06/keeper/resident", O_RDONLY);
  if (fd < 0) return;
  sz = lseek(fd, 0, SEEK_END);
  if (sz <= 0 || sz > (off_t)sizeof(buf) - 1) { close(fd); return; }
  lseek(fd, 0, SEEK_SET);
  n = 0;
  while (n < (size_t)sz) {
    r = read(fd, buf + n, (size_t)sz - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  close(fd);
  buf[n] = 0;
  {
    const char *p = buf, *limit=buf+n;
    while (p<limit) {
      const char *e = p;
      char name[65];
      while (e<limit && *e != '\n') e++;
      if (resident_line_clean(p, (size_t)(e - p), name, sizeof(name))) {
        resident_sync(name, 1);
      }
      p = e<limit ? e+1 : e;
    }
  }
}
/* multipart 上传同时保留 /tmp 原件及 root 只读持久副本。
 * 不再对 /data 全盘做 snapshot；root 安装脚本必须人工迁移为 HOME 用户应用。 */
/* Windows CRLF 文本脚本：\r 会被 ash 解析进重定向目标/语法（redir error /
 * syntax error，2026-10-19 .166 真机实证：脚本 rc=2 触发 keeper 误入安全
 * 模式）→ 上传时文本脚本统一去 \r；ELF 二进制按魔数保留原样。
 * 返回的指针：原数据（ELF 原样）或新 malloc 缓冲（调用方 free）。 */
static unsigned char *app_upload_crstrip(const unsigned char *data, size_t len, size_t *out_len) {
  if (len >= 4 && memcmp(data, "\x7f" "ELF", 4) == 0) { *out_len = len; return (unsigned char *)data; }
  unsigned char *buf = malloc(len ? len : 1);
  if (!buf) { *out_len = 0; return NULL; }
  size_t o = 0;
  for (size_t i = 0; i < len; i++)
    if (data[i] != '\r') buf[o++] = data[i];
  *out_len = o;
  return buf;
}

#include "app-extension-runner.h"
#include "app-extension-api.h"

#define GREETING_STAT LX06_STATBUF
#define GREETING_FSTAT LX06_FSTAT
#include "../../runtime/common/script-greeting.h"

static int app_greeting_schedule(const char *name) {
  struct ext_schema *schema=calloc(1,sizeof *schema);
  if(!schema)return -1;
  int declared=ext_schema_load(name,schema)==1&&ext_greeting_field(schema)>=0;
  free(schema);
  if(!declared)return 0;
  if(access("/usr/lib/lx06/script-settings",X_OK))return -1;
  char script[sizeof APPPERSIST+65];
  snprintf(script,sizeof script,APPPERSIST "/%s",name);
  pid_t pid=fork();
  if(pid)return pid>0?1:-1;
  signal(SIGCHLD,SIG_DFL);
  int null=open("/dev/null",O_RDWR);
  if(null<0||dup2(null,0)<0||dup2(null,1)<0||dup2(null,2)<0)_exit(126);
  for(int i=3;i<1024;i++)close(i);
  execl("/usr/lib/lx06/script-settings","script-settings","greeting-upload",script,(char *)NULL);
  _exit(127);
}

static void h_apps_upload(int fd, const char *head, const char *body, size_t blen,
                          const char *query) {
  char ct[512], boundary[220], name[65], resp[640], marker[240], holder[128];
  const char *bp, *ep, *sep, *fname, *quote, *fdata, *tail;
  size_t boundary_len, flen, n;
  int runnow = query && strstr(query, "run=1"), d = -1, m = -1, locked = 0;
  unsigned mflags = 0;
  /* 上传即部署的常驻语义：profile=resident → 无 600s 期限（daemon 存活的
     部署脚本不会在 10 分钟后触发 job-timeout 拉闸 safe_mode）。 */
  unsigned runflags = query && strstr(query, "profile=resident") ? APP_RESIDENT : 0;
  int status = 500;
  const char *error = "写入失败，已保存的文件保留供排查";
  long dfree, pid = 0;
  hdr_get(head, "Content-Type", ct, sizeof(ct));
  bp = strstr(ct, "boundary=");
  if (strncasecmp(ct, "multipart/", 10) || !bp) { errno = EINVAL; app_http_error(fd, 400, "需要 multipart boundary"); return; }
  bp += 9;
  if (*bp == '"') { bp++; ep = strchr(bp, '"'); }
  else { ep = bp + strcspn(bp, "; \t\r\n"); }
  if (!ep || ep == bp || (boundary_len = (size_t)(ep - bp)) >= sizeof(boundary)) {
    errno = EINVAL; app_http_error(fd, 400, "boundary 不合法"); return;
  }
  memcpy(boundary, bp, boundary_len); boundary[boundary_len] = 0;
  sep = memmem(body, blen < MAXHDR ? blen : MAXHDR, "\r\n\r\n", 4);
  if (!sep) { errno = EINVAL; app_http_error(fd, 400, "上传头损坏"); return; }
  fname = memmem(body, (size_t)(sep - body), "filename=\"", 10);
  if (!fname) { errno = EINVAL; app_http_error(fd, 400, "未收到文件名"); return; }
  fname += 10;
  quote = memchr(fname, '"', (size_t)(sep - fname));
  if (!quote || (n = (size_t)(quote - fname)) >= sizeof(name)) {
    errno = EINVAL; app_http_error(fd, 400, "文件名过长或损坏"); return;
  }
  memcpy(name, fname, n); name[n] = 0;
  if (!app_name_safe(name)) { errno = EINVAL; app_http_error(fd, 400, "名称不合法（字母数字._-，最长64字节）"); return; }
  if (ext_running(name) != 0) { errno = EBUSY; app_http_error(fd, 409, "该脚本的扩展操作尚未结束，请稍后更新"); return; }
  fdata = sep + 4;
  snprintf(marker, sizeof(marker), "\r\n--%s", boundary);
  tail = memmem(fdata, blen - (size_t)(fdata - body), marker, strlen(marker));
  if (!tail || (size_t)(body + blen - tail) != strlen(marker) + 4 ||
      memcmp(tail + strlen(marker), "--\r\n", 4) != 0) {
    errno = EINVAL; app_http_error(fd, 400, "只支持一个完整文件，上传尾损坏"); return;
  }
  flen = (size_t)(tail - fdata);
  if (!flen) { errno = EINVAL; app_http_error(fd, 400, "文件为空，没有上传内容"); return; }
  /* root 身份自声明（通用标记，见 app_marker_flags）+ 上传参数 isroot=1 简写。 */
  mflags = app_marker_flags((const unsigned char *)fdata, flen);
  /* WT1 §2.7 / §2.5. Official-on keeps today's upload path. */
  {
    const char *why = NULL;
    if (lx06_exit_phrases_ok(fdata, flen, lx06_official_kws_enabled(), &why) != 0) {
      errno = EINVAL;
      app_http_error(fd, 400, why ? why : "结束说法不能留空");
      return;
    }
    if (lx06_phrase_conflicts(fdata, flen, name, APPPERSIST)) {
      errno = EEXIST;
      app_http_error(fd, 409, "与已安装脚本的说法冲突，请换个说法");
      return;
    }
  }
  if (query && strstr(query, "isroot=1"))
    mflags |= APP_RUN_ROOT | APP_INSTALL_ROOT;
  /* 名称与形态统一（用户定案：每个 job 同一套规则）：名称合法字符即可
   * （app_name_safe），内容不限 .sh/ELF——启动方式按文件头判定（ELF/shebang
   * 直执行、其余 /bin/sh 解释，见 run_app_profile）。旧 root 安装脚本的
   * 不兼容判断在部署/运行期按 job 槽位语义给出，不再用扩展名一刀切。 */
  {
    long tfree;
    if (fs_space_kb("/tmp", NULL, NULL, &tfree) != 0) { app_http_error(fd, 507, "无法检查tmp空间"); return; }
    if (getenv("WEBD_TMP_FREE_KB")) tfree = atol(getenv("WEBD_TMP_FREE_KB"));
    if (tfree < (long)(flen / 1024) + 512) {
      fprintf(stderr, "webd upload %s: 507 tmp flen_kb=%ld tfree_kb=%ld\n", name, (long)(flen / 1024), tfree);
      fflush(stderr);
      errno = ENOSPC; app_http_error(fd, 507, "tmp空间不够"); return;
    }
  }
  dfree = data_free_kb();
  if (dfree < (long)(flen / 1024) + MIN_DATA_FREE_KB) {
    fprintf(stderr, "webd upload %s: 507 data flen_kb=%ld dfree_kb=%ld min_keep=%ld\n",
            name, (long)(flen / 1024), dfree, MIN_DATA_FREE_KB);
    fflush(stderr);
    errno = ENOSPC; app_http_error(fd, 507, "data空间不足"); return;
  }
  d = app_trusted_dir(APPDIR, 0700, 1);
  if (d < 0) goto fail;
  close(d); d = app_trusted_dir(APPPERSIST, 0755, 1);
  if (d < 0) goto fail;
  close(d); d = app_trusted_dir(APPLOGDIR, 0755, 1);
  if (d < 0) goto fail;
  close(d); d = -1;
  if (deploy_lock_active(holder, sizeof(holder))) { errno = EBUSY; status = 409; error = "管理操作进行中，请稍后重试"; goto fail; }
  if (deploy_lock_take(name) != 0) { status = 409; goto fail; }
  locked = 1;
  if (app_settings_begin() != 0) { status = 409; error = "配置或脚本正在读取/保存，请稍后重试"; goto fail; }
  m = app_legacy(name);
  if (m < 0) goto fail;
  if (m) { errno = EPROTONOSUPPORT; status = 409; error = "旧root部署须人工迁移清理，不会回退运行安装器"; goto fail; }
  /* 覆盖已有受管程序前必须确认它不在运行；keeper 不在时不猜停净。 */
  m = app_trusted_dir(APPPERSIST, 0755, 0);
  if (m < 0) goto fail;
  {
    LX06_STATBUF st;
    int exists = LX06_STATAT(m, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists && (!keeper_status_read(1) || app_running(name))) {
      errno = EBUSY; status = 409; error = "程序运行中或keeper不可读，拒绝覆盖"; goto fail;
    }
  }
  /* 流程（用户定）：上传落 /tmp（运行区）→ 自动部署 /tmp→/data（持久区）→
   * 部署成功后删除 /tmp 里的脚本；部署失败时 tmp 文件保留供排查/重试。 */
  d = app_trusted_dir(APPDIR, 0700, 0);
  if (d < 0) goto fail;
  {
    size_t dlen = flen;
    unsigned char *dcontent = app_upload_crstrip((const unsigned char *)fdata, flen, &dlen);
    if (!dcontent) { status = 500; error = "内存不足，无法处理上传内容"; close(d); d = -1; goto fail; }
    if (dcontent != (unsigned char *)fdata)
      fprintf(stderr, "webd upload %s: CRLF->LF stripped (flen %zu -> %zu)\n", name, flen, dlen);
    if (app_atomic(d, name, (const char *)dcontent, dlen, 0755) != 0) {
      int e = errno;
      if (dcontent != (unsigned char *)fdata) free(dcontent);
      status = e == ENOSPC ? 507 : 500;
      error = e == ENOSPC ? "tmp空间不够" : "写入tmp失败，请重试";
      close(d); d = -1; goto fail;
    }
    close(d); d = -1;
    if (app_copy_from_tmp(m, name, dlen) != 0) {
      int e = errno;
      if (dcontent != (unsigned char *)fdata) free(dcontent);
      status = e == ENOSPC ? 507 : 500;
      error = e == ENOSPC ? "data空间不足" : "部署到data失败，tmp中已保留上传文件供排查";
      goto fail;
    }
    if (dcontent != (unsigned char *)fdata) free(dcontent);
  }
  close(m); m = -1;
  if (appmeta_append(name) != 0) goto fail;
  if (app_remove_at(APPDIR, 0700, name) != 0) {
    status = 500;
    error = "已部署到data，但tmp中的脚本删除失败，请重试上传或重启清理";
    goto fail;
  }
  app_settings_end(); /* Keep deploy lock across the original keeper handoff. */
  int greeting_job=app_greeting_schedule(name);
  if (runnow && lx06_scripts_allowed()) {
    /* 首次运行=部署执行（安装阶段）：INSTALL 或 RUN 标记任一 → root；
     * 之后的手动/开机运行只看 RUN 标记（见 resident_app_exec_line/手动 run）。 */
    if (mflags & (APP_RUN_ROOT | APP_INSTALL_ROOT)) runflags |= LK_ROOT;
    pid = run_app_profile(name, NULL, runflags);
    if (pid <= 0) { status = 503; error = "keeper启动失败，running=0；脚本保留供排查，禁止root兼容回退"; goto fail; }
    if (runflags & APP_RESIDENT) resident_sync(name, 1); /* 登记开机常驻名单（尽力） */
  }
  deploy_lock_release(name);
  snprintf(resp, sizeof(resp),
    "{\"ok\":true,\"name\":\"%s\",\"size\":%lu,\"running\":%d,\"pid\":%ld,\"data_free_kb\":%ld,"
    "\"root\":%d,\"greeting_job\":%d,"
    "\"note\":\"%s\"}",
    name, (unsigned long)flen, pid > 0, pid, dfree,
    (mflags & (APP_RUN_ROOT | APP_INSTALL_ROOT)) ? 1 : 0, greeting_job,
    (mflags & (APP_RUN_ROOT | APP_INSTALL_ROOT))
      ? "脚本声明root（LX06-RUN:root/isroot=1）：root安装+root运行，无独立UID隔离，其余保护不变"
      : "用户隔离执行，HOME=/data/lx06/user/<name>；root需求请在脚本加 # LX06-RUN: root");
  send_json(fd, resp); return;
fail:
  { int e = errno;
    if (d >= 0) close(d);
    if (m >= 0) close(m);
    app_settings_end();
    if (locked) deploy_lock_release(name);
    errno = e;
  }
  app_http_error(fd, status, error);
}

static int app_log_open(const char *path, int flags) {
  const char *base;
  char name[65];
  size_t n;
  int d, f, create = (flags & O_CREAT) != 0, trunc = (flags & O_TRUNC) != 0;
  if (strncmp(path, APPLOGDIR "/", strlen(APPLOGDIR) + 1))
    return open(path, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644);
  base = path + strlen(APPLOGDIR) + 1;
  n = strlen(base);
  if (n > 6 && !strcmp(base + n - 6, ".log.1")) n -= 6;
  else if (n > 4 && !strcmp(base + n - 4, ".log")) n -= 4;
  else { errno = EINVAL; return -1; }
  if (n >= sizeof(name)) { errno = EINVAL; return -1; }
  memcpy(name, base, n); name[n] = 0;
  if (!app_name_safe(name)) { errno = EINVAL; return -1; }
  d = app_trusted_dir(APPLOGDIR, 0755, create);
  if (d < 0) return -1;
  f = app_open_file(d, base, flags & ~(O_CREAT | O_TRUNC));
  if (f < 0 && errno == ENOENT && create)
    f = openat(d, base, (flags & ~O_TRUNC) | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
  { int e = errno; close(d); errno = e; }
  if (f < 0) return -1;
  if (app_regular(f) != 0 || (trunc && ftruncate(f, 0) != 0)) { int e = errno; close(f); errno = e; return -1; }
  return f;
}

/* ---- fix15 日志管理：64K 硬上限轮转 + 开关/清理 ----
   轮转语义（单 .1 后缀，双文件合计 ≤128K）：
   - 当前 >64K：先把被截掉的旧段写进 .1（覆盖旧段，.1 永远只是「上一段」），
     再把当前文件尾部原地覆盖到文件头 + ftruncate 到 64K。
   - 全程不 rename：inode 不变 → O_APPEND 持有 fd 的长驻写者（dbwrap 的
     dropbear 等系统写者）下一笔追加自动从新 EOF 续写，不丢字节。
   扫描频率：主循环每 tick（5s select 超时）一次，12 个文件 stat 开销可忽略。 */
static void log_rotate(const char *p) {
  long sz = file_size(p);
  long skip, tail, off;
  char rot[590];
  int fd;
  if (sz <= (long)LOGROT_MAX || sz < 0) return;
  skip = sz - (long)LOGROT_MAX;        /* 要被截掉的前段长度 */
  tail = (long)LOGROT_MAX;             /* 保留的尾部长度 */
  /* 1) 旧段 [0,sz-64K) 的尾部 64K → .1（.1 同样硬限 64K：当前 64K + .1 64K
     = 双文件合计 128K 恒定，永不超） */
  snprintf(rot, sizeof(rot), "%s.1", p);
  {
    long start = skip - (long)LOGROT_MAX;
    if (start < 0) start = 0;
    fd = app_log_open(p, O_RDONLY);
    if (fd >= 0) {
      int rfd = app_log_open(rot, O_WRONLY | O_CREAT | O_TRUNC);
      if (rfd >= 0) {
        char b[4096];
        if (lseek(fd, start, SEEK_SET) == start) {
          long rem = skip - start;
          while (rem > 0) {
            long n = read(fd, b, (rem > 4096) ? 4096 : rem);
            if (n <= 0) break;
            { long w = 0; while (w < n) { long m = write(rfd, b + w, n - w); if (m <= 0) break; w += m; } }
            rem -= n;
          }
        }
        close(rfd);
      }
      close(fd);
    }
  }
  /* 2) 尾部原地下移：尾部在 [skip, sz)（长 tail），覆盖到 [0, tail)。
     pread 显式偏移读、pwrite 显式偏移写，不动 fd 位置（O_APPEND 写者无感）。 */
  fd = app_log_open(p, O_RDWR);
  if (fd < 0) return;
  off = 0;
  {
    char b[4096];
    long rem = tail;
    while (rem > 0) {
      long n = pread(fd, b, (rem > 4096) ? 4096 : rem, (off_t)(skip + off));
      if (n <= 0) break;
      { long w = 0; while (w < n) { long m = pwrite(fd, b + w, n - w, (off_t)(off + w)); if (m <= 0) break; w += m; } }
      off += n;
      rem -= n;
    }
  }
  if (ftruncate(fd, (off_t)LOGROT_MAX) != 0) { /* 截不到 64K 也尽量截 */ }
  close(fd);
}
/* 开关/清理：path 必须是 LOGDIR 下 *.log（调用方已限定类型，防路径注入）。
   同样只截断不 rename，保持 inode（持 fd 写者续写安全）。 */
static void log_set_off(const char *p) {
  /* 关：截断为 0 + 标记行。后续新内容=「关→开」之间的量，仍受 64K 轮转约束。 */
  int fd = app_log_open(p, O_WRONLY | O_CREAT);
  if (fd < 0) return;
  ftruncate(fd, 0);
  {
    const char *m = "\n[log off] 日志写入已暂停（web 日志开关=关）\n";
    long w = 0; size_t l = strlen(m);
    while ((size_t)w < l) { long n = write(fd, m + w, l - (size_t)w); if (n <= 0) break; w += n; }
  }
  close(fd);
}
static void log_clear_file(const char *p) {
  int fd = app_log_open(p, O_WRONLY | O_CREAT);
  if (fd < 0) return;
  ftruncate(fd, 0);
  close(fd);
}
/* 扫描 LOGDIR 全部 *.log（含 ssh-auth/webd-req/webd/各 app 部署日志）：
   - 开关=off（conf log_*=0）→ 每 tick 截回 0（写者继续追加也无所谓，5s 内
     又清零，净效果=日志停更，空间恒定）；
   - 开关=on → >64K 轮转。
   文件名→conf key 映射：ssh-auth.log→log_ssh、webd-req.log→log_web、
   webd.log→log_webd、<name>.log→log_app_<name>。 */
static void log_key_for(const char *fname, char *key, size_t keysz) {
  size_t l = strlen(fname);
  if (strcmp(fname, "ssh-auth.log") == 0) snprintf(key, keysz, "log_ssh");
  else if (strcmp(fname, "webd-req.log") == 0) snprintf(key, keysz, "log_web");
  else if (strcmp(fname, "webd.log") == 0) snprintf(key, keysz, "log_webd");
  else if (l > 4) {
    char base[140];
    size_t n = l - 4;                /* 去 .log 后缀 = app 名 */
    if (n > 139) n = 139;
    memcpy(base, fname, n);
    base[n] = 0;
    snprintf(key, keysz, "log_app_%s", base);
  } else
    key[0] = 0;
}
static void log_sweep(void) {
  const char *dirs[] = {LOGDIR, RUNLOGDIR, APPLOGDIR};
  unsigned pass;
  for (pass = 0; pass < 3; pass++) {
    int df = app_trusted_dir(dirs[pass], 0755, 0);
    DIR *d = df < 0 ? NULL : fdopendir(df);
    struct dirent *de;
    char full[600], key[200], dv[8];
    if (!d) { if (df >= 0) close(df); continue; }
    while ((de = readdir(d)) != NULL) {
      const char *e = de->d_name;
      size_t l = strlen(e);
      if (l <= 4 || l >= 70 || strcmp(e + l - 4, ".log") || e[0] == '.') continue;
      snprintf(full, sizeof(full), "%s/%s", dirs[pass], e);
      if (pass == 2) {
        char name[65];
        memcpy(name, e, l - 4); name[l - 4] = 0;
        if (!app_name_safe(name)) continue;
        snprintf(key, sizeof(key), "log_app_%s", name);
      } else log_key_for(e, key, sizeof(key));
      if (key[0] && conf_get(CONF, key, dv, sizeof(dv)) == 0 && dv[0] == '0') {
        if (file_size(full) > 0) log_clear_file(full);
      } else if (pass != 2) log_rotate(full);
      /* keeper 的 app 日志由 daemon 持续硬限 64KiB；网页不能靠轮询充当限额。 */
    }
    closedir(d);
  }
}
static void h_apps_log(int fd, const char *query) {
  char name[65] = "", logp[560], esc[132000], js[135000], buf[65536];
  char logkey[90], dv[8], source[1120];
  const char *note="";
  long offset = 0, sz, got;
  int running, on = 1, lf;
  FILE *f;
  const char *p;
  int i;
  p = query ? strstr(query, "name=") : NULL;
  if (p) {
    p += 5;
    for (i = 0; p[i] && p[i] != '&' && p[i] != '"' && i < 64; i++) name[i] = p[i];
    name[i] = 0;
    if (p[i] && p[i] != '&') { errno = EINVAL; app_http_error(fd, 400, "名称过长或损坏"); return; }
  }
  p = query ? strstr(query, "offset=") : NULL;
  if (p) offset = atol(p + 7);
  /* raw=1：读本 job 自身运行日志（keeper 落盘 <name>.log）；默认走 runlog
   * 链接（脚本 logfile= 声明，如小智对话日志）。部署/排障看 job 日志时用 raw。 */
  int raw = query && strstr(query, "raw=1") != NULL;
  /* rotated：调用方 offset 超出当前文件大小（新一轮运行截断了日志/轮转）→
   * 通知前端从 0 重新跟随，避免停在旧 offset 上「空日志+done」收场。 */
  int rotated = 0;
  if (!app_name_safe(name)) {
    send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"名称不合法\"}");
    return;
  }
  log_path_for(name, logp, sizeof(logp));
  int declared=raw ? 0 : app_runlog_path(name, logp, sizeof(logp));
  if (declared<0) note="日志路径声明无法解析，显示脚本运行日志；请在脚本顶部使用 logfile=/绝对路径";
  running = app_running(name);
  snprintf(logkey, sizeof(logkey), "log_app_%s", name);
  if (conf_get(CONF, logkey, dv, sizeof(dv)) == 0 && dv[0] == '0') on = 0;
  lf = app_log_open(logp, O_RDONLY);
  if (lf>=0 && app_regular(lf)!=0) { close(lf); lf=-1; }
  if (lf<0 && declared>0) {
    note="指定日志文件暂不可读，已回退到脚本运行日志";
    log_path_for(name,logp,sizeof(logp));
    lf=app_log_open(logp,O_RDONLY);
    if (lf>=0 && app_regular(lf)!=0) { close(lf); lf=-1; }
  }
  if (lf<0) note="日志尚未生成或不可读，请确认脚本已运行并输出日志";
  json_escape(logp,strlen(logp),source,sizeof(source));
  sz = lf < 0 ? 0 : (long)lseek(lf, 0, SEEK_END);
  if (sz < 0) sz = 0;
  if (offset < 0) offset = 0;
  if (offset > sz) { rotated = 1; offset = 0; }
  got = 0;
  f = lf < 0 ? NULL : fdopen(lf, "rb");
  if (!f && lf >= 0) close(lf);
  if (f) {
    fseek(f, offset, SEEK_SET);
    got = (long)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
  }
  if (got < 0) got = 0;
  buf[got] = 0;
  json_escape(buf, (size_t)got, esc, sizeof(esc));
  /* done 语义（与前端增量轮询配合）：
     - 运行中：恒 false → 前端持续跟随（watcher 类长驻脚本事件稀疏，
       不能按「本轮读满」停轮询，否则漏后续输出）；
     - 已退出：读到文件尾（offset+got>=sz）即 true → 前端停轮询收尾。 */
  {
    LX06_STATBUF lst;
  int active = 0;
  if (LX06_STAT(logp, &lst) == 0 && time(NULL) - lst.st_mtime < 15) active = 1;
  /* 运行中持续跟随；被链接的日志（如对话日志）只要 15s 内有更新也持续
     跟随 → 连续对话可实时看到，写完静默 15s 后自动停轮询。 */
  int isdone = running ? 0 : (offset + got >= sz && !active ? 1 : 0);
    snprintf(js, sizeof(js),
      "{\"ok\":true,\"offset\":%ld,\"next\":%ld,\"done\":%s,"
      "\"size\":%ld,\"on\":%d,\"rotated\":%d,\"copy_codes\":%d,\"source\":\"%s\",\"note\":\"%s\",\"data\":\"%s\"}",
      offset, offset + got, isdone ? "true" : "false", sz, on, rotated, !!(app_file_marker_flags(name)&APP_COPY_CODES),source, note, esc);
  }
  send_json(fd, js);
}
#include "app-log-feed.h"
static void h_apps_run(int fd, const char *body) {
  if(!lx06_scripts_allowed()){app_http_error(fd,403,"用户脚本开关已关闭，禁止调用");return;}
  char name[65], args[LK_ARGC * LK_ARG_LEN], profile[40];
  unsigned flags = 0;
  long pid;
  if (app_json_str(body, "name", name, sizeof(name)) != 0 ||
      app_json_str(body, "args", args, sizeof(args)) != 0 ||
      app_json_str(body, "profile", profile, sizeof(profile)) != 0 || !app_name_safe(name)) {
    errno = EINVAL; app_http_error(fd, 400, "名称/参数无效或过长，拒绝截断执行"); return;
  }
  /* route 已先校验登录；宽 VM/驻留/root 必须由管理者在本次请求明确授权
   * （root 亦可由部署脚本标记自声明，下方 OR 入）。 */
  if (!profile[0] || !strcmp(profile, "default")) flags = 0;
  else if (!strcmp(profile, "wide-vm")) flags = LK_WIDE_VM;
  else if (!strcmp(profile, "root")) flags = LK_ROOT;
  else if (!strcmp(profile, "resident")) flags = APP_RESIDENT;
  else if (!strcmp(profile, "resident-wide-vm")) flags = APP_RESIDENT | LK_WIDE_VM;
  else if (!strcmp(profile, "resident-root")) flags = APP_RESIDENT | LK_ROOT;
  else if (!strcmp(profile, "resident-root-wide-vm")) flags = APP_RESIDENT | LK_ROOT | LK_WIDE_VM;
  else { errno = EINVAL; app_http_error(fd, 400, "未知profile，未启动"); return; }
  /* 脚本自声明 root：RUN 标记 → 该应用一律 root 执行（不独立 UID），
   * 与 profile 授权叠加，不放宽 VM 限制；INSTALL 标记只对首次（部署）
   * 运行生效，手动再运行回独立身份（root 安装 + 非 root 运行正交）。 */
  if (app_file_marker_flags(name) & APP_RUN_ROOT)
    flags |= LK_ROOT;
  pid = run_app_profile(name, args, flags);
  if (pid <= 0) {
    int e = errno;
    app_http_error(fd, e == EINVAL || e == E2BIG || e == ENOEXEC ? 400 : e == EBUSY || e == EALREADY ? 409 : 503,
                   "keeper拒绝启动或文件缺失；旧root部署须人工迁移，不会放宽限制重试");
    return;
  }
  if (flags & APP_RESIDENT) resident_sync(name, 1); /* 登记开机常驻名单（尽力） */
  {
    char resp[100];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"running\":1,\"pid\":%ld}", pid);
    send_json(fd, resp);
  }
}
/* POST /api/keeper/recover：Web 解除安全模式入口（等价 SSH lx06-keeper recover）。
   能否解除由 daemon 判定（无活动 job 且保留 UID 无残留进程）；
   收到 recover 后再读一次状态，返回真实结果，前端不得乐观假设。 */
static void h_keeper_recover(int fd) {
  struct lk_request q;
  struct lk_reply r;
  char js[768], er[200];
  memset(&q, 0, sizeof(q));
  q.magic = LK_MAGIC; q.action = LK_RECOVER;
  keeper_invalidate();
  if (lk_call(&q, &r, 800) != 0) { app_http_error(fd, 503, "解除失败：keeper 未运行，或仍有用户程序进程（先停止再试）"); return; }
  memset(&q, 0, sizeof(q));
  q.magic = LK_MAGIC; q.action = LK_STATUS;
  if (lk_call(&q, &r, 500) != 0) { app_http_error(fd, 503, "安全模式已解除，但状态回读失败"); return; }
  r.reason[sizeof(r.reason) - 1] = 0;
  json_escape(r.reason, strlen(r.reason), er, sizeof(er));
  snprintf(js, sizeof(js), "{\"ok\":true,\"safe\":%u,\"reason\":\"%s\",\"jobs\":%u}",
    r.safe_mode, er, r.jobs);
  send_json(fd, js);
}
static void h_apps_kill(int fd, const char *body) {
  char name[65];
  if (app_json_str(body, "name", name, sizeof(name)) != 0 || !app_name_safe(name)) {
    errno = EINVAL; app_http_error(fd, 400, "名称不合法"); return;
  }
  if (kill_app(name) != 0) { app_http_error(fd, errno == EBUSY ? 409 : 503, "keeper未确认停净，记录保留；请稍后重试"); return; }
  deploy_lock_release(name);
  send_json(fd, "{\"ok\":true,\"running\":0}");
}
/* 脚本前 64 行若有 "# WEBD-UNINSTALL-TIMEOUT: N"（N 钳到 1..60），
 * 以 root 限时执行 /bin/sh <script> --uninstall。无标记返回 0，成功 1，失败 -1。
 * 不删脚本本身。只接受 root 属主的普通文件，不跟随符号链接。 */
static void ww_repair_missing_links(void);
static void ww_log(const char *fmt, ...);
static int app_declared_uninstall(const char *script) {
  if(!lx06_scripts_allowed())return 0; /* Firmware cleanup only while disabled. */
  FILE *f;
  char line[256];
  LX06_STATBUF st;
  int nlines = 0, sec = 0, found = 0, stcode = 0, i;
  pid_t pid, w;
  if (LX06_STATAT(AT_FDCWD, script, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(st.st_mode) || st.st_uid != 0) return 0;
  f = fopen(script, "r");
  if (!f) return 0;
  while (nlines < 64 && fgets(line, sizeof(line), f)) {
    char *p = line;
    int v = 0;
    nlines++;
    if (strncmp(p, "# WEBD-UNINSTALL-TIMEOUT:", 25) != 0) continue;
    p += 25;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') continue;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      if (v > 60) { v = 60; break; }
      p++;
    }
    if (v < 1) v = 1;
    sec = v;
    found = 1;
    break;
  }
  fclose(f);
  if (!found) return 0;
  /* webd 平时 SIG_IGN(SIGCHLD) 会自动收尸，waitpid 拿不到退出码。
     这段临时改回默认，等完再恢复。 */
  signal(SIGCHLD, SIG_DFL);
  pid = fork();
  if (pid < 0) { signal(SIGCHLD, SIG_IGN); return -1; }
  if (pid == 0) {
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
      if (fd > 2) close(fd);
    }
    setpgid(0, 0);
    alarm((unsigned)sec);
    execl("/bin/sh", "sh", script, "--uninstall", (char *)0);
    _exit(127);
  }
  setpgid(pid, pid);
  for (i = 0; i < sec * 10 + 20; i++) {
    w = waitpid(pid, &stcode, WNOHANG);
    if (w == pid) {
      signal(SIGCHLD, SIG_IGN);
      return (WIFEXITED(stcode) && WEXITSTATUS(stcode) == 0) ? 1 : -1;
    }
    if (w < 0 && errno != EINTR) { signal(SIGCHLD, SIG_IGN); return -1; }
    usleep(100000);
  }
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  waitpid(pid, &stcode, 0);
  signal(SIGCHLD, SIG_IGN);
  return -1;
}

static void h_apps_uninstall(int fd, const char *body) {
  char name[65], holder[128], logname[100], oldname[100], linkp[560] = "";
  struct lk_request q;
  struct lk_reply r;
  int d, legacy, locked = 0, cleanup = 0;
  unsigned count = 0;
  if (app_json_str(body, "name", name, sizeof(name)) != 0 || !app_name_safe(name)) {
    errno = EINVAL; app_http_error(fd, 400, "名称不合法"); return;
  }
  if (ext_forget(name) != 0) {
    if (errno == EBUSY) send_json(fd, "{\"ok\":true,\"pending\":true,\"note\":\"正在停止扩展操作\"}");
    else app_http_error(fd, 503, "扩展操作清理失败，请重试");
    return;
  }
  if (deploy_lock_active(holder, sizeof(holder))) {
    if (strcmp(holder, name)) { errno = EBUSY; app_http_error(fd, 409, "其他管理操作进行中"); return; }
    locked = 1;
  } else if (deploy_lock_take(name) != 0) { app_http_error(fd, 409, "管理锁不可用"); return; }
  else locked = 1;
  legacy = app_legacy(name);
  if (legacy < 0) goto fail;
  memset(&q, 0, sizeof(q));
  q.magic = LK_MAGIC; q.action = LK_FORGET;
  strcpy(q.name, name);
  keeper_invalidate();
  /* One bounded RPC per HTTP request. Waiting/sleeping here blocks the entire
     Web loop and can trigger portwatch. Browser retries pending work using the
     shared scheduler; keeper must confirm cleanup before success is reported. */
  {
    if (lk_call(&q, &r, 500) != 0) {
      if (errno != EBUSY) {
        /* 只上传未启动/已经 forget 的名称没有 slot；读回状态后才允许幂等清记录。 */
        unsigned i;
        if (errno != ENOENT || !keeper_status_read(1)) goto fail;
        for (i = 0; i < keeper_cached.jobs; i++)
          if (!strncmp(keeper_cached.job[i].name, name, sizeof(keeper_cached.job[i].name))) { errno = EBUSY; goto fail; }
      } else {
        goto fail;
      }
    }
  }
  /* 先解析脚本声明的日志链接（依赖部署脚本仍在），随卸载清扫链接目标。
     有 # WEBD-UNINSTALL-TIMEOUT 的脚本，在删文件前限时跑 --uninstall。 */
  app_runlog_path(name, linkp, sizeof(linkp));
  {
    char script[160];
    snprintf(script, sizeof(script), APPPERSIST "/%s", name);
    if (access(script, R_OK) != 0)
      snprintf(script, sizeof(script), APPDIR "/%s", name);
    if (access(script, R_OK) == 0)
      cleanup = app_declared_uninstall(script);
  }
  /* Cleanup keeps its original lifecycle and runs without the settings lock.
   * Commit admission below is nonblocking. A busy reader leaves source bytes,
   * saved values and appmeta intact; the existing pending response retries. */
  if (app_settings_begin() != 0) goto fail;
  d=app_trusted_dir(LX06_GREETING_DIR,0700,0);
  if(d>=0) {
    unsigned assets=0;
    int removed=app_remove_tree(d,name,0,&assets);
    if(!removed)removed=fsync(d);
    int e=errno;close(d);errno=e;
    if(removed)goto fail;
  } else if(errno!=ENOENT)goto fail;
  if (app_remove_at(APPDIR, 0700, name) != 0 || app_remove_at(APPPERSIST, 0755, name) != 0) goto fail;
  ww_repair_missing_links();
  snprintf(logname, sizeof(logname), "%s.log", name);
  if (app_remove_at(APPLOGDIR, 0755, logname) != 0) goto fail;
  snprintf(logname, sizeof(logname), "%s.log.1", name);
  if (app_remove_at(APPLOGDIR, 0755, logname) != 0) goto fail;
  /* 清旧版管理日志/临时状态，但不信任其中的 PID，更不触碰业务目录。 */
  snprintf(logname, sizeof(logname), "%s.log", name);
  if (app_remove_at(LOGDIR, 0755, logname) != 0) goto fail;
  snprintf(logname, sizeof(logname), "%s.log.1", name);
  if (app_remove_at(LOGDIR, 0755, logname) != 0) goto fail;
  /* 清扫脚本声明的链接日志（仅受管日志目录内；best-effort 不阻塞）。 */
  app_remove_linklog(linkp);
  {
    const char *suffix[] = {"pid", "st", "rc"};
    unsigned i;
    for (i = 0; i < 3; i++) {
      snprintf(oldname, sizeof(oldname), ".%s.%s", name, suffix[i]);
      if (app_remove_at(APPDIR, 0700, oldname) != 0) goto fail;
    }
  }
  d = app_trusted_dir(BKDIR, 0700, 0);
  if (d < 0) { if (errno != ENOENT) goto fail; }
  else {
    int result = app_remove_tree(d, name, 0, &count), e = errno;
    if (!result && fsync(d) != 0) { result = -1; e = errno; }
    close(d); errno = e;
    if (result) goto fail;
  }
  if (appmeta_remove(name) != 0) goto fail; /* 最后清顺序表，部分失败仍可重试。 */
  app_settings_end();
  resident_sync(name, 0); /* 注销开机常驻名单（尽力；残留行开机时按文件缺失跳过） */
  {
    char bound[65];
    if (lx06_official_bind_read(bound, sizeof(bound)) == 1 && !strcmp(bound, name))
      lx06_official_bind_clear_at(LX06_OFFICIAL_BIND_PATH);
  }
  deploy_lock_release(name);
  {
    char js[480];
    const char *base = legacy
      ? "受管脚本、日志、workspace 与元数据已清"
      : "受管脚本、日志、workspace 与元数据已删除";
    const char *extra = cleanup == 1 ? "；已执行脚本清理"
      : cleanup < 0 ? "；脚本清理失败，业务目录可能仍在"
      : "；脚本未声明清理，业务目录未动";
    snprintf(js, sizeof(js),
      "{\"ok\":true,\"running\":0,\"cleanup\":%d,\"note\":\"%s%s\"}",
      cleanup == 1 ? 1 : 0, base, extra);
    send_json(fd, js);
  }
  return;
fail:
  { int e = errno; app_settings_end(); if (locked) deploy_lock_release(name); errno = e; }
  if (errno == EBUSY) {
    send_json(fd, "{\"ok\":true,\"pending\":true,\"note\":\"正在停止脚本并清理文件\"}"); return;
  }
  app_http_error(fd, errno == EBUSY ? 409 : 503, "keeper未确认停净或清理未完成，记录保留；请稍后重试，旧业务文件须人工清理");
}

/* --- 系统日志查看（fix15 恢复）：GET /api/logs?type=req|ssh|webd&offset=N
   页面「SSH日志 / Web日志」按钮用（免 SSH 拉日志）。
   req(web) = webd-req.log（HTTP 请求日志）；ssh = ssh-auth.log（dropbear -E）；
   webd = webd.log（88 侧启动/崩溃）。单页 64KB，offset 续读（与 h_apps_log
   同款分页协议）。size/on 供弹窗显示当前大小与开关状态。 --- */
#define LOGS_PAGE 65536
static void h_logs(int fd, const char *query) {
  char type[8] = "req", name[560], buf[LOGS_PAGE], esc[LOGS_PAGE * 2], js[LOGS_PAGE * 2 + 256];
  char dv[8] = "1", key[128];
  long offset = 0, sz, got;
  FILE *f;
  const char *p;
  int i, on = 1;
  p = query ? strstr(query, "type=") : NULL;
  if (p) {
    p += 5;
    for (i = 0; p[i] && p[i] != '&' && p[i] != '"' && i < 7; i++) type[i] = p[i];
    type[7] = 0;
  }
  p = query ? strstr(query, "offset=") : NULL;
  if (p) offset = atol(p + 7);
  if (strcmp(type, "ssh") == 0) {
    snprintf(name, sizeof(name), "%s", SSHLOG);
    snprintf(key, sizeof(key), "log_ssh");
  } else if (strcmp(type, "webd") == 0) {
    snprintf(name, sizeof(name), "%s", WEBDLOG);
    snprintf(key, sizeof(key), "log_webd");
  } else {
    snprintf(name, sizeof(name), "%s", REQLOG);
    snprintf(key, sizeof(key), "log_web");
  }
  conf_get(CONF, key, dv, sizeof(dv));
  on = (dv[0] != '0');   /* 缺省=开 */
  sz = file_size(name);
  if (sz < 0) sz = 0;
  if (offset < 0 || offset > sz) offset = sz;
  got = 0;
  f = fopen(name, "rb");
  if (f) {
    fseek(f, offset, SEEK_SET);
    got = (long)fread(buf, 1, LOGS_PAGE - 1, f);
    fclose(f);
  }
  if (got < 0) got = 0;
  buf[got] = 0;
  json_escape(buf, (size_t)got, esc, sizeof(esc));
  snprintf(js, sizeof(js),
    "{\"ok\":true,\"type\":\"%s\",\"offset\":%ld,\"next\":%ld,\"done\":%s,"
    "\"size\":%ld,\"on\":%d,\"data\":\"%s\"}",
    type, offset, offset + got, (offset + got >= sz) ? "true" : "false",
    sz, on, esc);
  send_json(fd, js);
}

/* --- 日志开关/清理（fix15）：POST /api/logs/ctrl {"type":"ssh|web|webd|app",
   "name":"<app>"(app 型必填), "action":"on|off|clear"}
   on/off → conf log_<type>=1/0（缺省=开）+ 立即截断/标记（off）；
   clear → 原地截断为 0（不清开关状态）。
   ssh 型 live 生效：off 同时 kill lx06unified 主进程 → procd 拉起新实例，
   新 dropbear 按 log_ssh=0 把 -E stderr 丢 /dev/null（见 lx06_unified.c）。 --- */
static void h_logctrl(int fd, const char *body) {
  char type[8] = "", name[65] = "", action[8] = "";
  char key[90], path[600];
  int need_restart = 0;
  json_str(body, "type", type, sizeof(type));
  if (app_json_str(body, "name", name, sizeof(name)) != 0) {
    errno = EINVAL; app_http_error(fd, 400, "名称过长或请求损坏"); return;
  }
  json_str(body, "action", action, sizeof(action));
  if (strcmp(type, "app") == 0) {
    if (!app_name_safe(name)) {
      send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"名称不合法\"}");
      return;
    }
    app_runlog_path(name, path, sizeof(path)); /* runlog 链接优先 */
    snprintf(key, sizeof(key), "log_app_%s", name);
  } else if (strcmp(type, "ssh") == 0) {
    snprintf(path, sizeof(path), "%s", SSHLOG);
    snprintf(key, sizeof(key), "log_ssh");
  } else if (strcmp(type, "web") == 0) {
    snprintf(path, sizeof(path), "%s", REQLOG);
    snprintf(key, sizeof(key), "log_web");
  } else if (strcmp(type, "webd") == 0) {
    snprintf(path, sizeof(path), "%s", WEBDLOG);
    snprintf(key, sizeof(key), "log_webd");
  } else {
    send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"类型不合法\"}");
    return;
  }
  if (strcmp(action, "on") == 0) {
    if (conf_set_key(key, "1") != 0) {
      send_err(fd, 500, "Server Error", "{\"ok\":false,\"error\":\"写入失败\"}");
      return;
    }
    need_restart = (strcmp(type, "ssh") == 0);
    send_json(fd, need_restart
      ? "{\"ok\":true,\"note\":\"已开启（SSH 服务将重启数秒以生效）\"}"
      : "{\"ok\":true,\"note\":\"已开启\"}");
  } else if (strcmp(action, "off") == 0) {
    if (conf_set_key(key, "0") != 0) {
      send_err(fd, 500, "Server Error", "{\"ok\":false,\"error\":\"写入失败\"}");
      return;
    }
    log_set_off(path);
    need_restart = (strcmp(type, "ssh") == 0);
    send_json(fd, need_restart
      ? "{\"ok\":true,\"note\":\"已关闭（SSH 服务将重启数秒以生效）\"}"
      : "{\"ok\":true,\"note\":\"已关闭（当前内容已截断，新内容暂停写入）\"}");
  } else if (strcmp(action, "clear") == 0) {
    log_clear_file(path);
    send_json(fd, "{\"ok\":true,\"note\":\"已清空\"}");
  } else {
    send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"动作不合法\"}");
    return;
  }
  if (need_restart) {
#ifdef LX06_WEBD_INTEGRATED
    /* 统一二进制：回包已发出 → 再重启 lx06unified 主进程（getppid：webd
       是其直接子进程）：svr_dropbear_exit 会带走 webd，procd 拉新实例
       （新 dropbear 按 log_ssh=0 把 -E stderr 丢 /dev/null）。
       页面短暂断开 3~6s 后自动恢复。 */
    usleep(300000);
    kill(getppid(), SIGTERM);
#else
    /* 独立进程：log_ssh 只影响 dropbear 侧的 dbwrap（启动时读 conf）
       → 重启 S50 dropbear 服务即可（webd 进程自身不受影响，页面不断开）。 */
    system("/etc/init.d/dropbear restart >/dev/null 2>&1");
#endif
  }
}

/* --- 诊断回传开关（S3）：
   GET  /api/diag           → 当前 diag_push 状态
   POST /api/diag {"on":1}  → 开；{"on":0} → 关
   关后 ssh-diag.sh 的 boot/dhcp_bound 触发点直接跳过推送（不推 9876）。
   修完 bug 想安静就关；要抓下次现场就开。默认开（刷机排查期）。 --- */
static void h_diag(int fd, const char *body) {
  char dv[8] = "1";
  int on = -1;
  /* 已按用户要求移除诊断回传（开发期功能）→ 501。函数体保留，取证时
     把下面两行删掉即可恢复。 */
  (void)body; (void)dv; (void)on;
  send_err(fd, 501, "Not Implemented", "{\"ok\":false,\"error\":\"诊断功能已移除\"}");
  return;
  if (conf_get(CONF, "diag_push", dv, sizeof(dv)) != 0) dv[0] = 0;  /* 缺省=开 */
  if (body) {
    /* 解析 "on": 0/1（支持数字与字符串两种形态，json_str 只认引号串） */
    const char *p = strstr(body, "\"on\"");
    if (p) {
      p = strchr(p, ':');
      if (p) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '0') on = 0;
        else if (*p == '1') on = 1;
        else if (*p == '"') {
          p++;
          if (*p == '0') on = 0;
          else if (*p == '1') on = 1;
        }
      }
    }
  }
  if (on < 0) {
    /* GET：只读当前状态 */
    send_json(fd, (dv[0] == '0') ? "{\"ok\":true,\"diag_push\":0}" : "{\"ok\":true,\"diag_push\":1}");
    return;
  }
  if (conf_set_key("diag_push", on ? "1" : "0") != 0) {
    send_err(fd, 500, "Server Error", "{\"ok\":false,\"error\":\"写入失败\"}");
    return;
  }
  send_json(fd, on ? "{\"ok\":true,\"diag_push\":1,\"note\":\"已开启诊断回传\"}"
                   : "{\"ok\":true,\"diag_push\":0,\"note\":\"已关闭诊断回传\"}");
}

#include "core-noise.h"

static int noise_route(const char *script, int enrollment, int raw) {
  FILE *f = fopen(script, "r");
  char line[1024];
  int reader = 0, consumer = 0, enrollment_args = 0, framed = enrollment, marker = 0, epoch = 0;
  if (!f) return 0;
  while (fgets(line, sizeof(line), f)) {
    if (*line == '#') continue;
    if (!strcmp(line, "reader=/usr/lib/lx06/xiaoai/processed-mic-read\n")) reader = 1;
    if (strstr(line, enrollment ? "\"$reader\" --name enrollment" : "\"$reader\" --name kws")) consumer = 1;
    if (!strncmp(line, "set -- --name enrollment ", 25)) enrollment_args = 1;
    if (enrollment && enrollment_args && strstr(line, "\"$reader\" \"$@\"")) consumer = 1;
    if (strstr(line, "--kws-framed")) framed = 1;
    if (strstr(line, "epoch-capture-v2")) marker = 1;
    if (strstr(line, "--kws-epoch")) epoch = 1;
  }
  fclose(f);
  return reader && consumer && (raw ? marker && epoch : framed);
}

#define WW_RECORD_SCRIPT "/usr/lib/lx06/wakeword/ww-record.sh"
#define WW_LISTEN_SCRIPT "/usr/lib/lx06/wakeword/watch-ww.sh"
static int noise_raw_selected(void) {
  return access("/usr/lib/lx06/wakeword/epoch-capture-v2", R_OK) == 0 &&
         (noise_route(WW_RECORD_SCRIPT, 1, 1) || noise_route(WW_LISTEN_SCRIPT, 0, 1));
}

/* GET 只读取 keeper 状态，不做巡检执法、kill、recover 或启动。
 * 执法独立于网页轮询，所有 job RSS/members 由 daemon 按专属 UID 聚合。 */
static void h_core(int fd) {
  char js[12288], reason[1000], line[256], engine_state[2] = "-";
  size_t off;
  long up = 0, mt = 0, ma = 0, dfree = -1, tfree = -1;
  long dtot=-1, dused=-1, ttot=-1, tused=-1;
  int pending=0;
  double cpu=cpu_usage_pct();
  int available = keeper_status_read(1), ssh = 0, engine = 0;
  int passive_tap = 0, capturing = 0;
  pid_t engine_pid = 0;
  double temp_c = -1;
  FILE *f;
  DIR *d;
  struct dirent *de;
  unsigned i;
  f = fopen("/proc/uptime", "r");
  if (f) { if (fscanf(f, "%ld", &up) != 1) up = 0; fclose(f); }
  f = fopen("/proc/meminfo", "r");
  if (f) {
    while (fgets(line, sizeof(line), f)) {
      if (!strncmp(line, "MemTotal:", 9)) mt = atol(line + 9);
      else if (!strncmp(line, "MemAvailable:", 13)) ma = atol(line + 13);
    }
    fclose(f);
  }
  (void)fs_space_kb("/data", &dtot, &dused, &dfree);
  (void)fs_space_kb("/tmp", &ttot, &tused, &tfree);
  d=opendir("/sys/fs/pstore");
  while(d && (de=readdir(d))!=NULL) {
    char path[320]; LX06_STATBUF st;
    if(de->d_name[0]=='.')continue;
    snprintf(path,sizeof(path),"/sys/fs/pstore/%s",de->d_name);
    if(LX06_STAT(path,&st)==0 && st.st_size>0)pending++;
  }
  if(d)closedir(d);
  /* 温度：依次读 thermal_zoneN/temp（毫摄氏度）。LX06 zone0=soc_thermal；
     无可用 zone 时输出 null，前端该行保持 …，不影响其余行。 */
  {
    int z;
    for (z = 0; z < 4 && temp_c < 0; z++) {
      char tpath[80], tbuf[40];
      FILE *tf;
      long mv;
      snprintf(tpath, sizeof(tpath), "/sys/class/thermal/thermal_zone%d/temp", z);
      tf = fopen(tpath, "r");
      if (!tf) continue;
      if (fscanf(tf, "%39s", tbuf) == 1 && (mv = atol(tbuf)) > 0 && mv < 300000) temp_c = (double)mv / 1000.0;
      fclose(tf);
    }
  }
  d = opendir("/proc");
  while (d && (de = readdir(d)) != NULL) {
    char path[320];
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
    f = fopen(path, "r");
    if (!f) continue;
    if (fgets(line, sizeof(line), f)) {
      line[strcspn(line, "\n")] = 0;
      if (!strcmp(line, "dropbear")) ssh = 1;
      if (!strcmp(line, "mipns-xiaomi")) {
        FILE *sf;
        char statline[1024], *rp;
        engine = 1;
        snprintf(path, sizeof(path), "/proc/%s/maps", de->d_name);
        pid_t candidate = (pid_t)atoi(de->d_name);
        int running_pcm = noise_capture(candidate);
        if (engine_pid && !running_pcm) { fclose(f); continue; }
        engine_pid = candidate; capturing = running_pcm; passive_tap = 0;
        engine_state[0] = '-';
        LX06_STATBUF tap;
        sf = fopen(path, "r");
        if (sf) {
          if (!LX06_STAT("/usr/lib/lx06/xiaoai/processed-mic-tap.so", &tap))
            passive_tap = noise_mapped(sf, &tap);
          fclose(sf);
        }
        snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
        sf = fopen(path, "r");
        if (sf) {
          if (fgets(statline, sizeof(statline), sf) && (rp = strrchr(statline, ')')) != NULL && rp[1] == ' ' && strchr("RSDZTtXxKWPI", rp[2])) engine_state[0] = rp[2];
          fclose(sf);
        }
      }
    }
    fclose(f);
  }
  if (d) closedir(d);
  int raw = noise_raw_selected();
  int reader = access("/usr/lib/lx06/xiaoai/processed-mic-read", X_OK) == 0;
  int rec = reader && noise_route(WW_RECORD_SCRIPT, 1, raw);
  int wake = reader && noise_route(WW_LISTEN_SCRIPT, 0, raw);
  uint64_t birth = engine_pid ? lx06_process_ticks(engine_pid) : 0;
  long hz = sysconf(_SC_CLK_TCK);
  int starting = birth && hz > 0 && up - (long)(birth / hz) < 6;
  int native = passive_tap && rec && wake && !raw && capturing;
  int64_t now = lk_now_ms();
  noise_start(native ? engine_pid : 0, native ? birth : 0, now);
  noise_probe.refresh_until = native ? now + 12000 : 0;
  if (native) {
    noise_tick(now);
    noise_prime(now);
    now = lk_now_ms();
  }
  int frames = native && noise_probe.sampled > 0 && now - noise_probe.sampled <= 6500;
  const char *source = !rec || !wake ? "unknown" : raw ? "raw-hpf" : "native-vpm";
  const char *state = !engine ? "stopped" : starting && (!passive_tap || !capturing) ? "starting" :
                      !rec || !wake ? "unconfigured" : !passive_tap ? "unattached" :
                      !capturing ? "idle" : raw ? "raw" : frames ? "active" : "waiting";
  /* reason 来自可信本地 daemon，但仍必须 JSON 转义且约束 NUL 边界。 */
  keeper_cached.reason[sizeof(keeper_cached.reason) - 1] = 0;
  {
    const char *rk = available ? keeper_cached.reason
                               : (keeper_ever_ok ? keeper_last.reason : "keeper-unavailable");
    json_escape(rk, strlen(rk), reason, sizeof(reason));
  }
  {
    char tjs[16];
    if (temp_c >= 0) snprintf(tjs, sizeof(tjs), "%.1f", temp_c);
    else snprintf(tjs, sizeof(tjs), "null");
    off = (size_t)snprintf(js, sizeof(js),
      "{\"ok\":true,\"uptime\":%ld,\"mem\":{\"total_kb\":%ld,\"avail_kb\":%ld},"
      "\"cpu\":%.1f,\"memory\":%.1f,\"mem_total_kb\":%ld,\"mem_avail_kb\":%ld,"
      "\"data_total_kb\":%ld,\"data_used_kb\":%ld,\"tmp_total_kb\":%ld,\"tmp_used_kb\":%ld,\"crash_pending\":%d,"
      "\"fs\":{\"data_free_kb\":%ld,\"tmp_free_kb\":%ld},"
      "\"core\":{\"ssh\":%d,\"webd\":1,\"engine\":%d,\"engine_state\":\"%s\",\"wdt\":%d,\"keeper\":%d,\"safe_mode\":%u,\"user_apps\":%u,\"temp_c\":%s,\"noise\":{\"source\":\"%s\",\"state\":\"%s\",\"mapped\":%d,\"capturing\":%d,\"frames_observed\":%d,\"record\":%d,\"threshold\":%d,\"wake\":%d,\"script\":null,\"enabled\":%d}},"
      "\"keeper\":{\"available\":%d,\"safe\":%u,\"reason\":\"%s\",\"rss_kb\":%llu,\"budget_kb\":%llu},\"apps\":[",
      up, mt, ma, cpu, mt>0 ? 100.0*(mt-ma)/mt : 0.0, mt, ma,
      dtot,dused,ttot,tused,pending,dfree,tfree,ssh, engine, engine_state, access("/dev/watchdog0", F_OK) == 0,
      available, available ? !!keeper_cached.safe_mode : 0, available ? keeper_cached.jobs : 0, tjs,
      source, state, passive_tap, capturing, frames, frames && rec, frames && rec, frames && wake, frames,
      available, available ? keeper_cached.safe_mode : (keeper_ever_ok ? keeper_last.safe_mode : 1), reason,
      (unsigned long long)keeper_cached.rss_kb, (unsigned long long)keeper_cached.budget_kb);
  }
  for (i = 0; available && i < keeper_cached.jobs; i++) {
    const struct lk_job_info *j = &keeper_cached.job[i];
    char name[400];
    size_t len = strnlen(j->name, sizeof(j->name));
    json_escape(j->name, len, name, sizeof(name));
    off += (size_t)snprintf(js + off, sizeof(js) - off,
      "%s{\"name\":\"%s\",\"pid\":%lld,\"rss_kb\":%llu,\"limit_kb\":%llu,\"members\":%u,\"forcever\":0}",
      i ? "," : "", name, (long long)j->pid, (unsigned long long)j->rss_kb,
      (unsigned long long)keeper_cached.budget_kb, j->members);
  }
  snprintf(js + off, sizeof(js) - off, "]}");
  send_json(fd, js);
}

/* GET /api/syslog?lines=N：/var/log/messages 尾部 N 行（默认150，上限800）。
   用户程序写坏系统时的第一现场（kernel oops / oom-killer / 设备错误）。 */
static void h_syslog(int fd, const char *query) {
  char esc[70000], js[71500], buf[65536];
  char dv[16];
  long lines = 150, sz = 0, tail, got;
  int n = 0, i;
  FILE *f;
  const char *p, *start;

  p = query ? strstr(query, "lines=") : NULL;
  if (p) {
    dv[0] = 0;
    for (i = 0; p[i] && p[i] != '&' && p[i] != '"' && i < 14; i++) dv[i] = p[i];
    dv[i] = 0;
    lines = atol(dv);
  }
  if (lines < 1) lines = 1;
  if (lines > 800) lines = 800;

  f = fopen("/var/log/messages", "rb");
  if (!f) {
    send_err(fd, 500, "Server Error", "{\"ok\":false,\"error\":\"messages 不可读\"}");
    return;
  }
  sz = file_size("/var/log/messages");
  if (sz < 0) sz = 0;
  tail = 65536;
  if (sz < tail) tail = sz;
  if (tail > 0) {
    fseek(f, (long)(sz - tail), SEEK_SET);
    got = (long)fread(buf, 1, sizeof(buf) - 1, f);
    if (got < 0) got = 0;
  } else got = 0;
  fclose(f);
  buf[got] = 0;
  /* 窗口内取最后 lines 行 */
  start = buf;
  n = 0;
  for (i = (int)got - 1; i >= 0; i--) {
    if (buf[i] == '\n') {
      n++;
      if (n >= (int)lines) { start = buf + i + 1; break; }
    }
  }
  json_escape(start, (size_t)((long)got - (start - buf)), esc, sizeof(esc));
  snprintf(js, sizeof(js),
    "{\"ok\":true,\"size\":%ld,\"data\":\"%s\"}", sz, esc);
  send_json(fd, js);
}

/* GET /api/crash[?file=<dump目录>/文件]：pstore 崩溃快照。
   无参=列表（pending + 历史 dump 内文件清单）；带参=读取单文件尾部 128KB
   （2026-10-17 用户要求崩溃记录上限 128KB；取尾部：oops/panic 关键信息在 dmesg 末尾）。 */
static void h_crash(int fd, const char *query) {
  char file[200] = "", js[150000], esc[145000], buf[131200];
  char dp[300];
  int i, k;
  FILE *f;
  const char *p;

  p = query ? strstr(query, "file=") : NULL;
  if (p) {
    p += 5;
    for (i = 0; p[i] && p[i] != '&' && p[i] != '"' && i < 190; i++) file[i] = p[i];
    file[190] = 0;
  }
  if (file[0]) {
    /* 段校验：只允许 [A-Za-z0-9._-] 与最多 1 个 '/'，禁 .. */
    int segs = (strchr(file, '/') != NULL) ? 2 : 1;
    char full[420];
    if (segs > 2 || strstr(file, "..")) {
      send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"文件名不合法\"}");
      return;
    }
    for (i = 0; file[i]; i++) {
      char c = file[i];
      if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') &&
          !(c >= 'A' && c <= 'Z') && c != '.' && c != '_' && c != '-' && c != '/') {
        send_err(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"文件名不合法\"}");
        return;
      }
    }
    snprintf(full, sizeof(full), "/data/pstore-dump/%s", file);
    f = fopen(full, "rb");
    if (!f) {
      send_err(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"快照不存在\"}");
      return;
    }
    /* 取尾部 128KB（文件更大时跳过头部）：崩溃 dmesg 的 oops/panic 在末尾，
       旧版从头读 32KB 必然漏掉现场。 */
    {
      long fsize = file_size(full);
      if (fsize > (long)(sizeof(buf) - 1))
        fseek(f, fsize - (long)(sizeof(buf) - 1), SEEK_SET);
    }
    k = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (k < 0) k = 0;
    buf[k] = 0;
    json_escape(buf, (size_t)k, esc, sizeof(esc));
    snprintf(js, sizeof(js),
      "{\"ok\":true,\"file\":\"%s\",\"data\":\"%s\"}", file, esc);
    send_json(fd, js);
    return;
  }
  /* 列表模式 */
  {
    DIR *d = opendir("/sys/fs/pstore");
    struct dirent *de;
    LX06_STATBUF st;
    int pending = 0;
    while (d && (de = readdir(d)) != NULL) {
      char fp[300];
      if (de->d_name[0] == '.') continue;
      snprintf(fp, sizeof(fp), "/sys/fs/pstore/%s", de->d_name);
      if (LX06_STAT(fp, &st) == 0 && st.st_size > 0) pending++;
    }
    if (d) closedir(d);
    snprintf(js, sizeof(js),
      "{\"ok\":true,\"pending\":%d,\"dumps\":[", pending);
    k = (int)strlen(js);
    d = opendir("/data/pstore-dump");
    if (d) {
      int first = 1;
      int shown = 0;
      while ((de = readdir(d)) != NULL && shown < 4) {
        struct dirent *de2;
        DIR *dd;
        LX06_STATBUF dst;
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(dp, sizeof(dp), "/data/pstore-dump/%s", de->d_name);
        if (LX06_STAT(dp, &dst) != 0 || !S_ISDIR(dst.st_mode)) continue;
        shown++;
        k += snprintf(js + k, sizeof(js) - (size_t)k,
          "%s{\"name\":\"%s\",\"files\":[", first ? "" : ",", de->d_name);
        first = 0;
        dd = opendir(dp);
        if (dd) {
          int ffirst = 1;
          while ((de2 = readdir(dd)) != NULL) {
            char fp2[600];
            LX06_STATBUF fst;
            if (de2->d_name[0] == '.') continue;
            snprintf(fp2, sizeof(fp2), "%s/%s", dp, de2->d_name);
            if (LX06_STAT(fp2, &fst) != 0 || !S_ISREG(fst.st_mode)) continue;
            if (fst.st_size > 1048576) continue; /* 只列 ≤1MB 的 */
            k += snprintf(js + k, sizeof(js) - (size_t)k,
              "%s{\"n\":\"%s\",\"s\":%ld}", ffirst ? "" : ",",
              de2->d_name, (long)fst.st_size);
            ffirst = 0;
          }
          closedir(dd);
        }
        k += snprintf(js + k, sizeof(js) - (size_t)k, "]}");
      }
      closedir(d);
    }
    k += snprintf(js + k, sizeof(js) - (size_t)k, "]}");
  }
  send_json(fd, js);
}

/* ============================ 唤醒技能（本地模板唤醒 · 直通脚本） ============================
 * 数据面:  /data/wakeword/  {wakeword.conf, models/<名>.lwt, recs/<名>/N.wav}；状态/日志在 /tmp
 * 常驻面:  /etc/init.d/lx06wakeword → watch-ww.sh（arecord|detect → /tmp/lx06-ww-event）
 * 分发面:  主循环 ww_event_poll()：
 *           有关联脚本（含 # LX06-LOCALWAKEUP: on 标记）→ run_app_profile(link,"--wakeup")
 *           未关联/标记缺失 → /bin/wakeup.sh WuW（官方小爱唤醒事件，原生路径）
 * 录音面:  /api/wakeword/record fork 子进程跑 ww-record.sh
 *          （miplayer 提示 → beep → arecord|capture 自动裁空白 → 存 recs → ≥3 自动 train）
 * 阈值:    0.05–0.35，逐词。score 是距离（越小越像）：真词≈0.095、
 * 官方词音频≈0.225、噪声基线≈0.237（10-27 z7 实测）。默认 0.18——
 * 旧默认 0.46 高于噪声基线=来者不拒，任何语音都触发。 */
#define WWBASE   "/data/wakeword"
#define WWHOME   "/usr/lib/lx06/wakeword"
#include "wake-profile.h"
#define WWCONF   WW_STORE_PATH("/wakeword.conf")
#define WWMODEL  WW_STORE_PATH("/models")
#define WWRECS   WW_STORE_PATH("/recs")
#define WWEVENT  "/tmp/lx06-ww-event"
#define WWSTATE  "/tmp/lx06-ww-record-state"
#define WW_STATE_CAP 768
#define WWLOGF   "/tmp/lx06-wake.log"
#define WW_MAXWORDS 6
#define WW_MAXRECS  10 /* existing read/play/delete compatibility */
#define WW_NEW_MAXRECS 5
#define WW_MINRECS 3
#include "../wakeword/wake_policy.h"
#define WW_GATE_CEILING WAKE_GATE_CEILING
#define WW_RECFREE_MIN_KB 256L   /* 录一条前 /data 最低余量（单条 <100KB） */
#include "../wakeword/ww_store.h"
static int ww_mutation_fd = -1;
static int ww_recording_active(void);

typedef struct {
  char name[24];    /* UTF-8，2–6 汉字（≤18 字节） */
  double th;        /* Feature-specific distance; six decimals survive edits. */
  double accepted_gate;
  char feature[64]; /* Explicit threshold calibration identity, never inferred. */
  char model_sha256[65]; /* Calibration belongs to these exact trained bytes. */
  char link[65];    /* 关联脚本名（app_name_safe），可为空 */
  int enabled;
  int p05_conflict, p05_near, p05_pending, p05_requested;
} ww_word_t;

static void ww_feature_id(char *out, size_t size) {
  char line[160], name[32], policy[32]; unsigned version, dimension;
  out[0] = 0;
  void (*previous)(int) = signal(SIGCHLD, SIG_DFL);
  FILE *f = popen(WWHOME "/wakeword_template info 2>/dev/null", "r");
  if (!f) { signal(SIGCHLD, previous); return; }
  if (fgets(line, sizeof(line), f) &&
      sscanf(line, "FEATURE %31s model_version=%u", name, &version) == 2 &&
      strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(name)) {
    snprintf(out, size, "%s:%u", name, version);
    /* Identical MFCC/model bytes can have different live search policies.
     * Never accept a median-window threshold for a duration-bank detector. */
    if (strstr(line, "calibration=")) {
      if (sscanf(line, "FEATURE %31s model_version=%u dimension=%u calibration=%31s",
                 name, &version, &dimension, policy) == 4 &&
          strspn(policy, "abcdefghijklmnopqrstuvwxyz0123456789-") == strlen(policy))
        snprintf(out, size, "%s:%u:%s", name, version, policy);
      else out[0] = 0;
    }
  }
  if (pclose(f) != 0) out[0] = 0; /* Legacy binary has no feature identity. */
  signal(SIGCHLD, previous);
}

/* 词名：2–6 个 CJK 统一表意文字（U+4E00–U+9FFF 的 UTF-8），整串不得含其他字节。 */
static int ww_cjk3(const char *p) {
  unsigned char b0 = (unsigned char)p[0], b1 = (unsigned char)p[1], b2 = (unsigned char)p[2];
  if ((b0 & 0xf0) != 0xe0 || (b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80) return 0;
  unsigned cp = ((b0 & 15) << 12) | ((b1 & 63) << 6) | (b2 & 63);
  return cp >= 0x4e00 && cp <= 0x9fff;
}
static int ww_name_ok(const char *n) {
  int chars = 0, off;
  for (off = 0; n[off] && n[off + 1] && n[off + 2]; off += 3)
    if (ww_cjk3(n + off)) chars++;
    else return 0;
  if (n[off] != 0) return 0;      /* 悬尾不完整 UTF-8 */
  return chars >= 2 && chars <= 6;
}

/* Existing 2–6 character names remain readable and manageable. */
static int ww_new_name_ok(const char *n) {
  size_t bytes = strlen(n);
  return (bytes == 9 || bytes == 12) && ww_name_ok(n);
}

/* A compatible feature alone is insufficient after adding/removing templates.
 * Manual edits or successful automatic calibration bind the exact model.
 * Callers modifying a model/config hold the shared mutation lock. */
static int ww_model_identity(const char *name, char *out, size_t size) {
  char cmd[768], line[240];
  out[0] = 0;
  if (size < 65 || !ww_name_ok(name)) return -1;
  const char *models = WWMODEL;
  snprintf(cmd, sizeof(cmd), WWHOME "/wakeword_template check \"%s/%s.lwt\" >/dev/null 2>&1 && sha256sum \"%s/%s.lwt\" 2>/dev/null", models, name, models, name);
  void (*previous)(int) = signal(SIGCHLD, SIG_DFL);
  FILE *f = popen(cmd, "r");
  if (!f) { signal(SIGCHLD, previous); return -1; }
  if (fgets(line, sizeof(line), f) && strspn(line, "0123456789abcdef") == 64 && line[64] == ' ') {
    memcpy(out, line, 64); out[64] = 0;
  }
  if (pclose(f) != 0) out[0] = 0;
  signal(SIGCHLD, previous);
  return out[0] ? 0 : -1;
}

static int ww_calibration_required(const ww_word_t *word, const char *feature) {
  char model_sha256[65];
  return !feature[0] || strcmp(feature, word->feature) || !word->model_sha256[0] ||
         ww_model_identity(word->name, model_sha256, sizeof(model_sha256)) != 0 ||
         strcmp(model_sha256, word->model_sha256);
}

/* conf 解析：每行 word=<名>|threshold=<0.05-0.35 距离阈值>|link=<脚本名>|enabled=<0/1>。
 * 字段缺失用缺省值；行坏丢弃。返回词数（≤max）。 */
static int ww_read_conf(ww_word_t *w, int max) {
  FILE *f;
  int n = 0;
  f = fopen(WWCONF, "r");
  if (!f) return 0;
  for (;;) {
    char line[512];
    if (!fgets(line, sizeof(line), f)) break;
    line[strcspn(line, "\r\n")] = 0;
    if (n >= max) break;
    {
      char *p = line;
      if (strncmp(p, "word=", 5)) continue;
      w[n].name[0] = 0;
      w[n].th = 0.18;
      w[n].accepted_gate = 0;
      w[n].feature[0] = 0;
      w[n].model_sha256[0] = 0;
      w[n].link[0] = 0;
      w[n].p05_conflict = w[n].p05_near = w[n].p05_pending = 0; w[n].p05_requested = 1;
      w[n].enabled = 1;
      p += 5;
      {
        /* 第一个字段是词本身（"word=" 前缀已在上面 p+=5 吃掉），其余字段带键。 */
        char *pipe = strchr(p, '|');
        if (pipe) *pipe = 0;
        if (p[0] && strlen(p) < 24)
          strcpy(w[n].name, p);
        if (pipe) p = pipe + 1;
        while (*p && *p != '\n') {
          pipe = strchr(p, '|');
          if (pipe) *pipe = 0;
          if (!strncmp(p, "threshold=", 10) && p[10])
            w[n].th = strtod(p + 10, NULL);
          else if (!strncmp(p, WW_ACCEPTED_GATE_FIELD, sizeof(WW_ACCEPTED_GATE_FIELD) - 1)) {
            const char *value = p + sizeof(WW_ACCEPTED_GATE_FIELD) - 1;
            char *end;
            double gate = strtod(value, &end);
            if (!*end && gate >= .000001 && gate <= WW_GATE_CEILING) w[n].accepted_gate = gate;
          }
          else if (!strncmp(p, "feature=", 8) && strlen(p + 8) < sizeof(w[n].feature) &&
                   strspn(p + 8, "abcdefghijklmnopqrstuvwxyz0123456789-:") == strlen(p + 8))
            strcpy(w[n].feature, p + 8);
          else if (!strncmp(p, "model_sha256=", 13) && strlen(p + 13) == 64 &&
                   strspn(p + 13, "0123456789abcdef") == 64)
            strcpy(w[n].model_sha256, p + 13);
          else if (!strncmp(p, "link=", 5) && strlen(p + 5) < 65)
            strcpy(w[n].link, p + 5);
          else if (!strncmp(p, "enabled=", 8) && p[8])
            w[n].enabled = (p[8] == '1');
          else if (!strncmp(p, "p05_conflict=", 13)) w[n].p05_conflict = p[13] == '1';
          else if (!strncmp(p, "p05_near=", 9)) w[n].p05_near = p[9] == '1';
          else if (!strncmp(p, "p05_pending=", 12)) w[n].p05_pending = p[12] == '1';
          else if (!strncmp(p, "p05_requested=", 14)) w[n].p05_requested = p[14] == '1';
          if (pipe) p = pipe + 1;
          else break;
        }
      }
      /* 读宽容：历史 conf 存 0.46（旧语义默认）——解析期只挡明显坏值
       * （<0.01 / >0.99），不丢词；严验证只在 API 写入（h_ww_update）。
       * 10-27 教训：解析期收紧会把整个词表读空 → update 404 + 事件分发
       * 找不到词 → 本地词全哑。 */
      if (w[n].name[0] && ww_name_ok(w[n].name) &&
          w[n].th >= 0.000001 && w[n].th <= 0.99)
        n++;
    }
  }
  fclose(f);
  return n;
}
/* Config bytes are published by ww_store: the wakeword directory is opened
 * once (O_DIRECTORY|O_NOFOLLOW), then stage, fsync, and renameat2 stay on that
 * fd. Replacing the path after the open does not redirect this write. A
 * same-UID process that already has a directory fd, or that can replace a
 * parent mount, is still outside this protocol; see
 * docs/wakeword-owned-store-recovery-design.md. */
#include "../wakeword/ww_store.c"
static int ww_write_conf(const ww_word_t *w, int n) {
  char keep[4096], out[8192];
  size_t kn = 0, used = 0;
  FILE *f = fopen(WWCONF, "r");
  if (f) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "word=", 5)) continue;
      if (kn + strlen(line) + 1 >= sizeof keep) { fclose(f); errno = EFBIG; return -1; }
      memcpy(keep + kn, line, strlen(line)); kn += strlen(line);
    }
    fclose(f);
  }
  keep[kn] = 0;
  used = (size_t)snprintf(out, sizeof out, "%s", keep);
  for (int i = 0; i < n; i++) {
    int wrote = snprintf(out + used, sizeof out - used,
      "word=%s|threshold=%.6f|link=%s|enabled=%d|feature=%s|model_sha256=%s|p05_conflict=%d|p05_near=%d|p05_pending=%d|p05_requested=%d|" WW_ACCEPTED_GATE_FIELD "%.6f\n",
      w[i].name, w[i].th, w[i].link, w[i].enabled, w[i].feature, w[i].model_sha256,
      w[i].p05_conflict, w[i].p05_near, w[i].p05_pending, w[i].p05_requested, w[i].accepted_gate);
    if (wrote < 0 || (size_t)wrote >= sizeof out - used) { errno = EFBIG; return -1; }
    used += (size_t)wrote;
  }
  return ww_store_replace_conf(WW_STORE_PATH(""), out, used);
}
static int ww_link_missing(const char *name) {
  char path[340];
  if(!app_name_safe(name))return 0;
  snprintf(path,sizeof(path),APPPERSIST "/%s",name);
  return access(path,F_OK)!=0 && errno==ENOENT;
}
/* Repair all aliases, preserving recordings, calibrated models and thresholds.
 * Use the training/mutation lock; if training owns it, list refresh retries.
 * Runtime target resolution also falls back before any session handoff. */
static void ww_repair_missing_links(void) {
  int lock=open("/tmp/lx06-ww-mutate.lock",O_RDWR|O_CREAT|O_CLOEXEC,0600);
  if(lock<0)return;
  if(flock(lock,LOCK_EX|LOCK_NB)!=0){close(lock);return;}
  ww_word_t words[WW_MAXWORDS];int n=ww_read_conf(words,WW_MAXWORDS),changed=0;
  for(int i=0;i<n;i++)if(words[i].link[0]&&ww_link_missing(words[i].link)) {
    words[i].link[0]=0;changed=1;
  }
  if(changed && ww_write_conf(words,n)==0)ww_log("Missing script links restored to official");
  close(lock);
}
static int ww_model_exists(const char *name) {
  char p[160];
  LX06_STATBUF st;
  snprintf(p, sizeof(p), "%s/%s.lwt", WWMODEL, name);
  return LX06_STAT(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 100;
}
/* Count only the numbered, regular enrollment WAVs. Legacy 6–10 are
 * retained and remain readable, but the recorder never adds a sixth. */
static int ww_reccount(const char *name) {
  char path[120];
  LX06_STATBUF st;
  int count = 0;
  for (int n = 1; n <= WW_MAXRECS; n++) {
    snprintf(path, sizeof(path), "%s/%s/%d.wav", WWRECS, name, n);
    if (LX06_STAT(path, &st) == 0 && S_ISREG(st.st_mode)) count++;
  }
  return count;
}
/* The async trainer keeps the original mutation lock until the real shell
 * child has exited. Only tagged trainer results are shown in the UI. */
static int ww_retrain(const char *name, char *detail, size_t cap) {
  char cmd[256], line[512];
  const char result[] = "LX06_WW_RESULT ";
  snprintf(detail, cap, "重训未完成，请查看唤醒日志");
  if (!ww_name_ok(name) || ww_mutation_fd < 0) return -1;
  if (dup2(ww_mutation_fd, 9) < 0 || fcntl(9, F_SETFD, 0) < 0 ||
      setenv("LX06_WW_LOCK_HELD", "1", 1) || setenv("LX06_WW_WEB_RETRAIN", "1", 1))
    return -1;
  snprintf(cmd, sizeof(cmd), "exec sh " WWHOME "/ww-train.sh \"%s\" 2>&1", name);
  FILE *child = popen(cmd, "r");
  if (!child) return -1;
  int reported = 0;
  while (fgets(line, sizeof line, child)) {
    size_t end = strcspn(line, "\r\n");
    if (line[end] && !strncmp(line, result, sizeof(result) - 1) &&
        end > sizeof(result) - 1 && end - (sizeof(result) - 1) < cap) {
      line[end] = 0;
      snprintf(detail, cap, "%s", line + sizeof(result) - 1);
      reported = 1;
    } else {
      line[end] = 0;
      if (line[0]) ww_log("RETRAIN word=%s %s", name, line);
    }
  }
  return pclose(child) == 0 && reported ? 0 : -1;
}

/* 监听守护是否存活（/proc cmdline 扫描）。 */
static int ww_service_up(void) {
  DIR *d;
  struct dirent *de;
  int up = 0;
  d = opendir("/proc");
  if (!d) return 0;
  while ((de = readdir(d)) != NULL) {
    char cmdl[64], cmdbuf[256];
    ssize_t rn;
    int f;
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    snprintf(cmdl, sizeof(cmdl), "/proc/%s/cmdline", de->d_name);
    f = open(cmdl, O_RDONLY);
    if (f < 0) continue;
    rn = read(f, cmdbuf, sizeof(cmdbuf) - 1);
    close(f);
    if (rn > 0) {
      cmdbuf[rn] = 0;
      /* cmdline 是 NUL 分隔的参数表：strstr 只看到第一个 NUL 前的 argv[0]
       * （守护进程 argv[0]=/bin/sh，参数在第二段）→ 旧代码 idle 时永远 0。 */
      for (ssize_t i = 0; i < rn; i++)
        if (cmdbuf[i] == 0) cmdbuf[i] = ' ';
      if (strstr(cmdbuf, "watch-ww.sh") || strstr(cmdbuf, "wakeword_template detect "))
        up = 1;
    }
  }
  closedir(d);
  return up;
}
static void ww_log(const char *fmt, ...) {
  char msg[256], line[300], ts[32];
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (!tm || !strftime(ts, sizeof(ts), "%F %T", tm)) return;
  snprintf(line, sizeof(line), "%s %s\n", ts, msg);
  log_append_line(WWLOGF, line);
}

/* Wake history reads the two bounded log streams and returns only confirmed
 * official matches and accepted local script dispatches. */
#define WW_HISTORY_COUNT 20
#define WW_HISTORY_DIAGNOSTICS 2
#define WW_DIAGNOSTICS_CLEAR_LINE "DIAGNOSTICS-CLEARED\n"
struct ww_history_entry {
  time_t time;
  int official;
  char word[24];
  const char *outcome, *reason;
  int cloud_confirmed;
};
static int ww_diagnostic_line(const char *body, struct ww_history_entry *entry) {
  static const struct {const char *prefix, *reason;} blocks[] = {
    {" TRIGGER-SUPPRESS-UNFENCED label=", "capture"},
    {" TRIGGER-SUPPRESS-SESSION label=", "session"},
    {" TRIGGER-SUPPRESS-OFFICIAL label=", "official"},
    {" TRIGGER-SUPPRESS-PREVIEW label=", "preview"},
    {" TRIGGER-SUPPRESS-WINDOW label=", "window"},
    {" TRIGGER-MISS label=", "target"}
  };
  entry->outcome = "detected";
  entry->reason = "";
  for (size_t i = 0; i < sizeof(blocks) / sizeof(*blocks); i++) {
    size_t len = strlen(blocks[i].prefix);
    if (strncmp(body, blocks[i].prefix, len)) continue;
    entry->outcome = "blocked";
    entry->reason = blocks[i].reason;
    return sscanf(body + len, "%23s", entry->word) == 1 && ww_name_ok(entry->word);
  }
  char action[24];
  if (sscanf(body, " wake label=%23s score=%23s", entry->word, action) == 2)
    return ww_name_ok(entry->word);
  if (sscanf(body, " CLOUD-OK label=%23s", entry->word) == 1) {
    entry->cloud_confirmed = 1;
    return ww_name_ok(entry->word);
  }
  if (sscanf(body, " TRIGGER-QUEUED label=%23s action=%23s", entry->word, action) != 2 &&
      sscanf(body, " TRIGGER label=%23s action=%23s", entry->word, action) != 2) return 0;
  if (!strcmp(action, "RUN") || !strcmp(action, "RESIDENT") || !strcmp(action, "OFFICIAL-NATIVE"))
    entry->outcome = "queued";
  else {
    entry->outcome = "blocked";
    entry->reason = "target";
  }
  return ww_name_ok(entry->word);
}
static int ww_history_parse(const char *line, int official, time_t now,
                            struct ww_history_entry *entry) {
  struct tm stamp = {0};
  const char *body = strptime(line, official == 1 ? "%b %e %T" : "%F %T", &stamp);
  if (!body || *body != ' ') return 0;
  if (official == WW_HISTORY_DIAGNOSTICS) {
    if (!ww_diagnostic_line(body, entry)) return 0;
  } else if (official) {
    struct tm current;
    if (!localtime_r(&now, &current)) return 0;
    stamp.tm_year = current.tm_year;
    if (current.tm_mon == 0 && stamp.tm_mon == 11) stamp.tm_year--;
    const char *prefix = ": lx06-native: official event=1 ";
    body = strstr(body, ": ");
    if (!body || strncmp(body, prefix, strlen(prefix))) return 0;
    body += strlen(prefix);
    if (strcmp(body, "唤醒词：小爱同学")) {
      double angle;
      char extra;
      if (sscanf(body, "angle=%lf%c", &angle, &extra) != 1 || !isfinite(angle)) return 0;
    }
    strcpy(entry->word, "小爱同学");
  } else {
    char action[20];
    if (sscanf(body, " TRIGGER label=%23s action=%19s", entry->word, action) != 2 ||
        !ww_name_ok(entry->word) || (strcmp(action, "RUN") && strcmp(action, "RESIDENT")))
      return 0;
  }
  stamp.tm_isdst = -1;
  entry->time = mktime(&stamp);
  entry->official = official;
  return entry->time != (time_t)-1 && difftime(entry->time, now) <= 300;
}
static void ww_history_log(const char *path, int official, time_t now,
                           struct ww_history_entry *events, size_t *count) {
  char buf[65536];
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) return;
  LX06_STATBUF st;
  if (LX06_FSTAT(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return; }
  off_t offset = st.st_size > (off_t)sizeof(buf) ? st.st_size - sizeof(buf) : 0;
  if (lseek(fd, offset, SEEK_SET) < 0) { close(fd); return; }
  size_t n = 0;
  while (n < sizeof(buf)) {
    ssize_t got = read(fd, buf + n, sizeof(buf) - n);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    n += (size_t)got;
  }
  close(fd);
  char *line = buf, *nl;
  if (offset) {
    nl = memchr(buf, '\n', n);
    if (!nl) return;
    line = nl + 1;
  }
  while ((nl = memchr(line, '\n', (size_t)(buf + n - line)))) {
    if (official == WW_HISTORY_DIAGNOSTICS &&
        (size_t)(nl + 1 - line) == sizeof(WW_DIAGNOSTICS_CLEAR_LINE) - 1 &&
        !memcmp(line, WW_DIAGNOSTICS_CLEAR_LINE, sizeof(WW_DIAGNOSTICS_CLEAR_LINE) - 1)) {
      *count = 0;
      line = nl + 1;
      continue;
    }
    *nl = 0;
    struct ww_history_entry entry = {0};
    if (!memchr(line, 0, (size_t)(nl - line)) && ww_history_parse(line, official, now, &entry)) {
      size_t i = 0;
      if (official == WW_HISTORY_DIAGNOSTICS) {
        while (i < *count && (events[i].time != entry.time || strcmp(events[i].word, entry.word))) i++;
        if (i < *count) {
          events[i].cloud_confirmed |= entry.cloud_confirmed;
          if (strcmp(entry.outcome, "detected")) {
            events[i].outcome = entry.outcome;
            events[i].reason = entry.reason;
          }
          line = nl + 1;
          continue;
        }
        i = 0;
      }
      while (i < *count && events[i].time > entry.time) i++;
      if (i < WW_HISTORY_COUNT) {
        if (*count < WW_HISTORY_COUNT) (*count)++;
        memmove(events + i + 1, events + i, (*count - i - 1) * sizeof(*events));
        events[i] = entry;
      }
    }
    line = nl + 1;
  }
}
static void ww_send_wake_log(int fd, int diagnostics) {
  struct ww_history_entry events[WW_HISTORY_COUNT];
  time_t now = time(NULL);
  size_t count = 0;
  if (!diagnostics) {
    ww_history_log("/var/log/messages.1", 1, now, events, &count);
    ww_history_log("/var/log/messages", 1, now, events, &count);
  }
  int mode = diagnostics ? WW_HISTORY_DIAGNOSTICS : 0;
  ww_history_log(WWLOGF ".1", mode, now, events, &count);
  ww_history_log(WWLOGF, mode, now, events, &count);
  char out[4096] = "{\"ok\":true,\"events\":[";
  size_t at = strlen(out);
  const size_t start = at;
  for (size_t i = 0; i < count; i++) {
    struct tm stamp;
    char time_text[20], word[64];
    if (!localtime_r(&events[i].time, &stamp) ||
        !strftime(time_text, sizeof(time_text), "%F %T", &stamp)) continue;
    json_escape(events[i].word, strlen(events[i].word), word, sizeof(word));
    int n;
    if (diagnostics) {
      n = snprintf(out + at, sizeof(out) - at,
                   "%s{\"time\":\"%s\",\"word\":\"%s\",\"outcome\":\"%s\",\"reason\":\"%s\",\"cloud_confirmed\":%s}",
                   at == start ? "" : ",", time_text, word, events[i].outcome, events[i].reason,
                   events[i].cloud_confirmed ? "true" : "false");
    } else n = snprintf(out + at, sizeof(out) - at,
                     "%s{\"time\":\"%s\",\"word\":\"%s\",\"source\":\"%s\"}",
                     at == start ? "" : ",",
                     time_text, word, events[i].official ? "official" : "local");
    if (n < 0 || (size_t)n >= sizeof(out) - at - 2) break;
    at += (size_t)n;
  }
  strcpy(out + at, "]}");
  send_json(fd, out);
}
static void h_ww_history(int fd) { ww_send_wake_log(fd, 0); }

/* Retain terminal diagnostics until the next operation. The mutation lock,
 * rather than the state file's age, decides whether training is still live. */
static int ww_state_format(char *out, size_t cap, const char *status,
                           const char *word, int test, const char *detail) {
  char escaped[WW_STATE_CAP];
  if (!detail) detail = "";
  json_escape(detail, strlen(detail), escaped, sizeof escaped);
  int n = snprintf(out, cap,
                   "{\"status\":\"%s\",\"word\":\"%s\",\"test\":%d,\"detail\":\"%s\"}",
                   status, word, test, escaped);
  return n >= 0 && (size_t)n < cap ? 0 : -1;
}
static int ww_state_read(char *out, size_t outsz, int *busy) {
  int fd = open(WWSTATE, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  LX06_STATBUF st;
  if (busy) *busy = 0;
  out[0] = 0;
  if (fd < 0) return -1;
  if (LX06_FSTAT(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  FILE *f = fdopen(fd, "r");
  if (!f) { close(fd); return -1; }
  size_t n = fread(out, 1, outsz - 1, f);
  int complete = fgetc(f) == EOF && !ferror(f);
  fclose(f);
  char status[16], word[24], test[4], detail[WW_STATE_CAP];
  int found = 0;
  out[n] = 0;
  if (!complete || !n || memchr(out, 0, n) ||
      app_json_str(out, "status", status, sizeof status) ||
      app_json_str(out, "word", word, sizeof word) || (word[0] && !ww_name_ok(word)) ||
      app_json_str(out, "test", test, sizeof test) || (strcmp(test, "0") && strcmp(test, "1")) ||
      app_json_strx(out, "detail", detail, sizeof detail, &found) || !found) {
    out[0] = 0;
    errno = EINVAL;
    return -1;
  }
  if (!strcmp(status, "recording") || !strcmp(status, "training")) {
    if (busy) *busy = 1;
  } else if (strcmp(status, "done") && strcmp(status, "error")) {
    out[0] = 0;
    errno = EINVAL;
    return -1;
  }
  /* Return only the verified state fields, in the same form as our writer. */
  if (ww_state_format(out, outsz, status, word, test[0] - '0', detail) != 0) {
    out[0] = 0;
    errno = EINVAL;
    return -1;
  }
  return 0;
}
static void ww_state_write(const char *status, const char *word, int test, const char *detail) {
  char tmp[] = WWSTATE ".XXXXXX", buf[WW_STATE_CAP];
  if (ww_state_format(buf, sizeof buf, status, word, test, detail) != 0) return;
  int fd = mkstemp(tmp);
  if (fd < 0) return;
  FILE *f = fdopen(fd, "w");
  if (!f) { close(fd); unlink(tmp); return; }
  int failed = fputs(buf, f) == EOF;
  if (fclose(f) != 0 || failed || rename(tmp, WWSTATE) != 0) unlink(tmp);
}
/* 配置变更后重启监听守护：detect 管道参数启动时定死，restart=整条重建。
 * fork+system，不阻塞 HTTP 应答（stop+start ~1s 异步收敛）。
 * V44.2（z2 修复）: 状态保持式——仅当守护本来在跑才 restart；停着就保持停。
 * 旧逻辑无条件 restart 会把停用的栈拉起 → xiaoai-audio --keep 持有 voip
 * 会话 → 官方「小爱同学」唤醒后无法对话（2026-10-26 双盲实证：有栈
 * Wakeup=0/追问无答，无栈正常；静置 4 分钟不自愈）。手动启停走
 * /api/wakeword/service，不受此影响。 */
static void ww_service_restart(void) {
  if (fork() == 0) {
    /* Listener restart does not own the configuration mutation. Keeping its
     * inherited lease rejects the next user action after an acknowledged add. */
    if (ww_mutation_fd >= 0) close(ww_mutation_fd);
    int fd2;
    fd2 = open("/dev/null", O_RDONLY | O_WRONLY);
    if (fd2 >= 0) { dup2(fd2, 0); dup2(fd2, 1); dup2(fd2, 2); close(fd2); }
    system("pidof wakeword_template >/dev/null 2>&1 && "
           "/etc/init.d/lx06wakeword restart >/dev/null 2>&1");
    _exit(0);
  }
}
/* 含 localwakeup 标记的已部署脚本（链接下拉候选）。返回 strdup 名数组个数。 */
static int ww_linked_apps(char ***out) {
  DIR *d;
  struct dirent *de;
  char *list[WW_MAXWORDS * 4];
  int n = 0;
  d = opendir(APPPERSIST);
  if (!d) { *out = NULL; return 0; }
  while ((de = readdir(d)) != NULL && n < 24) {
    if (de->d_name[0] == '.') continue;
    if (!app_name_safe(de->d_name)) continue;
    if (app_file_marker_flags(de->d_name) & APP_LOCAL_WAKEWORD)
      list[n++] = strdup(de->d_name);
  }
  closedir(d);
  *out = n ? (char **)realloc(NULL, (size_t)n * sizeof(char *)) : NULL;
  for (int i = 0; i < n; i++) (*out)[i] = list[i];
  return n;
}

/* 双路由：写 tap 的临时路由覆盖（120s TTL 按 mtime 过期，回落默认文件）。
 * "official"=官方唤醒 / 链到官方的本地词；"mic"=非官方，送 xiaoai-mic。
 * 官方 speech 帧不能原样送 common.usock。 */
static void ww_set_route(const char *r) {
  int f = open("/tmp/xiaoai-agent/route", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (f >= 0) { (void)!write(f, r, strlen(r)); close(f); }
}
/* 「官方小爱同学」开关只关四个字。门机制三代史：
 * ① V28.2 wakeup.sh 钩子——V44 官方级联不调它，失活。
 * ② /data/mipns/mitv + SIGHUP——02:40/03:37 两轮通过，但 03:43 马拉松
 *    会话后字节失效（mitv=off+HUP 落地、官方 KWS 照常 wakeup_real，
 *    05:29 event.log 实证），跨断电/重启不复原，弃用。
 * ③ xiaoai-ctl kws（V30 原生 DOWN 9/8 直注引擎）——05:33 实测：off 后
 *    官方词零唤醒零应答零叮咚，on 秒恢复（05:35 应答）；无 HUP 无重载
 *    竞态无 FSM 毒化。engine KWS 开关与 fifo set_wakeup_status 分路，
 *    本地词 dispatch 不受影响（开窗逻辑仍保留以防万一+语义对齐）。 */
static int ww_official_read(void) {
  char c[8] = {0};
  int f = open(WWBASE "/official_xiaoai", O_RDONLY);
  /* 缺文件默认开。只有明确写成 0 才关。 */
  if (f < 0) return 1;
  if (read(f, c, sizeof(c) - 1) > 0) c[7] = 0;
  close(f);
  return c[0] != '0';
}
/* pns KWS 门：xiaoai-ctl kws（DOWN socket 直注，即时生效，幂等）。 */
static void ww_kws_gate(int on) {
  if (fork() == 0) {
    int fd2 = open("/dev/null", O_WRONLY);
    if (fd2 >= 0) { dup2(fd2, 1); dup2(fd2, 2); close(fd2); }
    execl("/bin/sh", "sh", "-c",
          on ? "/usr/lib/lx06/xiaoai/xiaoai-ctl kws on >/dev/null 2>&1 "
               "|| /data/lx06/xiaoai/xiaoai-ctl kws on >/dev/null 2>&1"
             : "/usr/lib/lx06/xiaoai/xiaoai-ctl kws off >/dev/null 2>&1 "
               "|| /data/lx06/xiaoai/xiaoai-ctl kws off >/dev/null 2>&1",
          (char *)0);
    _exit(1);
  }
}
/* 官方词关时本地词触发 → 临时开窗：gate(1) 后 dispatch，对话结束（voip
 * 回 idle）且开关仍为关 → 回关。6s 起步 + 2s 轮询，曝光=对话时长+≤8s。 */
static void ww_kws_window_rearm(void) {
  if (fork() == 0) {
    int fd2 = open("/dev/null", O_WRONLY);
    if (fd2 >= 0) { dup2(fd2, 1); dup2(fd2, 2); close(fd2); }
    execl("/bin/sh", "sh", "-c",
          "sleep 6; "
          "i=0; while [ $i -lt 60 ]; do "
          "  [ \"$(voip_helper -e query 2>/dev/null)\" = \"idle\" ] && break; "
          "  sleep 2; i=$((i+1)); done; "
          "if [ \"$(cat " WWBASE "/official_xiaoai 2>/dev/null)\" = \"0\" ]; then "
          "  /usr/lib/lx06/xiaoai/xiaoai-ctl kws off >/dev/null 2>&1 "
          "  || /data/lx06/xiaoai/xiaoai-ctl kws off >/dev/null 2>&1; "
          "fi",
          (char *)0);
    _exit(1);
  }
}
static int ww_official_apply(int on, int persist) {
  int f;
  if (persist) {
    if (mkdir(WWBASE, 0755) != 0 && errno != EEXIST) return -1;
    char tmp[] = WWBASE "/official_xiaoai.tmp.XXXXXX";
    /* mkstemp atomically creates an exclusive, unique file and retries collisions.
     * Never remove stale names: they may belong to another process. */
    f = mkstemp(tmp);
    if (f < 0) return -1;
    int ok = fchmod(f, 0644) == 0 && write(f, on ? "1" : "0", 1) == 1 && fsync(f) == 0;
    if (close(f) != 0) ok = 0;
    if (!ok || rename(tmp, WWBASE "/official_xiaoai") != 0) {
      unlink(tmp); return -1;
    }
  }
  f = open("/tmp/xiaoai-agent/route.default", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (f >= 0) { (void)!write(f, "official", 8); close(f); }
  /* Native callback gating reads official_xiaoai. The stock processing engine
   * must stay enabled so local words can still open an official ASR session. */
  ww_kws_gate(1);
  ww_log("OFFICIAL-TOGGLE on=%d route.default=official native_keyword_gate=1", on);
  return 0;
}

/* V41.2: 官方 KWS 实时仲裁。
 * 官方「小爱同学」KWS 级联不走 wakeup.sh WuW（10-26 08:45 实测：叮咚经
 * pnshelper tone 事件、wakeup.sh 只在 "ready" 时被调，/tmp/lx06-official-wake
 * 标记不写）→ 标记判不准。这里直接读 /var/log/messages 尾部：pns 的
 * "mipns_speech.event=wakeup-2!"（二级检测通过，pns 即将/已经 enable
 * asr=1）在最近 win_s 秒内出现 → pns 已在处理官方对话，丢弃本 DTW 触发
 * （放行会自播 xw.mp3 + voip 事件灌管，比官方基线慢 3~9s）。
 * 用 wakeup-2 而非 wakeup-1：1-level 可能 "unnormal wakeup, do nothing"
 * (08:33:14 实测 score 11.0)，那时 asr 未武装，不能吞本地词。 */
static int official_wake_recent(int win_s) {
  static char buf[66000];
  int fd = open("/var/log/messages", O_RDONLY);
  if (fd < 0) return 0;
  LX06_STATBUF st;
  if (LX06_FSTAT(fd, &st) != 0 || st.st_size < 32) { close(fd); return 0; }
  long tail = 65536;
  if ((long)st.st_size < tail) tail = (long)st.st_size;
  ssize_t n = pread(fd, buf, (size_t)tail, (off_t)st.st_size - tail);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = 0;
  time_t now = time(NULL);
  time_t tref = now;
  struct tm *tr = localtime(&tref);
  if (!tr) return 0;
  static const char *mnames[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
  char *p = buf;
  while ((p = strstr(p, "wakeup-2!")) != 0) {
    char *ls = p;
    while (ls > buf && *(ls - 1) != '\n') ls--;
    int mon = -1;
    for (int i = 0; i < 12; i++)
      if (ls[0] && ls[3] == ' ' && !strncmp(ls, mnames[i], 3)) { mon = i + 1; break; }
    p += 9;
    if (mon < 0) continue;
    int day = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(ls + 4, "%d %d:%d:%d", &day, &hh, &mm, &ss) != 4) continue;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = tr->tm_year;
    tmv.tm_mon = mon - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hh;
    tmv.tm_min = mm;
    tmv.tm_sec = ss;
    time_t ts = mktime(&tmv);
    if (ts > 0 && now - ts >= -5 && now - ts <= win_s) return 1;
  }
  return 0;
}

/* Local detector events are dispatched only while no session is active.
 * Cloud ASR never writes here. */
static char ww_event_seen[128];
static struct {
  int active;
  int64_t deadline;
  char event[128], label[24], link[65];
  struct lx06_session_record owner;
} ww_handoff;

static int ww_official_priority(void) {
  if (!ww_official_read()) return 0;
  const char *markers[]={"/tmp/lx06-native-wake","/tmp/lx06-official-wake"};
  for (unsigned i=0;i<2;i++) {
    LX06_STATBUF st;
    if (LX06_STAT(markers[i],&st)) continue;
    long age=(long)time(NULL)-(long)st.st_mtime;
    if (age<0) continue;
    if (i==1) { if(age<=8) return 1; else continue; }
    char kind[32]={0}; int f=open(markers[i],O_RDONLY);
    if(f>=0) { (void)read(f,kind,sizeof(kind)-1);close(f); }
    if(!strncmp(kind,"official ",9) && age<=8) return 1;
    /* A local alias runs the stock wake callbacks too, generating legacy WuW
     * markers and wakeup-2 logs. Their shared official destination must not
     * block another valid alias (or an alias for the same script). A genuine
     * hardware keyword replaces this origin marker with "official" before its
     * callbacks, so real official takeover retains priority. Origin does not
     * expire after eight seconds: stock legacy markers can be refreshed later
     * in the SAME local-origin conversation. Aging out the origin alone made
     * the next correctly detected local word silently disappear. */
    if(!strncmp(kind,"local ",6)) return 0;
  }
  return official_wake_recent(8);
}
static int ww_target(const char *label,ww_word_t *word) {
  ww_word_t words[WW_MAXWORDS];
  int n=ww_read_conf(words,WW_MAXWORDS);
  for(int i=0;i<n;i++) if(!strcmp(words[i].name,label)) {
    if(!words[i].enabled || words[i].p05_pending || words[i].th > WW_GATE_CEILING || !ww_model_exists(label)) return 0;
    /* The running detector already checks the full calibrated model. Check
     * the binding again before stopping an active conversation. */
    char feature[64];ww_feature_id(feature,sizeof(feature));
    if(ww_calibration_required(&words[i],feature)) return 0;
    if(words[i].link[0]&&ww_link_missing(words[i].link))words[i].link[0]=0;
    if(words[i].link[0] && (!lx06_scripts_allowed() || !(app_file_marker_flags(words[i].link)&APP_LOCAL_WAKEWORD))) return 0;
    *word=words[i];return 1;
  }
  return 0;
}
/* WT1-CHANNEL-BEGIN. Kept inside the dispatcher slice so the historical
 * --wakeup call stays on the empty-channel path. */
#ifndef APPPERSIST
#define APPPERSIST "/data/lx06/apps"
#endif
#ifndef LX06_LISTEN_HELPER
#define LX06_LISTEN_HELPER "/usr/lib/lx06/xiaoai/lx06-listen-window"
#endif
#ifndef LX06_LISTEN_RESULT
#define LX06_LISTEN_RESULT "/tmp/lx06-listen-result"
#endif
static pid_t lx06_listen_pid;
static const char *lx06_dispatch_source = "local";
static int lx06_listen_alive(void) {
  if (lx06_listen_pid <= 0) return 0;
  if (waitpid(lx06_listen_pid, NULL, WNOHANG) == 0) return 1;
  lx06_listen_pid = 0;
  return 0;
}
static int lx06_launch_script(const char *name, const char *wake_text, const char *source) {
  unsigned flags, run;
  char args[256];
  long pid;
  if (!name || !wake_text || !lx06_scripts_allowed()) return -1;
  if (strlen(wake_text) > 120) return -1;
  int phone=app_phone_sender(name);
  const char *phone_source=NULL;
  if(phone<0)return -1;
  if(phone) {
    if(!source||!wake_text[0]||strlen(wake_text)>96)return -1;
    if(!strcmp(source,"official"))phone_source="official_asr";
    else if(!strcmp(source,"local"))phone_source="local_wake";
    else return -1;
    for(const unsigned char *p=(const unsigned char *)wake_text;*p;p++)if(*p<32||*p==127)return -1;
  }
  flags = app_file_marker_flags(name);
  if (app_running(name)) {
    char mailbox[160], temporary[180], line[192];
    int out;
    snprintf(mailbox, sizeof(mailbox), "/tmp/lx06-wake-%s.event", name);
    snprintf(temporary, sizeof(temporary), "%s.tmp", mailbox);
    if(phone)snprintf(line,sizeof line,"%s\t0\t%.2f\t%s\n",wake_text,lk_now_ms()/1000.0,phone_source);
    else snprintf(line, sizeof(line), "%s 0 %.2f\n", wake_text, lk_now_ms() / 1000.0);
    out = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) return -1;
    if (write(out, line, strlen(line)) != (ssize_t)strlen(line)) { close(out); unlink(temporary); return -1; }
    close(out);
    if (rename(temporary, mailbox) != 0) { unlink(temporary); return -1; }
    ww_log("TRIGGER action=RESIDENT link=%s source=%s", name, lx06_dispatch_source);
    return 0;
  }
  run = (flags & APP_RUN_ROOT) ? LK_ROOT : 0;
  if(phone) {
    /* keeper_args consumes quotes and backslashes without shell evaluation. */
    size_t at=(size_t)snprintf(args,sizeof args,"--phone-wakeup %s \"",phone_source);
    for(const char *p=wake_text;*p;p++) {
      if(at+3>=sizeof args)return -1;
      if(*p=='"'||*p=='\\')args[at++]='\\';
      args[at++]=*p;
    }
    args[at++]='"';args[at]=0;
  } else snprintf(args, sizeof(args), "--wakeup %s", wake_text);
  pid = run_app_profile(name, args, run);
  if (pid <= 0) return -1;
  ww_log("TRIGGER action=RUN link=%s source=%s pid=%ld", name, lx06_dispatch_source, pid);
  return 0;
}
static int lx06_listen_spawn(const char *script, const char *source) {
  pid_t pid;
  if (lx06_listen_alive()) return -1;
  if (access(LX06_LISTEN_HELPER, X_OK) != 0) return -1;
  pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    execl(LX06_LISTEN_HELPER, "lx06-listen-window", script, source, (char *)NULL);
    _exit(127);
  }
  lx06_listen_pid = pid;
  lx06_dispatch_source = source;
  ww_log("TRIGGER action=LISTEN link=%s source=%s pid=%ld", script, source, (long)pid);
  return 0;
}
static void lx06_listen_reap(void) {
  char buf[256], status[32], script[65], source[16], text[128];
  int fd, n;
  status[0] = script[0] = source[0] = text[0] = 0;
  lx06_listen_alive();
  fd = open(LX06_LISTEN_RESULT, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  n = (int)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  unlink(LX06_LISTEN_RESULT);
  if (n <= 0) return;
  buf[n] = 0;
  int text_at=0;
  int fields=sscanf(buf, "status=%31s script=%64s source=%15s text=%n%127s", status, script, source, &text_at, text);
  if(fields==4 && app_phone_sender(script)==1) {
    /* The original helper emits the entire selected phrase after text=.
     * Phone requests preserve it verbatim, including spaces and quotes. */
    size_t size=(size_t)n-(size_t)text_at;
    if(size&&buf[text_at+size-1]=='\n')size--;
    if(size>96)text[0]=0;
    else {memcpy(text,buf+text_at,size);text[size]=0;}
  }
  if (!strcmp(status, "hit") && script[0] && text[0] && strcmp(text, "-")) {
    lx06_dispatch_source = source[0] ? source : "local";
    if (lx06_launch_script(script, text, source) != 0)
      ww_log("TRIGGER action=LISTEN-LAUNCH-FAIL link=%s source=%s", script, lx06_dispatch_source);
    lx06_dispatch_source = "local";
    return;
  }
  /* Prompt kind only. The heard sentence is not logged. */
  if (!strcmp(status, "timeout")) ww_log("LISTEN %s", "timeout");
  else if (!strcmp(status, "asr")) ww_log("LISTEN %s", "asr");
  else if (!strcmp(status, "miss")) ww_log("LISTEN %s", "miss");
  else if (!strcmp(status, "reject")) ww_log("LISTEN %s", "reject");
  else if (!strcmp(status, "cancel")) ww_log("LISTEN %s", "cancel");
}
static int lx06_stop_official_tts(void) {
  /* Match the official command router: stop only the stock TTS media group. */
  void (*previous)(int) = signal(SIGCHLD, SIG_DFL);
  if (previous == SIG_ERR) return -1;
  int status = system("ubus -t 1 call mediaplayer player_play_operation "
                      "'{\"media\":\"tts\",\"action\":\"stop\"}' >/dev/null 2>&1");
  signal(SIGCHLD, previous);
  return status == 0 ? 0 : -1;
}

static void lx06_bind_poll(void) {
  char event_id[66], id[65];
  const char *claimed = LX06_OFFICIAL_BIND_EVENT ".webd";
  LX06_STATBUF st;
  /* Claim the event before reading it. A confirmed wake published while we
   * handle this one remains at the original path for the next poll. */
  if (rename(LX06_OFFICIAL_BIND_EVENT, claimed) != 0) return;
  int fd = open(claimed, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  unlink(claimed);
  if (fd < 0) return;
  if (LX06_FSTAT(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
    close(fd);
    return;
  }
  ssize_t n = read(fd, event_id, sizeof(event_id));
  close(fd);
  if (n < 2 || n != st.st_size || n >= (ssize_t)sizeof(event_id) ||
      event_id[n - 1] != '\n' || memchr(event_id, 0, (size_t)n)) return;
  event_id[n - 1] = 0;
  if (!lx06_bind_name_ok(event_id)) return;
  int state = lx06_official_bind_decide_at(LX06_OFFICIAL_BIND_PATH, APPPERSIST, id, sizeof(id));
  if (state != LX06_BIND_ARMED || strcmp(event_id, id)) return;
  int channel = lx06_local_channel_armed_at(APPPERSIST, id);
  int route = lx06_route_official(lx06_official_kws_enabled(), state, channel);
  if (route == LX06_OFFICIAL_PASSTHROUGH) {
    if (lx06_stop_official_tts() != 0) {
      ww_log("BIND action=STOP-TTS-FAIL link=%s", id);
      return;
    }
    lx06_dispatch_source = "official";
    if (lx06_launch_script(id, "小爱同学", "official") != 0)
      ww_log("BIND action=RUN-FAIL link=%s", id);
    lx06_dispatch_source = "local";
  } else if (route == LX06_OFFICIAL_LISTEN) {
    if (lx06_listen_spawn(id, "official") != 0)
      ww_log("BIND action=LISTEN-FAIL link=%s", id);
  }
}
static void ww_dispatch(const ww_word_t *word,const char *event) {
    if(word->link[0]&&!lx06_scripts_allowed()) {ww_log("TRIGGER-DISABLED label=%s",word->name);return;}
    {
      int ran = 0;
      if (word->link[0] && (app_file_marker_flags(word->link) & APP_LOCAL_WAKEWORD)) {
        if (lx06_local_channel_armed_at(APPPERSIST, word->link)) {
          int route = lx06_route_local(lx06_listen_alive(), 1);
          if (route != LX06_LOCAL_LISTEN) {
            ww_log("TRIGGER-SUPPRESS-WINDOW label=%s", word->name);
            return;
          }
          if (lx06_listen_spawn(word->link, "local") != 0)
            ww_log("TRIGGER label=%s action=LISTEN-FAIL link=%s", word->name, word->link);
          return;
        }
        int phone=app_phone_sender(word->link);
        if(phone) {
          if(phone<0||lx06_launch_script(word->link,word->name,"local")!=0)
            ww_log("TRIGGER label=%s action=PHONE-LAUNCH-FAIL link=%s",word->name,word->link);
          return;
        }
        if (app_running(word->link)) {
          /* Only webd consumes the shared detector event. A resident receives
           * its own mailbox, after the old session lease has been released. */
          char mailbox[160], temporary[180];
          snprintf(mailbox,sizeof(mailbox),"/tmp/lx06-wake-%s.event",word->link);
          snprintf(temporary,sizeof(temporary),"%s.tmp",mailbox);
          int out=open(temporary,O_WRONLY|O_CREAT|O_TRUNC,0600);
          if(out>=0) {
            (void)write(out,event,strlen(event)); close(out);
            if(rename(temporary,mailbox)==0)
              ww_log("TRIGGER label=%s action=RESIDENT link=%s",word->name,word->link);
          }
          return;
        }
        unsigned flags = (app_file_marker_flags(word->link) & APP_RUN_ROOT) ? LK_ROOT : 0;
        char arguments[80]; snprintf(arguments,sizeof(arguments),"--wakeup %s",word->name);
        long pid = run_app_profile(word->link, arguments, flags);
        if (pid > 0) {
          ww_log("TRIGGER label=%s action=RUN link=%s pid=%ld", word->name, word->link, pid);
          ran = 1;
        } else {
          ww_log("TRIGGER label=%s action=RUN-FAIL link=%s err=%s",
                 word->name, word->link, strerror(errno));
        }
      } else if (word->link[0]) {
        ww_log("TRIGGER label=%s action=FALLBACK link=%s (no marker/uninstalled)",
               word->name, word->link);
      }
      if (!ran && !word->link[0]) {
        /* The native bridge supplies WUW data before opening the ASR stream.
         * src=1,event=1 is the microphone switch and must never be used here. */
        int request = open("/tmp/lx06-native-request.web", O_WRONLY|O_CREAT|O_TRUNC, 0600);
        if (request >= 0) {
          ssize_t written = write(request, "wake\n", 5);
          close(request);
          if (written == 5 && rename("/tmp/lx06-native-request.web", "/tmp/lx06-native-request") == 0)
            ww_log("TRIGGER label=%s action=OFFICIAL-NATIVE", word->name);
          else ww_log("TRIGGER label=%s action=OFFICIAL-REQUEST-FAIL", word->name);
        } else ww_log("TRIGGER label=%s action=OFFICIAL-REQUEST-FAIL", word->name);
      }
    }
}
static void ww_pending_poll(void) {
  if(!ww_handoff.active) return;
  if(ww_recording_active() || ww_official_priority() || lk_now_ms()>ww_handoff.deadline) {
    ww_log("SWITCH-CANCEL label=%s (official/recording/timeout)",ww_handoff.label);
    ww_handoff.active=0;return;
  }
  struct lx06_session_record owner;
  if(lx06_session_read(&owner)) {
    if(memcmp(&owner,&ww_handoff.owner,sizeof(owner))) {
      ww_log("SWITCH-CANCEL label=%s (owner changed)",ww_handoff.label);
      ww_handoff.active=0;
    }
    return;
  }
  ww_handoff.active=0;
  ww_word_t word;
  if(!ww_target(ww_handoff.label,&word) || strcmp(word.link,ww_handoff.link)) {
    ww_log("SWITCH-CANCEL label=%s (target changed)",ww_handoff.label);return;
  }
  if(ww_official_priority()) return;
  ww_log("SWITCH-READY label=%s link=%s",word.name,word.link);
  double score=0;
  (void)sscanf(ww_handoff.event,"%*s %lf",&score);
  snprintf(ww_handoff.event,sizeof(ww_handoff.event),"%s %.6f %.2f\n",word.name,score,lk_now_ms()/1000.0);
  ww_dispatch(&word,ww_handoff.event);
}
static int ww_preview_active(void) {
  const char *markers[] = {"/tmp/lx06-selfplay", "/tmp/lx06-ww-preview"};
  for (unsigned i = 0; i < 2; i++) {
    LX06_STATBUF st;
    if (!LX06_STAT(markers[i], &st)) {
      long elapsed = (long)time(NULL) - (long)st.st_mtime;
      if (elapsed >= 0 && elapsed <= (i ? 3 : 15)) return 1;
    }
  }
  return 0;
}
static void ww_event_poll(void) {
  /* No detector event currently carries provable session provenance.  Do not
   * revive an earlier handoff either; all local routes must fail closed. */
  lx06_bind_poll();
  lx06_listen_reap();
  ww_handoff.active=0;
  ww_pending_poll();
  char buf[128];int f=open(WWEVENT,O_RDONLY);
  if(f<0) return;
  ssize_t n=read(f,buf,sizeof(buf)-1);close(f);
  if(n<=0) return;
  buf[n]=0;
  if(!strcmp(buf,ww_event_seen)) return;
  snprintf(ww_event_seen,sizeof(ww_event_seen),"%s",buf);
  unlink(WWEVENT);
  char label[24],tail;double score,stamp;
  if(sscanf(buf,"%23s %lf %lf %c",label,&score,&stamp,&tail)!=3 || !ww_name_ok(label) ||
     !isfinite(score) || score<0 || !isfinite(stamp) || stamp<0) return;
  double age=lk_now_ms()/1000.0-stamp;
  if(age<-.5 || age>3 || ww_recording_active() || ww_handoff.active) return;
  if(ww_official_priority()) {
    ww_log("TRIGGER-SUPPRESS-OFFICIAL label=%s",label);return;
  }
  if (ww_preview_active()) {
    ww_log("TRIGGER-SUPPRESS-PREVIEW label=%s",label);return;
  }
  ww_word_t word;
  if(!ww_target(label,&word) || score>word.th) {
    ww_log("TRIGGER-MISS label=%s (unavailable/unmatched target)",label);return;
  }
  if (lx06_listen_alive()) {
    ww_log("TRIGGER-SUPPRESS-WINDOW label=%s", label);
    return;
  }
  struct lx06_session_record owner;
  if(lx06_session_read(&owner)) {
    /* Local matches, including official-linked aliases, never preempt a
     * third-party session. Unknown identity is deliberately also busy. */
    ww_log("TRIGGER-SUPPRESS-SESSION label=%s",label);
    return;
  }
  /* A fresh-looking timestamp is not proof that the acoustic match occurred
   * outside a session: detector/FIFO/watcher processing can be delayed. */
  ww_log("TRIGGER-SUPPRESS-UNFENCED label=%s",label);
  return;
}

/* ---- web88 端点 ---- */
/* Defined with the P0.5 block, which is included below this handler. */
int wake_settings_read(const char *dir, const char *name, char *out, size_t cap);
int wake_samples_json(const char *recdir, char *out, size_t cap);
static int ww_cloud_on(void) {
  char value[16];
  const char *env = getenv("LX06_WAKE_CLOUD_VERIFY");
  if (env) return !strcmp(env, "on") || !strcmp(env, "1");
  if (wake_settings_read(WW_STORE_PATH(""), "cloud-verify", value, sizeof(value)) != 0) return 0;
  return !strcmp(value, "on");
}
#include "../wakeword/wake_policy.h"
#include "local-wake-epoch.inc"
/* GET /api/wakeword — 总览（词列表/守护状态/余量/录音进度/可链接脚本）。 */
static void h_ww_list(int fd) {
  /* A status request must never repair or rewrite user configuration. */
  ww_word_t w[WW_MAXWORDS];
  int nw = ww_read_conf(w, WW_MAXWORDS);
  char js[24576], st[WW_STATE_CAP], feature[64];
  ww_feature_id(feature, sizeof(feature));
  char **apps;
  int na = ww_linked_apps(&apps);
  int k = 0;
  {
    char bound[65] = "";
    int fallback = 0, bi = 0;
    char bindable[8][65];
    DIR *bd = opendir(APPPERSIST);
    struct dirent *de;
    lx06_official_bind_read(bound, sizeof(bound));
    for (int i = 0; i < nw; i++) if (w[i].enabled && !w[i].link[0]) fallback = 1;
    if (bd) {
      while ((de = readdir(bd)) && bi < 8) {
        if (de->d_name[0] == '.' || !app_name_safe(de->d_name)) continue;
        if (app_file_marker_flags(de->d_name) & APP_OFFICIAL_WAKEUP) {
          snprintf(bindable[bi], sizeof(bindable[bi]), "%s", de->d_name);
          bi++;
        }
      }
      closedir(bd);
    }
    int service = ww_service_up();
    k += snprintf(js + k, sizeof(js) - (size_t)k,
      "{\"ok\":true,\"local_wake\":1,\"dispatch_ready\":%s,\"management_ui_read_only\":false,\"service\":%d,\"data_free_kb\":%ld,\"official\":%d,\"official_bind\":\"%s\",\"official_fallback\":%d,\"cloud_verify\":%d,\"bindable\":[",
      service && (ww_epoch_capture_enabled() || access(WWHOME "/paired-capture-v1", R_OK)==0) ? "true" : "false", service,
      data_free_kb(), ww_official_read(), bound, fallback, ww_cloud_on());
    for (int i = 0; i < bi; i++) {
      char ae[128];
      json_escape(bindable[i], strlen(bindable[i]), ae, sizeof(ae));
      k += snprintf(js + k, sizeof(js) - (size_t)k, "%s\"%s\"", i ? "," : "", ae);
    }
    k += snprintf(js + k, sizeof(js) - (size_t)k, "],\"state\":");
  }
  if (ww_state_read(st, sizeof(st), NULL) == 0)
    k += snprintf(js + k, sizeof(js) - (size_t)k, "%s", st);
  else
    k += snprintf(js + k, sizeof(js) - (size_t)k, "null");
  k += snprintf(js + k, sizeof(js) - (size_t)k, ",\"words\":[");
  for (int i = 0; i < nw; i++) {
    char ne[48], le[128], samples[2048], recdir[96];
    json_escape(w[i].name, strlen(w[i].name), ne, sizeof(ne));
    json_escape(w[i].link, strlen(w[i].link), le, sizeof(le));
    int count = ww_reccount(w[i].name);
    int model = ww_model_exists(w[i].name);
    int needs_training = w[i].th > WW_GATE_CEILING || ww_calibration_required(&w[i], feature);
    int ready = count >= WW_MINRECS && model && !needs_training &&
                !w[i].p05_conflict && !w[i].p05_pending;
    snprintf(recdir, sizeof(recdir), "%s/%s", WWRECS, w[i].name);
    if (wake_samples_json(recdir, samples, sizeof(samples)) != 0)
      snprintf(samples, sizeof(samples), "[]");
    if (k < 0 || (size_t)k + strlen(samples) + 320 >= sizeof(js)) break;
    k += snprintf(js + k, sizeof(js) - (size_t)k,
      "%s{\"name\":\"%s\",\"th\":%.6f,\"link\":\"%s\",\"enabled\":%d,"
      "\"reccount\":%d,\"model\":%d,\"marker\":%d,\"calibration_required\":%d,\"conflict\":%d,\"near\":%d,\"pending\":%d,\"ready\":%d,\"samples\":%s}",
      i ? "," : "", ne, w[i].th, le, (int)w[i].enabled,
      count, model, w[i].link[0] ? !!(app_file_marker_flags(w[i].link) & APP_LOCAL_WAKEWORD) : 0,
      needs_training, w[i].p05_conflict, w[i].p05_near, w[i].p05_pending, ready, samples);
  }
  k += snprintf(js + k, sizeof(js) - (size_t)k, "],\"linked_apps\":[");
  for (int i = 0; i < na; i++) {
    char ae[128];
    json_escape(apps[i], strlen(apps[i]), ae, sizeof(ae));
    k += snprintf(js + k, sizeof(js) - (size_t)k, "%s\"%s\"", i ? "," : "", ae);
  }
  k += snprintf(js + k, sizeof(js) - (size_t)k, "],\"cleared\":[");
  {
    FILE *notice = fopen(WW_STORE_PATH("/journal/p05-cleared.txt"), "r");
    char line[128];
    int first = 1;
    if (notice) {
      while (k < (int)sizeof(js) - 80 && fgets(line, sizeof line, notice)) {
        char *sp = strchr(line, ' ');
        char ne[96];
        line[strcspn(line, "\r\n")] = 0;
        if (sp) *sp = 0;
        if (!line[0] || !strcmp(line, "-")) continue;
        json_escape(line, strlen(line), ne, sizeof ne);
        k += snprintf(js + k, sizeof(js) - (size_t)k, "%s\"%s\"", first ? "" : ",", ne);
        first = 0;
      }
      fclose(notice);
    }
  }
  k += snprintf(js + k, sizeof(js) - (size_t)k, "]}");
  for (int i = 0; i < na; i++) free(apps[i]);
  free(apps);
  send_json(fd, js);
}
/* POST /api/wakeword/official {"on":0/1} — 「官方小爱同学」唤醒词开关。
 * V28.2：开关控制「小爱同学」四字的唤醒（非对话开关）：
 *   开 → pns KWS 检出「小爱同学」→ 叮咚 + 官方云应答（几点了等）；
 *   关 → 说「小爱同学」不唤醒：wakeup.sh OFFICIAL-GATE 实时屏蔽 KWS 全部
 *        副作用（叮咚/LED/云上传）。pns 常驻不重启（它是唯一上行源：
 *        本地唤醒词 KWS/录音/小智/豆包流都来自其扇出），mico 官方云链路
 *        保持 —— 本地唤醒词（小智小智等）仍可对接官方接口。
 * 门控实时读 /data/wakeword/official_xiaoai → 开关即时生效。 */
static void h_ww_official(int fd, const char *body) {
  char en[8];
  if (app_json_str(body, "on", en, sizeof(en)) != 0) {
    app_http_error(fd, 400, "缺少参数 on"); return;
  }
  int on = (en[0] == '1' || !strcmp(en, "true"));
  if (ww_official_apply(on, 1) != 0) {
    app_http_error(fd, 500, "无法保存官方唤醒词开关"); return;
  }
  send_json(fd, on ? "{\"ok\":true,\"on\":1}" : "{\"ok\":true,\"on\":0}");
}
/* WT1-OFFICIAL-BIND-API §2.9. confirm=1 is required before a non-empty bind. */
static int lx06_finish_previous_binding(const char *previous) {
  struct lx06_session_record owner, active;
  char name[LX06_SESSION_NAME_SIZE];
  if (!*previous || !lx06_session_identity(&owner, name) || strcmp(previous, name))
    return 0;
  int stopped = lx06_session_preempt_expected(&owner);
  int64_t deadline = lk_now_ms() + 2500;
  /* Wait for the exact old session to release the microphone and player
   * before returning its routing to the stock wake callback. */
  while (lx06_session_read(&active) && !memcmp(&owner, &active, sizeof(owner))) {
    if (stopped < 0 || lk_now_ms() >= deadline) return -1;
    usleep(20000);
  }
  return 0;
}

static void h_ww_official_bind(int fd, const char *body) {
  char script[65], confirm[8], current[65];
  int have_script = 0, have_confirm = 0;
  if (app_json_strx(body, "script", script, sizeof(script), &have_script) != 0 || !have_script) {
    app_http_error(fd, 400, "缺少参数 script"); return;
  }
  if (script[0]) {
    if (!app_name_safe(script) || !(app_file_marker_flags(script) & APP_OFFICIAL_WAKEUP)) {
      app_http_error(fd, 400, "该脚本不能被小爱同学绑定"); return;
    }
    app_json_strx(body, "confirm", confirm, sizeof(confirm), &have_confirm);
    if (!have_confirm || confirm[0] != '1') {
      app_http_error(fd, 400, "绑定前需要确认影响"); return;
    }
  }
  int current_status = lx06_official_bind_read(current, sizeof(current));
  if (current_status < 0 || strcmp(current, script)) {
    if (script[0] && mkdir(WWBASE, 0755) != 0 && errno != EEXIST) {
      app_http_error(fd, 500, "无法保存官方唤醒绑定"); return;
    }
    if (lx06_stop_official_tts() != 0) {
      app_http_error(fd, 503, "无法停止小爱播报，绑定未切换"); return;
    }
    if (current_status == 1 && lx06_finish_previous_binding(current) != 0) {
      app_http_error(fd, 503, "原脚本会话尚未结束，绑定未切换"); return;
    }
    if (lx06_official_bind_write_at(LX06_OFFICIAL_BIND_PATH, script) != 0) {
      app_http_error(fd, 500, "无法保存官方唤醒绑定"); return;
    }
  }
  send_json(fd, script[0] ? "{\"ok\":true}" : "{\"ok\":true,\"script\":\"\"}");
}
/* 公共：从 body 取词名并校验存在。 */
static int ww_body_word(const char *body, char *name, size_t nsz) {
  if (app_json_str(body, "name", name, nsz) != 0) return -1;
  if (!ww_name_ok(name)) return -2;
  return 0;
}
static int ww_word_find(const ww_word_t *w, int nw, const char *name) {
  for (int i = 0; i < nw; i++)
    if (!strcmp(w[i].name, name)) return i;
  return -1;
}
/* Atomic no-replace only: unsupported kernels/filesystems fail closed. */
#include <fcntl.h>
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
static int ww_move_noreplace(int dir, const char *from, const char *to) {
#ifdef SYS_renameat2
  return (int)syscall(SYS_renameat2, dir, from, dir, to, RENAME_NOREPLACE);
#else
  (void)dir; (void)from; (void)to; errno = ENOSYS; return -1;
#endif
}
/* POST /api/wakeword/add {"name"} */
static void h_ww_add(int fd, const char *body) {
  char name[24], feature[64];
  ww_word_t w[WW_MAXWORDS];
  int nw;
  if (ww_body_word(body, name, sizeof(name)) != 0 || !ww_new_name_ok(name)) {
    app_http_error(fd, 400, "新唤醒词须为 3–4 个汉字"); return;
  }
  nw = ww_read_conf(w, WW_MAXWORDS);
  if (ww_word_find(w, nw, name) >= 0) { app_http_error(fd, 409, "该唤醒词已存在"); return; }
  if (nw >= WW_MAXWORDS) { app_http_error(fd, 409, "唤醒词已达上限（6 个）"); return; }
  if (data_free_kb() < WW_RECFREE_MIN_KB) { app_http_error(fd, 507, "/data 空间不足"); return; }
  ww_feature_id(feature, sizeof(feature));
  if (!feature[0]) { app_http_error(fd, 503, "唤醒组件未就绪"); return; }
  {
    int base, recs, models, word;
    const char *store = WW_STORE_PATH("");
    if (mkdir(store, 0755) != 0 && errno != EEXIST) { app_http_error(fd, 500, "无法创建唤醒词目录"); return; }
    base = open(store, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (base < 0) { app_http_error(fd, 500, "唤醒词目录不安全"); return; }
    if (mkdirat(base, "models", 0755) != 0 && errno != EEXIST) { close(base); app_http_error(fd, 500, "无法创建模型目录"); return; }
    if (mkdirat(base, "recs", 0755) != 0 && errno != EEXIST) { close(base); app_http_error(fd, 500, "无法创建录音根目录"); return; }
    models = openat(base, "models", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (models < 0) { close(base); app_http_error(fd, 500, "模型目录不安全"); return; }
    close(models);
    recs = openat(base, "recs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(base);
    if (recs < 0) { app_http_error(fd, 500, "录音根目录不安全"); return; }
    if (mkdirat(recs, name, 0755) != 0 && errno != EEXIST) { close(recs); app_http_error(fd, 500, "无法创建录音目录"); return; }
    word = openat(recs, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(recs);
    if (word < 0) { app_http_error(fd, 500, "录音目录不安全"); return; }
    close(word);
  }
  memset(&w[nw], 0, sizeof(w[nw]));
  w[nw].name[0] = 0; strcpy(w[nw].name, name);
  w[nw].th = 0.18;
  w[nw].enabled = 1;
  w[nw].p05_requested = 1;
  snprintf(w[nw].feature, sizeof(w[nw].feature), "%s", feature);
  if (ww_write_conf(w, nw + 1) != 0) { app_http_error(fd, 500, "写入配置失败"); return; }
  ww_log("ADD word=%s", name);
  ww_service_restart();
  send_json(fd, "{\"ok\":true,\"note\":\"请录入 3–5 条\"}");
}
/* POST /api/wakeword/rename {"old","new"} */
static void h_ww_rename(int fd, const char *body) {
  char oldn[24], newn[24];
  ww_word_t w[WW_MAXWORDS];
  int nw, i;
  if (app_json_str(body, "old", oldn, sizeof(oldn)) != 0 ||
      app_json_str(body, "new", newn, sizeof(newn)) != 0 ||
      !ww_name_ok(oldn) || !ww_new_name_ok(newn)) {
    app_http_error(fd, 400, "原唤醒词须为 2–6 个汉字，新名称须为 3–4 个汉字"); return;
  }
  nw = ww_read_conf(w, WW_MAXWORDS);
  if (ww_word_find(w, nw, oldn) < 0) { app_http_error(fd, 404, "原唤醒词不存在"); return; }
  if (!strcmp(oldn, newn)) { send_json(fd, "{\"ok\":true}"); return; }
  if (ww_word_find(w, nw, newn) >= 0) { app_http_error(fd, 409, "新名称已被占用"); return; }
  {
    char mo[32], mn[32];
    int base, recs, models, moved_model = 0;
    LX06_STATBUF st;
    base = open(WW_STORE_PATH(""), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (base < 0) { app_http_error(fd, 500, "唤醒词目录不安全"); return; }
    recs = openat(base, "recs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    models = openat(base, "models", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(base);
    if (recs < 0 || models < 0) {
      if (recs >= 0) close(recs);
      if (models >= 0) close(models);
      app_http_error(fd, 500, "数据目录不安全"); return;
    }
    snprintf(mo, sizeof(mo), "%s.lwt", oldn);
    snprintf(mn, sizeof(mn), "%s.lwt", newn);
    /* Residual namespace race: renameat2 names the source, not the inode
     * validated here. A concurrent writer to recs can swap oldn after this
     * check; dirfds and NOFOLLOW alone cannot make that check atomic. The
     * mutation lock serializes cooperating webd operations only. This rename
     * sequence also has no crash-recovery transaction. */
    if (LX06_STATAT(recs, oldn, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode)) {
      close(recs); close(models); app_http_error(fd, 500, "录音目录不安全"); return;
    }
    if (ww_move_noreplace(models, mo, mn) == 0) moved_model = 1;
    else if (errno != ENOENT) {
      close(recs); close(models); app_http_error(fd, 409, "模型重命名失败"); return;
    }
    if (ww_move_noreplace(recs, oldn, newn) != 0) {
      if (moved_model && ww_move_noreplace(models, mn, mo) != 0)
        ww_log("RENAME rollback model failed %s -> %s", newn, oldn);
      close(recs); close(models); app_http_error(fd, 500, "录音目录重命名失败"); return;
    }
    for (i = 0; i < nw; i++)
      if (!strcmp(w[i].name, oldn)) strcpy(w[i].name, newn);
    if (ww_write_conf(w, nw) != 0) {
      if (ww_move_noreplace(recs, newn, oldn) != 0)
        ww_log("RENAME rollback recordings failed %s -> %s", newn, oldn);
      if (moved_model && ww_move_noreplace(models, mn, mo) != 0)
        ww_log("RENAME rollback model failed %s -> %s", newn, oldn);
      close(recs); close(models); app_http_error(fd, 500, "写入配置失败"); return;
    }
    close(recs); close(models);
  }
  ww_log("RENAME %s -> %s", oldn, newn);
  ww_service_restart();
  send_json(fd, "{\"ok\":true}");
}
/* POST /api/wakeword/del {"name"} */
static void h_ww_del(int fd, const char *body) {
  char name[24];
  ww_word_t w[WW_MAXWORDS];
  int nw, idx, i;
  if (ww_body_word(body, name, sizeof(name)) != 0) {
    app_http_error(fd, 400, "唤醒词须为 2–6 个汉字"); return;
  }
  nw = ww_read_conf(w, WW_MAXWORDS);
  idx = ww_word_find(w, nw, name);
  if (idx < 0) { app_http_error(fd, 404, "唤醒词不存在"); return; }
  /* Disable before removing files. Partial cleanup leaves the word
   * available to retry deletion, with no usable model during that interval. */
  const char *store = WW_STORE_PATH("");
  if (ww_store_mark_unready(store, name) || ww_store_remove_word(store, name)) {
    app_http_error(fd, 500, "未能清理全部文件，请重试");
    return;
  }
  for (i = idx; i < nw - 1; i++) w[i] = w[i + 1];
  if (ww_write_conf(w, nw - 1) != 0) { app_http_error(fd, 500, "写入配置失败"); return; }
  ww_log("DEL word=%s", name);
  ww_service_restart();
  send_json(fd, "{\"ok\":true}");
}
/* POST /api/wakeword/update {"name","th"?,"link"?,"enabled"?} */
/* Threshold edits use the same locked, asynchronous all-word operation as
 * enrollment. The helper saves original c before default-disable/calibration. */
static int ww_joint_start(const char *name, const char *gate) {
  if (access(WWHOME "/wakeword_calibrate", X_OK) || ww_mutation_fd < 0) return -1;
  ww_state_write("training", name, 0, "正在保存原c矩阵并全词联合校准");
  pid_t worker = fork();
  if (worker < 0) { ww_state_write("error", name, 0, "无法启动全词校准"); return -1; }
  if (!worker) {
    signal(SIGCHLD, SIG_DFL);
    if (dup2(ww_mutation_fd, 9) < 0 || fcntl(9, F_SETFD, 0) < 0) _exit(1);
    setenv("LX06_WW_LOCK_HELD", "1", 1);
    int out = webd_log_sink("wake");
    if (out < 0) out = open("/dev/null", O_WRONLY);
    if (out >= 0) { dup2(out, 1); dup2(out, 2); if (out != 9) close(out); }
    for (int other=3;other<1024;other++) if (other!=9) close(other);
    char cmd[512];
    if (ww_epoch_capture_enabled()) {
      setenv("LX06_WAKE_DATA", WW_STORE_PATH(""), 1);
      setenv("LX06_WAKE_FORENSICS_DIR", WW_RAW_FORENSICS, 1);
    }
    if (name[0]) snprintf(cmd, sizeof(cmd), WWHOME "/wakeword_calibrate joint '%s' '%s' '%s' '%s'", WW_STORE_PATH(""), WW_JOINT_ROOT, name, gate);
    else snprintf(cmd, sizeof(cmd), WWHOME "/wakeword_calibrate joint '%s' '%s'", WW_STORE_PATH(""), WW_JOINT_ROOT);
    int ok = system(cmd) == 0;
    ww_state_write(ok ? "done" : "error", name, 0,
      ok ? "原c矩阵已保留；冲突默认禁用；校准结果另列" : "全词校准未完成，请查看原矩阵及录音，未计验收通过");
    if (ok && ww_service_up()) system("/etc/init.d/lx06wakeword restart >/dev/null 2>&1");
    _exit(ok ? 0 : 1);
  }
  return 0;
}
static void h_ww_update(int fd, const char *body) {
  char name[24];
  ww_word_t w[WW_MAXWORDS];
  int nw, idx;
  if (ww_body_word(body, name, sizeof(name)) != 0) {
    app_http_error(fd, 400, "唤醒词须为 2–6 个汉字"); return;
  }
  nw = ww_read_conf(w, WW_MAXWORDS);
  idx = ww_word_find(w, nw, name);
  if (idx < 0) { app_http_error(fd, 404, "唤醒词不存在"); return; }
  ww_word_t previous = w[idx];
  {
    /* 可选参数必须看 found：缺键≠空值（app_json_str 的历史陷阱，4332 段
     * 曾把「未带 th」当成 th="" → strtod=0 → 永远报阈值错，且每次 update
     * 都会把 enabled 清 0、link 清空）。 */
    int fth = 0, flink = 0, fen = 0;
    char ths[16], link[65], en[8];
    if (app_json_strx(body, "th", ths, sizeof(ths), &fth) != 0) fth = 0;
    if (app_json_strx(body, "link", link, sizeof(link), &flink) != 0) flink = 0;
    if (app_json_strx(body, "enabled", en, sizeof(en), &fen) != 0) fen = 0;
    if (fth) {
      char *end;
      double th = strtod(ths, &end);
      if (end == ths || *end || !(th >= 0.000001 && th <= WW_GATE_CEILING)) { app_http_error(fd, 400, "阈值不能超过 0.26，请重新录入"); return; }
      if (flink || fen) { app_http_error(fd, 400, "门限请单独保存，先完成全词校准再改启用或链接"); return; }
      char gate[24]; snprintf(gate, sizeof(gate), "%.6f", th);
      if (ww_joint_start(name, gate)) { app_http_error(fd, 503, "全词校准程序不可用，门限未保存"); return; }
      send_json(fd, "{\"ok\":true,\"pending\":1}"); return;
    }
    if (flink) {
      if (link[0] && (!app_name_safe(link) ||
                      (app_file_marker_flags(link) & APP_LOCAL_WAKEWORD) == 0)) {
        app_http_error(fd, 400, "链接脚本须含 localwakeup 标记"); return;
      }
      strcpy(w[idx].link, link);
    }
    if (fen && strcmp(en, "0") && strcmp(en, "1")) {
      app_http_error(fd, 400, "启用状态无效"); return;
    }
    if (fen && en[0] == '1') {
      char force[8] = ""; app_json_str(body, "force_conflict", force, sizeof(force));
      if (w[idx].p05_pending) { app_http_error(fd, 409, "全词校准未完成，请先重训"); return; }
      if (w[idx].p05_conflict && strcmp(force, "1")) {
        send_err(fd, 409, "Conflict", "{\"ok\":false,\"need_confirm\":true,\"error\":\"原c矩阵有冲突；明确强开不等于通过声学验收\"}"); return;
      }
      char feature[64];
      ww_feature_id(feature, sizeof(feature));
      if (ww_reccount(name) < WW_MINRECS || w[idx].th > WW_GATE_CEILING ||
          ww_calibration_required(&w[idx], feature)) {
        app_http_error(fd, 409, "录入不合格，请重新录入"); return;
      }
    }
    if (fen) {
      w[idx].enabled = (en[0] == '1');
      w[idx].p05_requested = w[idx].enabled;
    }
  }
  int detector_changed = previous.th != w[idx].th || previous.enabled != w[idx].enabled ||
                         strcmp(previous.feature, w[idx].feature) != 0 ||
                         strcmp(previous.model_sha256, w[idx].model_sha256) != 0;
  if (!detector_changed && previous.p05_requested == w[idx].p05_requested &&
      !strcmp(previous.link, w[idx].link)) {
    send_json(fd, "{\"ok\":true}"); return;
  }
  if (ww_write_conf(w, nw) != 0) { app_http_error(fd, 500, "写入配置失败"); return; }
  ww_log("UPDATE word=%s th=%.2f link=%s enabled=%d",
         name, w[idx].th, w[idx].link, w[idx].enabled);
  /* Routing is read by the dispatcher for each event. A link edit must not
   * restart the microphone/feature pipeline and lose the next word's onset. */
  if (detector_changed) ww_service_restart();
  send_json(fd, "{\"ok\":true}");
}
/* POST /api/wakeword/record {"name","test"?} — fork 跑 ww-record.sh，立即应答。 */
static int ww_recording_active(void);
static void h_ww_record(int fd, const char *body) {
  char name[24], ts[8], ns[8], purpose[16] = "", condition[16] = "";
  int replace = 0, test_req = 0, collection = 0, has_purpose = 0;
  if (ww_body_word(body, name, sizeof(name)) != 0) {
    app_http_error(fd, 400, "唤醒词须为 2–6 个汉字"); return;
  }
  if (app_json_strx(body, "purpose", purpose, sizeof(purpose), &has_purpose) != 0) {
    app_http_error(fd,400,"独立采集参数无效"); return;
  }
  if (has_purpose) {
    collection = !strcmp(purpose,"acceptance") || !strcmp(purpose,"calibration") || !strcmp(purpose,"diagnostic");
    if (!collection || app_json_str(body,"condition",condition,sizeof(condition)) ||
        (strcmp(condition,"normal") && strcmp(condition,"far") && strcmp(condition,"soft"))) {
      app_http_error(fd,400,"独立采集用途或位置无效"); return;
    }
  }
  test_req = app_json_str(body, "test", ts, sizeof(ts)) == 0 && ts[0] == '1';
  if (collection) test_req = 1;
  if (!test_req && app_json_str(body, "n", ns, sizeof(ns)) == 0 && ns[0]) {
    replace = atoi(ns);
    if (replace < 1 || replace > WW_MAXRECS) {
      app_http_error(fd, 400, "录音序号 1–10"); return;
    }
  }
  {
    ww_word_t w[WW_MAXWORDS];
    int nw = ww_read_conf(w, WW_MAXWORDS);
    int idx = ww_word_find(w, nw, name);
    if (idx < 0) { app_http_error(fd, 404, "唤醒词不存在"); return; }
    if (collection) {
      /* T03 must collect before migration, including words without new models. */
    } else if (test_req) {
      char feature[64];
      ww_feature_id(feature, sizeof(feature));
      if (ww_reccount(name) < WW_MINRECS || w[idx].p05_pending ||
          w[idx].th > WW_GATE_CEILING || ww_calibration_required(&w[idx], feature)) {
        app_http_error(fd, 409, "请先录满 3 条并完成训练"); return;
      }
    } else if (replace) {
      char path[120];
      LX06_STATBUF st;
      snprintf(path, sizeof(path), "%s/%s/%d.wav", WWRECS, name, replace);
      if (LX06_STAT(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        app_http_error(fd, 404, "要重录的录音不存在"); return;
      }
    } else {
      if (ww_reccount(name) >= WW_NEW_MAXRECS) {
        app_http_error(fd, 409, "最多保留 5 条录音"); return;
      }
    }
  }
  if (LX06_STAT("/tmp/local-audio-operation.lock", &(LX06_STATBUF){0}) == 0) {
    app_http_error(fd, 409, "音频占用中（正在播放），稍后再试"); return;
  }
  if (ww_recording_active()) {
    app_http_error(fd, 409, "已有录音/测试进行中，请稍候"); return;
  }
  if (lx06_session_busy()) {
    app_http_error(fd, 409, "正在对话或播放，请结束后再录音"); return;
  }
  if (!test_req && data_free_kb() < WW_RECFREE_MIN_KB) { app_http_error(fd, 507, "录音空间不足"); return; }
  {
    int test = test_req;
    pid_t pid;
    if (!collection && app_json_str(body, "test", ts, sizeof(ts)) == 0) test = (ts[0] == '1');
    /* Reserve in the single-threaded parent before fork: the next HTTP request
     * must not depend on when the child happens to get scheduled. */
    ww_state_write("recording", name, test, test ? "test" : "");
    if (!ww_recording_active()) {
      app_http_error(fd, 500, "无法写入录音状态"); return;
    }
    pid = fork();
    if (pid < 0) {
      ww_state_write("error", name, test, "无法启动录音进程");
      app_http_error(fd, 500, "无法启动录音进程"); return;
    }
    if (pid == 0) {
      /* 只执行固件内的录音组件，DATA 仅存用户录音和模型。 */
      char cmd[256];
      const char *wwbase = WWHOME;
      int fd2;
      /* The web server auto-reaps children, but this worker and the recorder
       * must wait for tone/capture/player exit statuses themselves. */
      signal(SIGCHLD, SIG_DFL);
      fd2 = open("/dev/null", O_RDWR);
      if (fd2 >= 0) { dup2(fd2, 0); dup2(fd2, 1); dup2(fd2, 2); close(fd2); }
      if (ww_mutation_fd >= 0) {
        if (dup2(ww_mutation_fd, 9) < 0 || fcntl(9, F_SETFD, 0) < 0) _exit(1);
        setenv("LX06_WW_LOCK_HELD", "1", 1);
      }
      if (collection)
        snprintf(cmd, sizeof(cmd), "sh %s/ww-record.sh \"%s\" %s %s", wwbase, name, purpose, condition);
      else if (replace)
        snprintf(cmd, sizeof(cmd), "sh %s/ww-record.sh \"%s\" \"\" %d", wwbase, name, replace);
      else
        snprintf(cmd, sizeof(cmd), "sh %s/ww-record.sh \"%s\"%s",
                 wwbase, name, test ? " test" : "");
      system(cmd);
      /* 录音可能生成/更新模型 → 重建 detect 管道加载新模型（~1s）。 */
      if (!test && ww_service_up()) system("/etc/init.d/lx06wakeword restart >/dev/null 2>&1");
      _exit(0);
    }
    send_json(fd, "{\"ok\":true,\"state\":\"recording\",\"note\":\"提示音后说出唤醒词\"}");
  }
}
/* POST /api/wakeword/service {"action":"start|stop|restart"} — 手动控制监听守护。 */
static void h_ww_service(int fd, const char *body) {
  char act[16];
  if (app_json_str(body, "action", act, sizeof(act)) != 0 ||
      (strcmp(act, "start") != 0 && strcmp(act, "stop") != 0 && strcmp(act, "restart") != 0)) {
    app_http_error(fd, 400, "action 须为 start/stop/restart"); return;
  }
  if (fork() == 0) {
    char cmd[80];
    int fd2;
    snprintf(cmd, sizeof(cmd), "/etc/init.d/lx06wakeword %s >/dev/null 2>&1", act);
    fd2 = open("/dev/null", O_RDONLY | O_WRONLY);
    if (fd2 >= 0) { dup2(fd2, 0); dup2(fd2, 1); dup2(fd2, 2); close(fd2); }
    system(cmd);
    _exit(0);
  }
  send_json(fd, "{\"ok\":true}");
}
/* The mutation lock determines whether a reserved recording or training job
 * is still active. Keep the word when reporting an abandoned worker. */
static int ww_recording_active(void) {
  char st[WW_STATE_CAP], word[24];
  int busy;
  LX06_STATBUF sb;
  if (ww_state_read(st, sizeof st, &busy) != 0 || !busy) return 0;
  if (LX06_STAT(WWSTATE, &sb) != 0) return 0;
  if (time(NULL) - (time_t)sb.st_mtime <= 2) return 1;
  int lock = open("/tmp/lx06-ww-mutate.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock < 0) return 1;
  if (flock(lock, LOCK_EX | LOCK_NB) != 0) { close(lock); return 1; }
  /* A trainer can finish between the first read and lock acquisition. */
  if (ww_state_read(st, sizeof st, &busy) != 0 || !busy) { close(lock); return 0; }
  unlink("/tmp/ww-record-lock/pid");
  rmdir("/tmp/ww-record-lock");
  if (app_json_str(st, "word", word, sizeof word) || (word[0] && !ww_name_ok(word))) word[0] = 0;
  ww_state_write("error", word, 0, "录音或训练中断，请重试");
  close(lock);
  return 0;
}
/* GET /api/wakeword/state */
static void h_ww_state(int fd) {
  char st[WW_STATE_CAP];
  int busy;
  int rc = ww_state_read(st, sizeof st, &busy);
  if (rc == 0 && busy && !ww_recording_active()) rc = ww_state_read(st, sizeof st, NULL);
  if (rc == 0) send_json(fd, st);
  else if (errno == ENOENT) send_json(fd, "{\"status\":\"idle\"}");
  else send_json(fd, "{\"status\":\"error\",\"word\":\"\",\"detail\":\"录音状态不完整，请重试\"}");
}
/* GET /api/wakeword/rec?name=<词>&n=<1-10> — 回放 WAV。 */
static void h_ww_rec_get(int fd, const char *q) {
  const char *pn = q ? strstr(q, "name=") : NULL;
  const char *pn2 = q ? strstr(q, "&n=") : NULL;
  char name[24], path[120];
  int f, n;
  if (!pn || !pn2 || pn + 5 >= pn2) { app_http_error(fd, 400, "参数无效"); return; }
  size_t used = 0;
  for (const char *p = pn + 5; p < pn2; p++) {
    int ch = (unsigned char)*p;
    if (ch == '%') {
      if (pn2 - p < 3 || !isxdigit((unsigned char)p[1]) || !isxdigit((unsigned char)p[2])) {
        app_http_error(fd, 400, "唤醒词无效"); return;
      }
      char hex[3] = {p[1], p[2], 0};
      ch = (int)strtol(hex, NULL, 16);
      p += 2;
    }
    if (!ch || used + 1 >= sizeof name) { app_http_error(fd, 400, "唤醒词无效"); return; }
    name[used++] = (char)ch;
  }
  name[used] = 0;
  if (!ww_name_ok(name)) { app_http_error(fd, 400, "唤醒词无效"); return; }
  n = atoi(pn2 + 3);
  if (n < 1 || n > 10) { app_http_error(fd, 400, "录音序号 1–10"); return; }
  snprintf(path, sizeof(path), "%s/%s/%d.wav", WWRECS, name, n);
  f = open(path, O_RDONLY);
  if (f < 0) { app_http_error(fd, 404, "录音不存在"); return; }
  {
    LX06_STATBUF st;
    if (LX06_FSTAT(f, &st) != 0 || !S_ISREG(st.st_mode)) { close(f); app_http_error(fd, 404, "录音不存在"); return; }
    if (st.st_size > 512 * 1024) { close(f); app_http_error(fd, 500, "录音文件过大"); return; }
    {
      char hdr[160];
      int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %ld\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (long)st.st_size);
      (void)write(fd, hdr, (size_t)hl);
      for (;;) {
        char chunk[4096];
        ssize_t r = read(f, chunk, sizeof(chunk));
        if (r <= 0) break;
        {
          ssize_t off = 0;
          while (off < r) {
            ssize_t w2 = write(fd, chunk + off, (size_t)(r - off));
            if (w2 <= 0) break;
            off += w2;
          }
          if (off < r) break;
        }
      }
    }
    close(f);
  }
}
/* Both explicit retraining and removing a sample use the same worker. */
static int ww_retrain_start(int fd, const char *name) {
  ww_state_write("training", name, 0, "正在重新训练");
  if (!ww_recording_active()) return -1;
  pid_t worker = fork();
  if (worker > 0) return 0;
  if (worker < 0) {
    ww_state_write("error", name, 0, "无法启动训练，请重试");
    return -1;
  }
  close(fd);
  signal(SIGCHLD, SIG_DFL);
  for (int other = 3; other < 1024; other++)
    if (other != ww_mutation_fd) close(other);
  char detail[180];
  int ok = ww_retrain(name, detail, sizeof detail) == 0;
  ww_log("RETRAIN word=%s result=%s %s", name, ok ? "done" : "error", detail);
  ww_state_write(ok ? "done" : "error", name, 0, detail);
  if (ok && ww_service_up()) system("/etc/init.d/lx06wakeword restart >/dev/null 2>&1");
  _exit(ok ? 0 : 1);
}

/* POST /api/wakeword/retrain {"name"} */
static void h_ww_retrain(int fd, const char *body) {
  char name[24];
  ww_word_t words[WW_MAXWORDS];
  if (ww_body_word(body, name, sizeof name) != 0) {
    app_http_error(fd, 400, "唤醒词无效");
    return;
  }
  int count = ww_read_conf(words, WW_MAXWORDS);
  if (ww_word_find(words, count, name) < 0) {
    app_http_error(fd, 404, "唤醒词不存在");
    return;
  }
  if (ww_reccount(name) < WW_MINRECS) {
    app_http_error(fd, 409, "请先录满 3 条");
    return;
  }
  if (access(WWHOME "/ww-train.sh", R_OK) || access(WWHOME "/wakeword_template", X_OK) ||
      access(WWHOME "/wakeword_calibrate", X_OK) || access(WWHOME "/ww_store", X_OK)) {
    ww_state_write("error", name, 0, "固件训练组件缺失，请更新固件");
    app_http_error(fd, 503, "固件训练组件缺失，请更新固件");
    return;
  }
  if (ww_retrain_start(fd, name)) {
    app_http_error(fd, 503, "无法启动训练，请重试");
    return;
  }
  send_json(fd, "{\"ok\":true,\"pending\":1}");
}

/* POST /api/wakeword/rec-del {"name","n"} — 删一条录音并重训（保留 ≥3 条）。 */
static void h_ww_rec_del(int fd, const char *body) {
  char name[24], ns[8], wav[16], side[24], temporary[32];
  LX06_STATBUF st;
  int n, cnt = 0;
  if (ww_body_word(body, name, sizeof(name)) != 0 ||
      app_json_str(body, "n", ns, sizeof(ns)) != 0) {
    app_http_error(fd, 400, "参数无效"); return;
  }
  if (!strcmp(ns, "10")) n = 10;
  else if (ns[0] >= '1' && ns[0] <= '9' && !ns[1]) n = ns[0] - '0';
  else { app_http_error(fd, 400, "录音序号 1–10"); return; }

  int recordings = open(WWRECS, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  int word = recordings < 0 ? -1 : openat(recordings, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (recordings >= 0) close(recordings);
  snprintf(wav, sizeof(wav), "%d.wav", n);
  if (word < 0 || LX06_STATAT(word, wav, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) {
    if (word >= 0) close(word);
    app_http_error(fd, 404, "录音不存在"); return;
  }
  for (int i = 1; i <= WW_MAXRECS; ++i) {
    char leaf[16];
    snprintf(leaf, sizeof(leaf), "%d.wav", i);
    if (!LX06_STATAT(word, leaf, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode)) ++cnt;
  }
  if (cnt - 1 < WW_MINRECS) {
    close(word);
    app_http_error(fd, 400, "至少保留 3 条录音"); return;
  }
  snprintf(side, sizeof(side), "%d.asr.json", n);
  snprintf(temporary, sizeof(temporary), "%d.asr.json.tmp", n);
  /* Keep actual sample numbers stable. Remove only the chosen recording and
   * its own recognition pair. The existing mutation lock also covers train. */
  if (ww_store_mark_unready(WW_STORE_PATH(""), name) ||
      (unlinkat(word, temporary, 0) && errno != ENOENT) ||
      (unlinkat(word, side, 0) && errno != ENOENT) ||
      unlinkat(word, wav, 0)) {
    close(word);
    app_http_error(fd, 500, "删除失败"); return;
  }
  if (fsync(word)) ww_log("REC-DEL-SYNC word=%s n=%d", name, n);
  close(word);
  /* The sample has been removed even if the worker cannot start. */
  if (ww_retrain_start(fd, name)) {
    send_json(fd, "{\"ok\":true,\"retrained\":0,\"note\":\"录音已删除，无法启动重训\"}");
  } else send_json(fd, "{\"ok\":true,\"pending\":1}");
}
/* All recording/model mutations share one kernel lock with the recorder and
 * background auto-trainer. The child inherits ownership across fork; the HTTP
 * parent releases its copy immediately instead of waiting for CPU work. */
static void h_ww_rec_play(int fd, const char *body);
static void ww_modify(void (*handler)(int,const char *), int fd, const char *body) {
  /* Enrollment is open. Config and trained models go through ww_store.
   * The flock only serializes cooperating recorder/trainer/webd processes. */
  if (ww_recording_active()) { app_http_error(fd, 409, "正在录音或训练，请稍候"); return; }
  if (!ww_epoch_capture_enabled() && noise_raw_selected()) {
    app_http_error(fd, 409, "本地唤醒音频组件版本不一致，请更新固件"); return;
  }
  int lock = open("/tmp/lx06-ww-mutate.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
    if (lock >= 0) close(lock);
    app_http_error(fd, 409, "正在录音或训练，请稍候"); return;
  }
  ww_mutation_fd = lock;
  if (ww_epoch_capture_enabled() && !ww_raw_store_ready(handler == h_ww_add))
    app_http_error(fd, 409, "录音配置尚未就绪");
  else handler(fd,body);
  ww_mutation_fd = -1;
  close(lock);
}
/* POST /api/wakeword/play {"name","n"} — 音箱外放这条录音，不走浏览器。 */
static void h_ww_rec_play(int fd, const char *body) {
  char name[24], ns[8], path[160], cmd[512];
  int n;
  if (ww_body_word(body, name, sizeof(name)) != 0 ||
      app_json_str(body, "n", ns, sizeof(ns)) != 0) {
    app_http_error(fd, 400, "参数无效"); return;
  }
  n = atoi(ns);
  if (n < 1 || n > 10) { app_http_error(fd, 400, "录音序号 1–10"); return; }
  snprintf(path, sizeof(path), "%s/%s/%d.wav", WWRECS, name, n);
  if (access(path, R_OK) != 0) { app_http_error(fd, 404, "录音不存在"); return; }
  if (lx06_session_busy()) { app_http_error(fd, 409, "正在对话或播放，请结束后再回放"); return; }
  if (access("/usr/lib/lx06/xiaoai/session-run", X_OK) != 0) {
    app_http_error(fd, 503, "回放管理组件缺失"); return;
  }
  pid_t worker = fork();
  if (worker < 0) { app_http_error(fd, 500, "无法启动回放"); return; }
  if (worker == 0) {
    /* Keep HTTP responsive while reporting the real player result. The inherited
     * mutation lock excludes recording/deletion until this worker has finished. */
    signal(SIGCHLD, SIG_DFL);
    for (int other = 3; other < 1024; ++other)
      if (other != fd && other != ww_mutation_fd) close(other);
    int fd2 = open("/dev/null", O_RDWR);
    if (fd2 >= 0) { dup2(fd2, 0); dup2(fd2, 1); dup2(fd2, 2); if (fd2 > 2) close(fd2); }
    int marker = open("/tmp/lx06-ww-preview", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (marker >= 0) close(marker);
    int logfd = webd_log_sink("wake");
    if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); close(logfd); }
    snprintf(cmd, sizeof(cmd), "LX06_SESSION_MAX_SECONDS=8 /usr/lib/lx06/xiaoai/session-run run wake-preview /usr/bin/aplay -q -D notify \"%s\"", path);
    int status = system(cmd);
    marker = open("/tmp/lx06-ww-preview", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (marker >= 0) close(marker);
    ww_log("PREVIEW word=%s n=%d status=%d", name, n, status);
    if (status == 0) send_json(fd, "{\"ok\":true,\"note\":\"回放已结束\"}");
    else app_http_error(fd, 503, "录音回放未完成，请刷新后重试");
    close(fd);
    _exit(0);
  }
}

/* --- Online upgrade integrity preflight (NO flash/DATA/env writes) --- */
static int ota_http_reject(conn_t *c, int code, const char *reason, const char *json) {
  send_err(c->fd, code, reason, json);
  conn_close(c);
  return 1;
}
static void ota_http_install_commit(conn_t *c, const ota_manifest *m, int streamed);
static void ota_receipt_feed(const conn_t *c, const void *bytes, size_t size);
static const char *ota_receipt_remember(const conn_t *c, const ota_manifest *m);

/* tmpfs quota measures filesystem capacity. Keep 16 MiB of physical
 * headroom after the remaining bytes of a staged upload land. */
#ifndef OTA_MEMINFO_PATH
#define OTA_MEMINFO_PATH "/proc/meminfo"
#endif
#define OTA_MEMORY_RESERVE_KB (16 * 1024)
#define OTA_MEMORY_RECHECK_BYTES (1024 * 1024)
#define OTA_BLOCK_BYTES 65536u
#define OTA_BODY_MAX (4096u + 46u * 1024u * 1024u)
static int ota_staging_memory_ok(size_t remaining) {
  FILE *f = fopen(OTA_MEMINFO_PATH, "r");
  char line[160];
  unsigned long long available_kb = 0;
  if (!f) return 0;
  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "MemAvailable: %llu kB", &available_kb) == 1) break;
  }
  fclose(f);
  return available_kb >= OTA_MEMORY_RESERVE_KB + (remaining + 1023ULL) / 1024ULL;
}

static void ota_http_data(conn_t *c, const void *bytes, size_t size) {
  char result[512];
  const ota_manifest *m;
  if (!c->ota) return;
  if (size > (size_t)c->bodywant - c->ota_received) {
    ota_http_reject(c, 400, "Bad Request", "{\"ok\":false,\"error\":\"上传长度与请求不符\"}"); return;
  }
  if (c->ota_install) {
    if (c->ota_received / OTA_MEMORY_RECHECK_BYTES !=
        (c->ota_received + size) / OTA_MEMORY_RECHECK_BYTES &&
        !ota_staging_memory_ok((size_t)c->bodywant - c->ota_received)) {
      ota_http_reject(c, 503, "Service Unavailable",
                      "{\"ok\":false,\"error\":\"insufficient_memory\"}"); return;
    }
    /* install-v1：同步流式落盘（只追加，失败即拒绝，不产生半包） */
    const unsigned char *p = (const unsigned char *)bytes;
    unsigned long long off = 0;
    while (off < size) {
      ssize_t w = write(c->ota_fd, p + off, size - off);
      if (w <= 0) {
        if (w < 0 && errno == EINTR) continue;
        ota_http_reject(c, 507, "Insufficient Storage",
                        "{\"ok\":false,\"error\":\"staging_write_failed\"}");
        return;
      }
      off += (unsigned long long)w;
    }
  }
  if (ota_stream_feed(c->ota, bytes, size)) {
    ota_http_reject(c, 422, "Unprocessable Content", "{\"ok\":false,\"error\":\"升级包格式或SHA-256校验失败，未写入系统\"}"); return;
  }
  if (c->ota_prepare) ota_receipt_feed(c, bytes, size);
  c->ota_received += size;
  if (c->ota_received != (size_t)c->bodywant) return;
  if (!sess_ok(c->ota_cookie)) {
    ota_http_reject(c, 401, "Unauthorized", "{\"ok\":false,\"error\":\"登录已失效，请重新登录\"}"); return;
  }
  if (ota_stream_finish(c->ota)) {
    ota_http_reject(c, 422, "Unprocessable Content", "{\"ok\":false,\"error\":\"升级包不完整或SHA-256校验失败，未写入系统\"}"); return;
  }
  m = ota_stream_manifest(c->ota);
  if (c->ota_install) {
    /* 整包校验通过（含 model=LX06）→ 槽位决策 + fork 安装 worker */
    ota_http_install_commit(c, m, 0);
    return;
  }
  if (c->ota_prepare) {
    const char *receipt = ota_receipt_remember(c, m);
    if (!receipt) {
      ota_http_reject(c, 503, "Service Unavailable",
                      "{\"ok\":false,\"error\":\"receipt_unavailable\"}");
      return;
    }
    snprintf(result, sizeof(result),
             "{\"ok\":true,\"verified\":true,\"version\":\"%s\",\"receipt\":\"%s\"}",
             m->version, receipt);
    send_json(c->fd, result);
    conn_close(c);
    return;
  }
  snprintf(result, sizeof(result), "{\"ok\":true,\"verified\":true,\"install_ready\":true,\"authenticated\":true,\"version\":\"%s\",\"kernel_size\":%lu,\"rootfs_size\":%lu,\"note\":\"完整性校验通过；如需安装请使用“固件升级”按钮\"}",
           m->version, (unsigned long)m->kernel_size, (unsigned long)m->rootfs_size);
  send_json(c->fd, result);
  conn_close(c);
}
/* --- 固件安装（A/B 槽位）：复用原系统 /sbin/mtd + /usr/bin/fw_env 机制 ---
 * verify-v1 全程只读（仅 ota_stream 状态）；install-v1 额外把包流式落盘 /tmp，
 * 在整包校验（格式 + 长度 + SHA-256，由 ota_stream_finish 完成，model=LX06
 * 亦在其内校验）通过后 fork 安装 worker：
 *   preparing → 槽位/几何复核，停音频服务
 *   wiping_data → （可选，勾选清空 DATA）用 boot 脚本同款命令直接清 mtd6/ubifs
 *   writing_kernel / verifying_kernel  → 内核流式写入非活动 boot 槽 + 读回哈希
 *   writing_rootfs / verifying_rootfs  → 根文件系统同理
 *   committing → fw_env -s boot_part <target>，再回读确认
 *   rebooting  → sync + /sbin/reboot -f
 * 每一步原子发布 /tmp/lx06-ota-status.json 供前端轮询；commit 之前任何失败都
 * 不影响正在运行的槽位 → 永远可重试、可重复升级（A/B 对称）。 */

#define OTA_UPLOAD_DIR   "/tmp/lx06-uploads"
#ifndef OTA_LOCK_FILE
#define OTA_LOCK_FILE    "/tmp/lx06-ota.lock"
#endif
#ifndef OTA_STATUS_FILE
#define OTA_STATUS_FILE  "/tmp/lx06-ota-status.json"
#endif
#define OTA_KERNEL_OFF   4096ULL        /* 与 online_upgrade_package.py 一致 */

/* 自带 SHA-256：读回校验不依赖 ota-verify 的内部实现 */
typedef struct {
  unsigned int h[8];
  unsigned long long total;
  unsigned char buf[64];
  unsigned int buflen;
} ota_sha;
#define OTA_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void ota_sha_block(ota_sha *s, const unsigned char *p) {
  static const unsigned int k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2, S0, S1, ch, mj;
  int i;
  for (i = 0; i < 16; ++i)
    w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) |
           ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
  for (i = 16; i < 64; ++i) {
    unsigned int s0 = OTA_ROTR(w[i-15], 7) ^ OTA_ROTR(w[i-15], 18) ^ (w[i-15] >> 3);
    unsigned int s1 = OTA_ROTR(w[i-2], 17) ^ OTA_ROTR(w[i-2], 19) ^ (w[i-2] >> 10);
    w[i] = w[i-16] + s0 + w[i-7] + s1;
  }
  a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
  e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
  for (i = 0; i < 64; ++i) {
    S1 = OTA_ROTR(e, 6) ^ OTA_ROTR(e, 11) ^ OTA_ROTR(e, 25);
    ch = (e & f) ^ ((~e) & g);
    t1 = h + S1 + ch + k[i] + w[i];
    S0 = OTA_ROTR(a, 2) ^ OTA_ROTR(a, 13) ^ OTA_ROTR(a, 22);
    mj = (a & b) ^ (a & c) ^ (b & c);
    t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
  s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}
static void ota_sha_update(ota_sha *s, const void *data, unsigned long size) {
  const unsigned char *p = (const unsigned char *)data;
  s->total += size;
  if (s->buflen) {
    unsigned int need = 64 - s->buflen;
    if (size < need) { memcpy(s->buf + s->buflen, p, size); s->buflen += (unsigned int)size; return; }
    memcpy(s->buf + s->buflen, p, need);
    ota_sha_block(s, s->buf);
    p += need; size -= need; s->buflen = 0;
  }
  while (size >= 64) { ota_sha_block(s, p); p += 64; size -= 64; }
  if (size) { memcpy(s->buf, p, size); s->buflen = (unsigned int)size; }
}
static void ota_sha_init(ota_sha *s) {
  s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
  s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
  s->total = 0; s->buflen = 0;
}
static void ota_sha_final(ota_sha *s, unsigned char out[32]) {
  unsigned long long bits = s->total * 8;
  unsigned char pad[128];
  unsigned long i;
  for (i = 0; i < sizeof(pad); ++i) pad[i] = 0;
  pad[0] = 0x80;
  /* 长度字段必须落在“首个填充块”的 56..63 字节处：
     buflen<56 时长度随本次 append 结束；否则先补满当前块、再输出独立长度块。
     旧版固定写 pad[64..71]，仅 buflen==56（或 bits==0）时恰好正确。 */
  for (i = 0; i < 8; ++i)
    pad[(s->buflen < 56) ? (56 - s->buflen + 7 - i) : (120 - s->buflen + 7 - i)]
      = (unsigned char)(bits >> (i * 8));
  ota_sha_update(s, pad, (s->buflen < 56) ? (56 - s->buflen + 8)
                                          : (56 - s->buflen + 8 + 64));
  for (i = 0; i < 8; ++i) {
    out[i*4]   = (unsigned char)(s->h[i] >> 24);
    out[i*4+1] = (unsigned char)(s->h[i] >> 16);
    out[i*4+2] = (unsigned char)(s->h[i] >> 8);
    out[i*4+3] = (unsigned char)s->h[i];
  }
}
static int ota_sha_matches(const unsigned char digest[32], const char *hex64) {
  static const char *digits = "0123456789abcdef";
  char tmp[65];
  int i;
  for (i = 0; i < 32; ++i) {
    tmp[i*2] = digits[digest[i] >> 4];
    tmp[i*2+1] = digits[digest[i] & 15];
  }
  tmp[64] = 0;
  return strcmp(tmp, hex64) == 0;
}

/* An install receipt pins the already verified body without keeping it in TMP.
 * This is the same integrity check as install-v1, including its trust model. */
struct ota_receipt {
  ota_manifest manifest;
  int64_t expires_ms;
  size_t length;
  char token[33];
  char session[128];
  unsigned char digests[(OTA_BODY_MAX + OTA_BLOCK_BYTES - 1) / OTA_BLOCK_BYTES][32];
};
static struct ota_receipt ota_verified, ota_candidate;
static ota_sha ota_candidate_block;

static void ota_receipt_feed(const conn_t *c, const void *bytes, size_t size) {
  const unsigned char *input = bytes;
  size_t offset = c->ota_received;
  while (size) {
    size_t available = OTA_BLOCK_BYTES - offset % OTA_BLOCK_BYTES;
    size_t take = size < available ? size : available;
    size_t block = offset / OTA_BLOCK_BYTES;
    if (available == OTA_BLOCK_BYTES) ota_sha_init(&ota_candidate_block);
    ota_sha_update(&ota_candidate_block, input, take);
    offset += take;
    if (take == available || offset == (size_t)c->bodywant)
      ota_sha_final(&ota_candidate_block, ota_candidate.digests[block]);
    input += take;
    size -= take;
  }
}

static const char *ota_receipt_remember(const conn_t *c, const ota_manifest *m) {
  static const char hex[] = "0123456789abcdef";
  unsigned char nonce[16];
  size_t i;
  if (sess_token_from_cookie(c->ota_cookie, ota_candidate.session, sizeof(ota_candidate.session)) ||
      rand_bytes(nonce, sizeof(nonce))) return NULL;
  for (i = 0; i < sizeof(nonce); ++i) {
    ota_candidate.token[2*i] = hex[nonce[i] >> 4];
    ota_candidate.token[2*i+1] = hex[nonce[i] & 15];
  }
  ota_candidate.token[32] = 0;
  ota_candidate.manifest = *m;
  ota_candidate.length = c->ota_received;
  ota_candidate.expires_ms = lk_now_ms() + 120000;
  ota_verified = ota_candidate;
  return ota_verified.token;
}

/* The forked installer owns the upload socket. Only checked bytes from one
 * block at a time are exposed to the inactive-slot writer. */
struct ota_upload {
  const conn_t *conn;
  int64_t deadline;
  size_t prefix_used, read_bytes, block_used, block_size;
  unsigned char block[OTA_BLOCK_BYTES];
  const char *error;
};

static int ota_upload_next(struct ota_upload *u) {
  const conn_t *c = u->conn;
  size_t left = ota_verified.length - u->read_bytes;
  size_t wanted = left < sizeof(u->block) ? left : sizeof(u->block);
  size_t prefix = c->buflen - c->head_end - u->prefix_used;
  size_t got = prefix < wanted ? prefix : wanted;
  ota_sha sha;
  unsigned char digest[32];
  size_t index = u->read_bytes / sizeof(u->block);
  if (!wanted) { u->error = "upload_cancelled"; return -1; }
  if (!sess_ok(c->ota_cookie)) { u->error = "session_expired"; return -1; }
  if (!(index % (OTA_MEMORY_RECHECK_BYTES / OTA_BLOCK_BYTES)) &&
      !ota_staging_memory_ok(sizeof(u->block))) {
    u->error = "insufficient_memory"; return -1;
  }
  memcpy(u->block, c->buf + c->head_end + u->prefix_used, got);
  u->prefix_used += got;
  while (got < wanted) {
    struct pollfd fd = { c->fd, POLLIN, 0 };
    int64_t remaining = u->deadline - lk_now_ms();
    int ready;
    ssize_t n;
    if (remaining <= 0) { u->error = "upload_cancelled"; return -1; }
    ready = poll(&fd, 1, remaining < 30000 ? (int)remaining : 30000);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) { u->error = "upload_cancelled"; return -1; }
    n = recv(c->fd, u->block + got, wanted - got, MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n <= 0) { u->error = "upload_cancelled"; return -1; }
    got += (size_t)n;
  }
  ota_sha_init(&sha);
  ota_sha_update(&sha, u->block, wanted);
  ota_sha_final(&sha, digest);
  if (memcmp(digest, ota_verified.digests[index], sizeof(digest))) {
    u->error = "upload_changed"; return -1;
  }
  u->read_bytes += wanted;
  u->block_size = wanted;
  u->block_used = 0;
  return 0;
}

static ssize_t ota_upload_read(struct ota_upload *u, void *buf, size_t size) {
  size_t available;
  if (u->block_used == u->block_size && ota_upload_next(u)) return -1;
  available = u->block_size - u->block_used;
  if (size > available) size = available;
  memcpy(buf, u->block + u->block_used, size);
  u->block_used += size;
  return (ssize_t)size;
}

/* fw_env 子进程隔离（webd 的信号/继承 fd 都会影响 fork 后行为） */
static int ota_fw_env_get(const char *name, char *out, size_t outsz) {
  int p[2], st, invalid = 0;
  pid_t pid, waited;
  char chunk[256];
  size_t used = 0;
  if (!outsz) return -1;
  out[0] = 0;
  if (pipe(p)) return -1;
  pid = fork();
  if (pid < 0) { close(p[0]); close(p[1]); return -1; }
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    close(p[0]);
    {
      int dn = open("/dev/null", O_RDWR);
      int ok = (dup2(p[1], 1) >= 0);
      if (dn >= 0) {
        if (dup2(dn, 0) < 0) ok = 0;
        if (dup2(dn, 2) < 0) ok = 0;
        if (dn > 2) close(dn);
      }
      if (ok) execl("/usr/bin/fw_env", "fw_env", "-g", name, (char *)0);
    }
    _exit(127);
  }
  close(p[1]);
  for (;;) {
    ssize_t rd = read(p[0], chunk, sizeof(chunk));
    if (rd < 0 && errno == EINTR) continue;
    if (rd < 0) { invalid = 1; break; }
    if (rd == 0) break;
    /* Drain the child even on invalid output so it can exit. Never treat a
       truncated value or a diagnostic containing NUL as a valid setting. */
    if ((size_t)rd > outsz - used - 1 || memchr(chunk, '\0', (size_t)rd)) invalid = 1;
    if (!invalid) {
      memcpy(out + used, chunk, (size_t)rd);
      used += (size_t)rd;
    }
  }
  close(p[0]);
  do { waited = waitpid(pid, &st, 0); } while (waited < 0 && errno == EINTR);
  if (invalid || waited != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    out[0] = 0;
    return -1;
  }
  while (used && (out[used-1] == '\n' || out[used-1] == '\r' || out[used-1] == ' ')) used--;
  out[used] = 0;
  if (used == 0) return -1; /* R1 实测定案: 厂商 fw_env 静默失败 (rc=0 + 无输出,
     nand_env_read 瞬时失败, 高内存压力下复现) — 空输出必须视为读失败,
     否则空串撞机型门禁误报 device_model_unsupported (R1 422 根因)。 */
  /* R1b 实测定案 (44MB 压测 120/120): 高压下读失败 = rc=0 + stdout 错误文本
     ([ubootenv] read error / Cannot read /dev/nand_env: -5, 至多 389 字节,
     多行 CRLF) + 末尾 "(null)"。合法值必为单行干净值 (实机仅 3/5/6/10/
     boot0/boot1), 否则按读失败 (重试), 防脏值撞机型门禁误报 422。 */
  if (memchr(out, '\n', used) || memchr(out, '\r', used) ||
      (used >= 9 && memcmp(out, "[ubootenv]", 9) == 0) ||
      (used == 6 && memcmp(out, "(null)", 6) == 0)) {
    out[0] = 0;
    return -1;
  }
  return 0;
}
/* 父进程 SIGCHLD=SIG_IGN 会自动回收子进程 → waitpid 拿不到退出码；
   在 fork/waitpid 窗口临时恢复 SIG_DFL（窗口毫秒级，仅影响本 pid）。 */
static int ota_parent_fw_env_get(const char *name, char *out, size_t outsz) {
  struct sigaction sa, old;
  int rc;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = SIG_DFL;
  sa.sa_flags = 0;
  if (sigaction(SIGCHLD, &sa, &old) != 0) return -1;
  rc = ota_fw_env_get(name, out, outsz);
  (void)sigaction(SIGCHLD, &old, NULL);
  return rc;
}

/* fw_env 读取失败重试：fork/pipe/NAND 读取失败（116MB RAM 实机 44MB tmpfs 上传后
   MemAvailable 贴近 0，fork 可 ENOMEM；厂商 fw_env 裸 NAND 读亦可瞬时失败）
   重试一次，并与"值不匹配"区分——不得与机型/槽位门禁合并，
   否则在低内存下误报 device_model_unsupported。500ms 阻塞可接受：
   安装是稀有重操作，主循环此前已花数秒接收上传。 */
static int ota_fw_env_get_retry(const char *name, char *out, size_t outsz) {
  int attempt;
  for (attempt = 0; attempt < 3; attempt++) {
    if (ota_parent_fw_env_get(name, out, outsz) == 0) return 0;
    if (attempt < 2) {
      struct timespec ts = { 0, 500 * 1000 * 1000 };
      nanosleep(&ts, 0);
    }
  }
  return -1;
}

static int ota_fw_env_set(const char *name, const char *value) {
  int st;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    {
      int dn = open("/dev/null", O_RDWR);
      if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
    }
    execl("/usr/bin/fw_env", "fw_env", "-s", name, value, (char *)0);
    _exit(127);
  }
  if (waitpid(pid, &st, 0) != pid) return -1;
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

/* R1b: 启动时 (无 46MB tmpfs 高压窗) 缓存 board_id/boot_part。
   install 路径高压窗内 fw_env 读 120/120 全败 (nand_env_read EIO,
   rc=0 + 错误文本/(null)/空) → 门禁/worker/状态端点读失败时回退本缓存;
   缓存不可用 (valid=0) 时走原"读失败=503/中止"路径, 行为不劣化。 */
static int ota_boot_env_cache_valid = 0;
static char ota_boot_env_cache_board[16] = "";
static char ota_boot_env_cache_boot[8] = "";
static void ota_boot_env_cache_init(void) {
  char b[16] = "", s[8] = "";
  if (ota_fw_env_get_retry("board_id", b, sizeof(b)) == 0 &&
      ota_fw_env_get_retry("boot_part", s, sizeof(s)) == 0 &&
      (strcmp(s, "boot0") == 0 || strcmp(s, "boot1") == 0)) {
    snprintf(ota_boot_env_cache_board, sizeof(ota_boot_env_cache_board), "%s", b);
    snprintf(ota_boot_env_cache_boot, sizeof(ota_boot_env_cache_boot), "%s", s);
    ota_boot_env_cache_valid = 1;
    fprintf(stderr, "[LX06OTA] boot env cache board_id=%s boot_part=%s\n", b, s);
  } else {
    fprintf(stderr, "[LX06OTA] boot env cache unavailable (board=%s boot=%s)\n", b, s);
  }
}

/* /proc/mtd → 分区表（设备号/大小/名字） */
typedef struct { int num; unsigned long long size; char name[32]; } ota_mtd_part;
static int ota_mtd_scan(ota_mtd_part *parts, int maxparts) {
  FILE *f = fopen("/proc/mtd", "r");
  char line[160];
  int n = 0;
  if (!f) return 0;
  while (n < maxparts && fgets(line, sizeof(line), f)) {
    char *colon = strchr(line, ':');
    unsigned int dev = 0, esz = 0;
    unsigned long long size = 0;
    char name[32] = "";
    if (!colon) continue;
    if (sscanf(line, "mtd%u:", &dev) != 1) continue;
    if (sscanf(colon + 1, "%llx %x \"%31[a-zA-Z0-9_]\"", &size, &esz, name) != 3) continue;
    parts[n].num = (int)dev;
    parts[n].size = size;
    snprintf(parts[n].name, sizeof(parts[n].name), "%s", name);
    n++;
  }
  fclose(f);
  return n;
}
static ota_mtd_part *ota_mtd_find(const ota_mtd_part *parts, int n, const char *name) {
  int i;
  for (i = 0; i < n; ++i)
    if (strcmp(parts[i].name, name) == 0) return (ota_mtd_part *)&parts[i];
  return 0;
}

/* 原子状态文件：rename 落地，前端 GET /api/upgrade/status 轮询。
   state/phase/source/target 为固定词；version 来自已校验 manifest；
   error 为固定错误码 —— 均不含 JSON 元字符。 */
static void ota_install_status(const char *state, const char *phase,
                               const char *version, const char *source,
                               const char *target, int wipe, const char *error) {
  char tmp[160], out[1100];
  int fd, off;
  long ts = (long)time(NULL);
  snprintf(tmp, sizeof(tmp), "%s.tmp.%d", OTA_STATUS_FILE, (int)getpid());
  off = snprintf(out, sizeof(out),
    "{\"state\":\"%s\",\"phase\":\"%s\",\"version\":\"%s\",\"source\":\"%s\","
    "\"target\":\"%s\",\"wipe\":%d,\"error\":",
    state, phase, version, source, target, wipe);
  if (off < 0 || (size_t)off >= sizeof(out)) return;
  if (error && *error) {
    off += snprintf(out + off, sizeof(out) - (size_t)off, "\"%s\"", error);
  } else {
    off += snprintf(out + off, sizeof(out) - (size_t)off, "null");
  }
  if (off < 0 || (size_t)off >= sizeof(out)) return;
  off += snprintf(out + off, sizeof(out) - (size_t)off, ",\"ts\":%ld}", ts);
  if (off < 0 || (size_t)off >= sizeof(out)) return;
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) {
    (void)!write(fd, out, (size_t)off);
    fsync(fd);
    close(fd);
    rename(tmp, OTA_STATUS_FILE);
  }
}

/* 安装期间临时停用的音频/通话服务（rc.common 脚本，procd 不会自动拉起） */
static const char *ota_audio_services[] = {
  "mico_aivs_lab", "idmruntime", "mibrain_service", "voip",
  "mediaplayer", "quickplayer", "miplay", "sound_effect", 0
};
static void ota_audio_run(const char *action) {
  int i;
  for (i = 0; ota_audio_services[i]; ++i) {
    char path[64];
    int st;
    pid_t pid;
    snprintf(path, sizeof(path), "/etc/init.d/%s", ota_audio_services[i]);
    if (access(path, X_OK) != 0) continue;
    pid = fork();
    if (pid < 0) continue;
    if (pid == 0) {
      signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
      {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
      }
      execv("/bin/sh", (char *[]){ "/bin/sh", path, (char *)action, (char *)0 });
      _exit(127);
    }
    (void)waitpid(pid, &st, 0);
  }
  sleep(1);
}
static void ota_audio_stop(void) { ota_audio_run("stop"); }
static void ota_audio_start(void) { ota_audio_run("start"); }

static void ota_kill_part(pid_t pid) {
  (void)kill(pid, SIGKILL);
  while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

/* Allow a finished child to be reaped even at the transaction deadline. */
static int ota_wait_part(pid_t pid, int64_t deadline) {
  int status;
  for (;;) {
    pid_t done = waitpid(pid, &status, deadline ? WNOHANG : 0);
    if (done == pid) return WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
    if (done < 0 && errno != EINTR) return -1;
    if (deadline && lk_now_ms() >= deadline) {
      ota_kill_part(pid);
      return -2;
    }
    if (done == 0) usleep(10000);
  }
}

/* 把 file[off, off+size) 流式送入 `mtd write - /dev/mtdN`（stdin） */
static int ota_mtd_stream_write(const char *file, unsigned long long off,
                                unsigned long long size, int mtd_num,
                                struct ota_upload *upload) {
  char dev[32];
  char *argv[] = { "/sbin/mtd", "write", "-", dev, 0 };
  int src = -1, p[2], status;
  unsigned long long sent = 0;
  unsigned char buf[65536];
  pid_t pid, owner = getpid();
  snprintf(dev, sizeof(dev), "/dev/mtd%d", mtd_num);
  if (!upload) {
    src = open(file, O_RDONLY | O_CLOEXEC);
    if (src < 0) return -1;
    if (lseek(src, (off_t)off, SEEK_SET) < 0) { close(src); return -1; }
  }
  if (pipe(p)) { close(src); return -1; }
  if (upload && fcntl(p[1], F_SETFL, O_NONBLOCK) < 0) {
    close(p[0]); close(p[1]); close(src); return -1;
  }
  pid = fork();
  if (pid < 0) { close(p[0]); close(p[1]); close(src); return -1; }
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    close(p[1]);
    if (upload && (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner)) _exit(127);
    if (dup2(p[0], 0) < 0) _exit(127);
    close(p[0]);
    {
      int dn = open("/dev/null", O_RDWR);
      if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); if (dn > 2) close(dn); }
    }
    execv("/sbin/mtd", argv);
    _exit(127);
  }
  close(p[0]);
  while (sent < size) {
    size_t want = size - sent < sizeof(buf) ? (size_t)(size - sent) : sizeof(buf);
    ssize_t r = upload ? ota_upload_read(upload, buf, want) : read(src, buf, want);
    size_t offp = 0;
    if (!upload && r < 0 && errno == EINTR) continue;
    if (r <= 0) goto stream_failed;
    while (offp < (size_t)r) {
      ssize_t w = write(p[1], buf + offp, (size_t)r - offp);
      if (w <= 0) {
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && upload && errno == EAGAIN) {
          struct pollfd fd = { p[1], POLLOUT, 0 };
          int64_t remaining = upload->deadline - lk_now_ms();
          int ready = remaining > 0 ? poll(&fd, 1, remaining < 30000 ? (int)remaining : 30000) : 0;
          if ((ready > 0 && (fd.revents & POLLOUT)) || (ready < 0 && errno == EINTR)) continue;
          if (ready == 0) upload->error = "partition_write_timeout";
        }
        goto stream_failed;
      }
      offp += (size_t)w;
    }
    sent += (unsigned long long)r;
  }
  close(p[1]);
  close(src);
  status = ota_wait_part(pid, upload ? upload->deadline : 0);
  if (status == -2 && upload) upload->error = "partition_write_timeout";
  return status == 0 ? 0 : -1;

stream_failed:
  close(p[1]);
  close(src);
  if (upload) ota_kill_part(pid);
  else (void)ota_wait_part(pid, 0);
  return -1;
}
/* `mtd -q -l size dump <name>` 读回整段并比对 SHA-256 */
static int ota_mtd_stream_verify(unsigned long long size, const char *mtd_name,
                                 const char *expected_hex, int64_t deadline) {
  char sizes[24];
  char *argv[] = { "/sbin/mtd", "-q", "-l", sizes, "dump", (char *)mtd_name, 0 };
  int p[2];
  unsigned long long got = 0;
  unsigned char digest[32], chunk[65536];
  ota_sha sha;
  pid_t pid, owner = getpid();
  snprintf(sizes, sizeof(sizes), "%llu", size);
  if (pipe(p)) return -1;
  pid = fork();
  if (pid < 0) { close(p[0]); close(p[1]); return -1; }
  if (pid == 0) {
    signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_DFL);
    close(p[0]);
    if (deadline && (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner)) _exit(127);
    if (dup2(p[1], 1) < 0) _exit(127);
    close(p[1]);
    {
      int dn = open("/dev/null", O_RDWR);
      if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); if (dn > 2) close(dn); }
    }
    execv("/sbin/mtd", argv);
    _exit(127);
  }
  close(p[1]);
  ota_sha_init(&sha);
  for (;;) {
    if (deadline) {
      struct pollfd fd = { p[0], POLLIN, 0 };
      int64_t remaining = deadline - lk_now_ms();
      int ready = remaining > 0 ? poll(&fd, 1, remaining < 30000 ? (int)remaining : 30000) : 0;
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0 || !(fd.revents & (POLLIN | POLLHUP))) {
        close(p[0]);
        ota_kill_part(pid);
        return -1;
      }
    }
    ssize_t r = read(p[0], chunk, sizeof(chunk));
    if (r < 0) {
      if (errno == EINTR) continue;
      close(p[0]); (void)ota_wait_part(pid, deadline);
      return -1;
    }
    if (r == 0) break;
    ota_sha_update(&sha, chunk, (unsigned long)r);
    got += (unsigned long long)r;
  }
  close(p[0]);
  if (ota_wait_part(pid, deadline) != 0 || got != size) return -1;
  ota_sha_final(&sha, digest);
  if (!ota_sha_matches(digest, expected_hex)) {
    char dh[65];
    for (int di = 0; di < 32; ++di)
      snprintf(dh + di * 2, 3, "%02x", digest[di]);
    fprintf(stderr, "[LX06OTA] readback name=%s size=%llu expected=%s digest=%s\n",
            mtd_name, (unsigned long long)size, expected_hex, dh);
  }
  return ota_sha_matches(digest, expected_hex) ? 0 : -1;
}

struct ota_install_plan {
  const char *pkg;
  const char *version;
  unsigned long long kernel_size;
  unsigned long long rootfs_size;
  const char *kernel_hash;
  const char *rootfs_hash;
  int wipe;
  int lock_fd;            /* worker 持有到 _exit，flock 随之释放 */
  const char *source_slot;/* "boot0" | "boot1"（指向 fork 前静态区） */
  struct ota_upload *upload;
};

static int ota_read_boot_id(char out[37]) {
  char id[40];
  FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
  out[0] = 0;
  if (!f) return -1;
  int available = fgets(id, sizeof(id), f) != NULL;
  fclose(f);
  if (!available || strcspn(id, "\n") != 36) return -1;
  for (size_t i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') return -1;
    } else if (!isxdigit((unsigned char)id[i])) return -1;
  }
  memcpy(out, id, 36);
  out[36] = 0;
  return 0;
}

static int ota_running_system_mtd(void) {
  char line[512], device[64], target[64], fs[32], mode[8], extra;
  FILE *f;
  int dev = -1;
  f = fopen("/proc/mounts", "r");
  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    int candidate;
    if (sscanf(line, "%63s %63s %31s %7[^, ]", device, target, fs, mode) != 4 ||
        strcmp(target, "/") || strcmp(fs, "squashfs") || strcmp(mode, "ro") ||
        sscanf(device, "/dev/mtdblock%d%c", &candidate, &extra) != 1 || candidate < 0) continue;
    if (dev >= 0 && dev != candidate) { dev = -1; break; }
    dev = candidate;
  }
  fclose(f);
  return dev;
}

static const char *ota_running_slot(void) {
  ota_mtd_part parts[16];
  int mtd = ota_running_system_mtd();
  if (mtd < 0) return "";
  int count = ota_mtd_scan(parts, 16);
  const ota_mtd_part *first = ota_mtd_find(parts, count, "system0");
  const ota_mtd_part *second = ota_mtd_find(parts, count, "system1");
  if (!first || !second || first->num == second->num) return "";
  if (mtd == first->num) return "boot0";
  if (mtd == second->num) return "boot1";
  return "";
}

static int ota_source_still_active(const char *source, int system_mtd) {
  char curr[32];
  return ota_running_system_mtd() == system_mtd &&
         !ota_fw_env_get_retry("boot_part", curr, sizeof(curr)) && !strcmp(curr, source);
}

static int ota_upload_connected(const struct ota_upload *upload) {
  char extra;
  ssize_t n;
  do {
    n = recv(upload->conn->fd, &extra, 1, MSG_DONTWAIT | MSG_PEEK);
  } while (n < 0 && errno == EINTR);
  return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

static void ota_install_worker(const struct ota_install_plan *plan) {
  ota_mtd_part parts[16];
  const ota_mtd_part *kb, *kbo, *s0p, *s1p, *tboot, *tsys;
  const char *tname, *tsname;
  const char *phase = "preparing";
  char cur[32] = "";
  char source_boot_id[37] = "";
  char err[48] = "";
  int n, failed = 0, i, audio_stopped = 0;
  int upload_fd = plan->upload ? plan->upload->conn->fd : -1;
  int64_t deadline = plan->upload ? plan->upload->deadline : 0;

#define OTA_FAIL(why) do { \
    if (!failed) { failed = 1; snprintf(err, sizeof(err), "%s", (why)); } \
    goto ota_worker_done; \
  } while (0)

  setsid();
  signal(SIGCHLD, SIG_DFL);
  signal(SIGPIPE, SIG_IGN);
  tname = plan->source_slot[4] == '0' ? "boot1" : "boot0";
  for (i = 3; i < 1024; ++i)
    if (i != plan->lock_fd && i != upload_fd) close(i);
  if (plan->upload) {
    struct timeval timeout = { 5, 0 };
    if (fcntl(upload_fd, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(upload_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)))
      OTA_FAIL("upload_unavailable");
    if (ota_read_boot_id(source_boot_id)) OTA_FAIL("boot_identity_unavailable");
  }

  fprintf(stderr, "[LX06OTA] worker start pkg=%s wipe=%d\n", plan->upload ? "stream" : plan->pkg, plan->wipe);
  ota_install_status("running", "preparing", plan->version,
                     plan->source_slot, tname, plan->wipe, 0);
  n = ota_mtd_scan(parts, 16);
  if (n <= 0) OTA_FAIL("mtd_layout_unavailable");
  if (ota_fw_env_get_retry("boot_part", cur, sizeof(cur)) != 0) {
    /* R1b: 高压窗读败 → 启动缓存兜底 (无缓存才中止) */
    if (plan->upload || !ota_boot_env_cache_valid) OTA_FAIL("boot_part_unavailable");
    snprintf(cur, sizeof(cur), "%s", ota_boot_env_cache_boot);
  }
  if (strcmp(cur, plan->source_slot) != 0)
    OTA_FAIL("source_slot_changed");
  kb = ota_mtd_find(parts, n, "boot0");
  kbo = ota_mtd_find(parts, n, "boot1");
  s0p = ota_mtd_find(parts, n, "system0");
  s1p = ota_mtd_find(parts, n, "system1");
  if (!kb || !kbo || !s0p || !s1p) OTA_FAIL("slot_names_unavailable");
  tsname = plan->source_slot[4] == '0' ? "system1" : "system0";
  tboot = ota_mtd_find(parts, n, tname);
  tsys = ota_mtd_find(parts, n, tsname);
  if (!tboot || !tsys) OTA_FAIL("target_slot_unavailable");
  if (kb->size != kbo->size || kb->size < 0x600000) OTA_FAIL("boot_geometry_unexpected");
  if (plan->kernel_size > tboot->size) OTA_FAIL("kernel_too_large_for_slot");
  if (plan->rootfs_size > tsys->size) OTA_FAIL("rootfs_too_large_for_slot");

  if (plan->upload) {
    if (!ota_source_still_active(plan->source_slot, plan->source_slot[4] == '0' ? s0p->num : s1p->num))
      OTA_FAIL("source_slot_changed");
    if (ota_upload_next(plan->upload)) OTA_FAIL(plan->upload->error);
    if (plan->upload->block_size <= OTA_KERNEL_OFF) OTA_FAIL("upload_changed");
    plan->upload->block_used = OTA_KERNEL_OFF;
  } else {
    ota_audio_stop();
    audio_stopped = 1;
  }
  /* 官方 stop_service 模式：写前 sync + 丢弃页缓存（运行中系统的 rootfs 页），
   * 降低刷写期 RAM 压力。失败忽略（模拟环境无 /proc/sys/vm/drop_caches）。 */
  sync();
  (void)system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");

  phase = "writing_kernel";
  ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
  if (ota_mtd_stream_write(plan->pkg, OTA_KERNEL_OFF, plan->kernel_size, tboot->num, plan->upload) != 0)
    OTA_FAIL(plan->upload && plan->upload->error ? plan->upload->error : "kernel_write_failed");
  if (!plan->upload) {
    phase = "verifying_kernel";
    ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
    if (ota_mtd_stream_verify(plan->kernel_size, tname, plan->kernel_hash, 0) != 0)
      OTA_FAIL("kernel_readback_mismatch");
  }
  phase = "writing_rootfs";
  ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
  if (ota_mtd_stream_write(plan->pkg, OTA_KERNEL_OFF + plan->kernel_size,
                           plan->rootfs_size, tsys->num, plan->upload) != 0)
    OTA_FAIL(plan->upload && plan->upload->error ? plan->upload->error : "rootfs_write_failed");
  if (plan->upload && (plan->upload->read_bytes != ota_verified.length ||
      plan->upload->block_used != plan->upload->block_size || !ota_upload_connected(plan->upload)))
    OTA_FAIL("upload_cancelled");
  if (plan->upload) {
    phase = "verifying_kernel";
    ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
    if (ota_mtd_stream_verify(plan->kernel_size, tname, plan->kernel_hash, deadline) != 0)
      OTA_FAIL("kernel_readback_mismatch");
  }
  phase = "verifying_rootfs";
  ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
  if (ota_mtd_stream_verify(plan->rootfs_size, tsname, plan->rootfs_hash, deadline) != 0)
    OTA_FAIL("rootfs_readback_mismatch");
  if (plan->upload) {
    if (!ota_source_still_active(plan->source_slot, plan->source_slot[4] == '0' ? s0p->num : s1p->num))
      OTA_FAIL("source_slot_changed");
    if (!sess_ok(plan->upload->conn->ota_cookie)) OTA_FAIL("session_expired");
    if (!ota_upload_connected(plan->upload)) OTA_FAIL("upload_cancelled");
  }

  if (plan->wipe) {
    if (plan->upload) {
      ota_audio_stop();
      audio_stopped = 1;
      if (!ota_source_still_active(plan->source_slot, plan->source_slot[4] == '0' ? s0p->num : s1p->num))
        OTA_FAIL("source_slot_changed");
      if (!sess_ok(plan->upload->conn->ota_cookie)) OTA_FAIL("session_expired");
      if (!ota_upload_connected(plan->upload)) OTA_FAIL("upload_cancelled");
    }
    /* 清空共享 DATA（mtd6/ubifs）。2026-11-03 实测定案（rc25.7 首跑 wipe=1
       失败 data_ubiattach_failed）：/etc/init.d/boot 的 detach→erase→attach→
       mkvol 序列只在开机时成立——那时没有旧 UBI，attach 必成；运行中 umount
       常因 fd 被占（webd 日志 fd / miio 等）走 lazy 路径 → ubidetach 失败
       （旧实现忽略）→ 旧 ubi0 仍挂 → ubiattach EBUSY。分两层、各自独立生效：
       (1) 文件级清空（当前系统，恒成功）：删光 /data 内容并回验为空；
           配网/账号/录音立即消失，本次升级即生效（用户可见效果）。
           空 /data = 出厂首启状态，boot 脚本按首启逻辑重建标准文件。
       (2) 深度格式化（下次开机，/etc/init.d/boot 钩子）：写
           /data/.lx06-deepwipe（/data=ubifs，跨重启保留；2026-11-03 定案：
           /tmp 是 tmpfs，重启即丢，rc25.9 首启实测标记丢失钩子未触发），
           boot 脚本 stock
           挂载完成后见标记 → umount+ubidetach+flash_erase+ubiattach+
           ubimkvol+mount；该点无用户进程持 /data fd，detach 必成 →
           NAND 级出厂清空。深度格式化失败不影响开机（退回文件级已清状态）。
       绝对路径：webd 的 PATH 不含 /usr/sbin。失败即中止安装（boot 槽未动）。 */
    phase = "wiping_data";
    ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
    if (system("find /data -mindepth 1 -maxdepth 1 -exec rm -rf {} \\; 2>/dev/null; sync") != 0)
      OTA_FAIL("data_wipe_failed");
    /* find -delete 对个别项失败仍可能返回 0 → 回验为空才是权威判据 */
    if (system("find /data -mindepth 1 2>/dev/null | head -n 1 > /tmp/.lx06-wipecheck") != 0)
      OTA_FAIL("data_wipe_check_failed");
    {
      FILE *wf = fopen("/tmp/.lx06-wipecheck", "r");
      char wb[512];
      size_t wn = wf ? fread(wb, 1, sizeof(wb) - 1, wf) : 0;
      if (wf) fclose(wf);
      (void)unlink("/tmp/.lx06-wipecheck");
      if (wn > 0) OTA_FAIL("data_wipe_incomplete");
    }
    if (system("echo 1 > /data/.lx06-deepwipe 2>/dev/null") != 0)
      OTA_FAIL("deepwipe_mark_failed");
    /* 2026-11-03 定案（rc25.10 首启实测 + 本机 Test A/B）：ubifs 无周期日志
       提交——标记写入到 reboot -f 之间若无提交触发，标记只存在于 RAM 的
       TNC，reboot -f 直接丢失（boot 钩子不触发）。rebooting 阶段的 sync
       离标记写入约 100s 且落在 rootfs 写读高压窗（R1c：高压可吞 NAND 写）。
       故在 wipe 低气压窗立即 sync 强制提交，drop_caches 后 NAND 级回读
       验证（head -c 1 = "1" 才算数）；失败即中止安装（boot 槽未动）。 */
    if (system("sync 2>/dev/null") != 0)
      OTA_FAIL("deepwipe_mark_sync_failed");
    (void)system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");
    if (system("[ \"$(head -c 1 /data/.lx06-deepwipe 2>/dev/null)\" = \"1\" ]") != 0)
      OTA_FAIL("deepwipe_mark_verify_failed");
  }

  /* R1c 实测定案: 46MB tmpfs 升级包是 /tmp 压力源 — 高压窗下 fw_env -s 报成功
     但写未落盘 (NAND EIO 被厂商工具吞掉), 读回亦 EIO → slot_commit_unconfirmed;
     重启后 env 仍为旧值 (写未持久, 旧系统安全启动)。此处 kernel+rootfs 均已
     写入并读回校验通过, 包文件不再被使用 → unlink + sync + drop_caches 释放
     ~46MB 页缓存, 让 commit 写/读回在无压窗执行。 */
  if (!plan->upload) (void)unlink(plan->pkg);
  sync();
  (void)system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");
  sleep(1);
  if (plan->upload) {
    if (!ota_source_still_active(plan->source_slot, plan->source_slot[4] == '0' ? s0p->num : s1p->num))
      OTA_FAIL("source_slot_changed");
    if (!plan->wipe && !sess_ok(plan->upload->conn->ota_cookie)) OTA_FAIL("session_expired");
    if (!plan->wipe && !ota_upload_connected(plan->upload)) OTA_FAIL("upload_cancelled");
  }
  phase = "committing";
  ota_install_status("running", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
  {
    /* R1b: 46MB 高压窗下 fw_env -s 亦可 EIO/fork-ENOMEM → 3 次重试
       (尝试间 drop_caches); 下方读回校验不变。仍失败=中止:
       boot_part 未变, 旧系统照开, 重新上传即可重试 (写目标槽幂等), 无死路。 */
    int commit_ok = 0, attempt;
    for (attempt = 0; attempt < 3; attempt++) {
      if (ota_fw_env_set("boot_part", tname) == 0) { commit_ok = 1; break; }
      (void)system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");
      if (attempt < 2) sleep(2);
    }
    if (!commit_ok) OTA_FAIL("slot_commit_failed");
  }
  {
    char got[32] = "";
    if (ota_fw_env_get_retry("boot_part", got, sizeof(got)) != 0 || strcmp(got, tname) != 0)
      OTA_FAIL("slot_commit_unconfirmed");
  }
  phase = "rebooting";
  ota_install_status("succeeded", phase, plan->version, plan->source_slot, tname, plan->wipe, 0);
  if (plan->upload) {
    char response[320];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"install_accepted\":true,\"version\":\"%s\","
             "\"source\":\"%s\",\"target\":\"%s\",\"wipe\":\"%d\",\"source_boot_id\":\"%s\"}",
             plan->version, plan->source_slot, tname, plan->wipe, source_boot_id);
    send_json(upload_fd, response);
    close(upload_fd);
  }
  fprintf(stderr, "[LX06OTA] commit ok → rebooting to %s\n", tname);
  sync();
  sleep(2);
  execl("/sbin/reboot", "reboot", "-f", (char *)0);
  (void)system("reboot -f");
  _exit(1);

ota_worker_done:
  fprintf(stderr, "[LX06OTA] install failed phase=%s err=%s\n", phase, err);
  if (audio_stopped) ota_audio_start();
  ota_install_status("failed", phase, plan->version, plan->source_slot, tname, plan->wipe, err);
  if (plan->upload) {
    char response[96];
    int code = !strcmp(err, "session_expired") ? 401 :
               (!strcmp(err, "insufficient_memory") || !strcmp(err, "upload_unavailable")) ? 503 : 422;
    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", err);
    send_err(upload_fd, code, code == 401 ? "Unauthorized" :
             code == 503 ? "Service Unavailable" : "Unprocessable Content", response);
    close(upload_fd);
  } else unlink(plan->pkg);
  _exit(1);
#undef OTA_FAIL
}

/* rc25.6 预门禁：上传前无压窗完成全部环境检查（协议不变）。
 * R1c/R1d 实测定案：原门禁在收完 46MB body 后才跑——恰落在高压窗：
 *   ① nand_env 驱动每读需 256KB 连续页 (order:4)，页面碎片化 → EIO/空输出/
 *      错误文本 → 门禁误杀 (R1c 503 / R1d 422) 或 commit 读回假成功 (根因#3)；
 *   ② /proc/mtd 的 fopen 亦可在高压下失败 (boot_slot_unavailable)。
 * 驱动在无压窗自愈 (1-3 分钟, 6/6 探测恢复实测)，故把检查前移到上传前：
 * 既避免误杀，又提前拒绝 (422/409 在上传开始前返回, 不占 46MB tmpfs)。
 * manifest 依赖的尺寸校验留在 commit（纯内存比较, 无驱动读, 高压下安全）。
 * 锁存入 c->ota_lock_fd：其后任何 reject/失败路径由 conn_reset 关闭释放；
 * fork 的 worker 继承 fd, 安装全程持锁 (重启前不释放)。 */
static ota_mtd_part ota_pregate_parts[16];
static int ota_pregate_n = 0;
static char ota_pregate_cur[8] = "";
static char ota_pregate_board[16] = "";
static int ota_http_install_pregate(conn_t *c) {
  char board[16] = "", cur[32] = "";
  int n;

  if (ota_fw_env_get_retry("board_id", board, sizeof(board)) != 0) {
    if (!ota_boot_env_cache_valid)
      return ota_http_reject(c, 503, "Service Unavailable",
                             "{\"ok\":false,\"error\":\"board_id_unavailable\"}");
    /* 启动缓存兜底：board_id 开机后不变 */
    snprintf(board, sizeof(board), "%s", ota_boot_env_cache_board);
  }
  if (strcmp(board, "3") && strcmp(board, "5") && strcmp(board, "6") && strcmp(board, "10"))
    return ota_http_reject(c, 422, "Unprocessable Entity",
                           "{\"ok\":false,\"error\":\"device_model_unsupported\"}");
  n = ota_mtd_scan(ota_pregate_parts, 16);
  if (n <= 0)
    return ota_http_reject(c, 422, "Unprocessable Entity",
                           "{\"ok\":false,\"error\":\"boot_slot_unavailable\"}");
  if (ota_fw_env_get_retry("boot_part", cur, sizeof(cur)) != 0) {
    if (!ota_boot_env_cache_valid)
      return ota_http_reject(c, 503, "Service Unavailable",
                             "{\"ok\":false,\"error\":\"boot_part_unavailable\"}");
    snprintf(cur, sizeof(cur), "%s", ota_boot_env_cache_boot);
  }
  if (strcmp(cur, "boot0") && strcmp(cur, "boot1"))
    return ota_http_reject(c, 422, "Unprocessable Entity",
                           "{\"ok\":false,\"error\":\"boot_slot_unavailable\"}");
  if (!ota_mtd_find(ota_pregate_parts, n, "boot0") ||
      !ota_mtd_find(ota_pregate_parts, n, "boot1") ||
      !ota_mtd_find(ota_pregate_parts, n, "system0") ||
      !ota_mtd_find(ota_pregate_parts, n, "system1"))
    return ota_http_reject(c, 422, "Unprocessable Entity",
                           "{\"ok\":false,\"error\":\"slot_names_unavailable\"}");
  {
    int lockfd = open(OTA_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
      if (lockfd >= 0) close(lockfd);
      return ota_http_reject(c, 409, "Conflict",
                             "{\"ok\":false,\"error\":\"upgrade_in_progress\"}");
    }
    c->ota_lock_fd = lockfd;
  }
  ota_pregate_n = n;
  snprintf(ota_pregate_board, sizeof(ota_pregate_board), "%s", board);
  snprintf(ota_pregate_cur, sizeof(ota_pregate_cur), "%s", cur);
  return 0;
}

/* install-v1：manifest 校验通过后的安装决策 + fork（总是发出响应并关闭连接；
 * 每个拒绝路径立即 return，reject 时不产生 worker）。
 * rc25.6：board/mtd/boot_part/内存/互斥五项门禁已由上传前预门禁（无压窗）完成，
 * 此处复用其结果（不重读驱动），仅做 manifest 尺寸校验（内存）+ fork worker。 */
static void ota_http_install_commit(conn_t *c, const ota_manifest *m, int streamed) {
  static char plan_pkg[128], plan_ver[64], plan_kh[65], plan_rh[65], plan_src[8];
  struct ota_install_plan plan;
  struct ota_upload upload;
  const ota_mtd_part *parts = ota_pregate_parts;
  int n = ota_pregate_n;
  const char *cur = ota_pregate_cur;
  const ota_mtd_part *kb, *kbo, *s0p, *s1p;
  const char *tname;
  int lockfd;

  if (n <= 0 || (strcmp(cur, "boot0") && strcmp(cur, "boot1"))) {
    ota_http_reject(c, 422, "Unprocessable Entity",
                    "{\"ok\":false,\"error\":\"boot_slot_unavailable\"}");
    return;
  }
  kb = ota_mtd_find(parts, n, "boot0");
  kbo = ota_mtd_find(parts, n, "boot1");
  s0p = ota_mtd_find(parts, n, "system0");
  s1p = ota_mtd_find(parts, n, "system1");
  if (!kb || !kbo || !s0p || !s1p) {
    ota_http_reject(c, 422, "Unprocessable Entity",
                    "{\"ok\":false,\"error\":\"slot_names_unavailable\"}");
    return;
  }
  tname = cur[4] == '0' ? "boot1" : "boot0";
  {
    const ota_mtd_part *tboot = ota_mtd_find(parts, n, tname);
    const ota_mtd_part *tsys = ota_mtd_find(parts, n, cur[4] == '0' ? "system1" : "system0");
    if (!tboot || !tsys) {
      ota_http_reject(c, 422, "Unprocessable Entity",
                      "{\"ok\":false,\"error\":\"target_slot_unavailable\"}");
      return;
    }
    /* manifest 依赖的尺寸校验：纯内存比较, 无驱动读, 高压下安全 */
    if (kb->size != kbo->size || kb->size < 0x600000 ||
        m->kernel_size > tboot->size || m->rootfs_size > tsys->size) {
      ota_http_reject(c, 413, "Request Entity Too Large",
                      "{\"ok\":false,\"error\":\"image_too_large_for_slot\"}");
      return;
    }
  }
  /* 门禁与互斥已由预门禁通过；锁在 c->ota_lock_fd（上面各 reject 路径由
     conn_reset 自动释放），fork 失败路径同理由 conn_reset 释放, 不得手动 close */
  lockfd = c->ota_lock_fd;
  snprintf(plan_pkg, sizeof(plan_pkg), "%s", c->ota_file);
  snprintf(plan_ver, sizeof(plan_ver), "%s", m->version);
  snprintf(plan_kh, sizeof(plan_kh), "%s", m->kernel_hash);
  snprintf(plan_rh, sizeof(plan_rh), "%s", m->rootfs_hash);
  snprintf(plan_src, sizeof(plan_src), "%s", cur);
  plan.pkg = plan_pkg;
  plan.version = plan_ver;
  plan.kernel_size = m->kernel_size;
  plan.rootfs_size = m->rootfs_size;
  plan.kernel_hash = plan_kh;
  plan.rootfs_hash = plan_rh;
  plan.wipe = c->ota_wipe;
  plan.lock_fd = lockfd;
  plan.source_slot = plan_src;
  plan.upload = NULL;
  if (streamed) {
    memset(&upload, 0, sizeof(upload));
    upload.conn = c;
    upload.deadline = lk_now_ms() + 600000;
    plan.upload = &upload;
  } else {
    ota_install_status("running", "queued", plan.version, cur, tname, c->ota_wipe, 0);
  }
  {
    pid_t wp = fork();
    if (streamed && wp != 0) ota_verified.token[0] = 0;
    if (wp < 0) {
      /* 锁在 c->ota_lock_fd：reject → conn_close → conn_reset 自动释放 */
      ota_http_reject(c, 500, "Internal Server Error",
                      "{\"ok\":false,\"error\":\"fork_failed\"}");
      return;
    }
    if (wp == 0) {
      /* worker：不返回（_exit 或 reboot）；持锁 fd 随进程消亡而释放 */
      ota_install_worker(&plan);
    }
    if (streamed) {
      /* Worker owns the socket and the writer lock until it replies. */
      conn_close(c);
      return;
    }
    /* 父进程继续此处 */
    {
      char out[512];
      snprintf(out, sizeof(out),
               "{\"ok\":true,\"install_accepted\":true,\"version\":\"%s\","
               "\"source\":\"%s\",\"target\":\"%s\",\"wipe\":\"%d\"}",
               m->version, cur, tname, c->ota_wipe);
      c->ota_file_handed = 1;   /* 包文件归 worker 所有，conn_reset 不得删除 */
      close(lockfd);            /* 父进程放手，worker 保留其副本 */
      c->ota_lock_fd = -1;
      send_json(c->fd, out);
    }
    conn_close(c);
  }
}

/* --- GET /api/upgrade/status：槽位 + 运行版本 + DATA 空间 + 安装状态 --- */
static void ota_status_json_get(const char *raw, const char *key, char *out, size_t outsz) {
  char pat[40];
  const char *p, *v, *e;
  size_t l;
  out[0] = 0;
  if (outsz == 0) return;
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  p = strstr(raw, pat);
  if (!p) return;
  v = p + strlen(pat);
  while (*v == ' ' || *v == '\t') v++;
  if (*v == '"') {
    v++;
    e = strchr(v, '"');
    if (!e) return;
    l = (size_t)(e - v);
  } else {
    e = v;
    while (*e && *e != ',' && *e != '}') e++;
    l = (size_t)(e - v);
  }
  if (l >= outsz) l = outsz - 1;
  memcpy(out, v, l);
  out[l] = 0;
}
static void h_upgrade_status(int fd) {
  char boot[32] = "", run_version[64] = "", boot_id[37];
  char raw[1200] = "";
  char state[16] = "", phase[24] = "", st_ver[64] = "", source[8] = "",
       target[8] = "", st_err[48] = "", st_wipe[4] = "";
  char ota_json[512], out[1024];
  long ts = 0;
  long data_free = -1;
  struct statvfs sv;
  FILE *f;
  int off;

  f = fopen("/lx06-firmware-version", "r");
  if (f) {
    if (fgets(run_version, sizeof(run_version), f)) {
      size_t l = strlen(run_version);
      while (l && (run_version[l-1] == '\n' || run_version[l-1] == '\r'))
        run_version[--l] = 0;
    }
    fclose(f);
  }
  if (!run_version[0]) strcpy(run_version, "unknown");
  if (ota_parent_fw_env_get("boot_part", boot, sizeof(boot)) != 0 &&
      ota_boot_env_cache_valid)
    snprintf(boot, sizeof(boot), "%s", ota_boot_env_cache_boot);
  if (!boot[0]) strcpy(boot, "unknown");
  data_free = (statvfs("/data", &sv) == 0)
    ? (long)((unsigned long long)sv.f_bavail * (unsigned long)sv.f_bsize) : -1;
  f = fopen(OTA_STATUS_FILE, "r");
  if (f) {
    size_t r = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[r] = 0;
    ota_status_json_get(raw, "state", state, sizeof(state));
    ota_status_json_get(raw, "phase", phase, sizeof(phase));
    ota_status_json_get(raw, "version", st_ver, sizeof(st_ver));
    ota_status_json_get(raw, "source", source, sizeof(source));
    ota_status_json_get(raw, "target", target, sizeof(target));
    ota_status_json_get(raw, "wipe", st_wipe, sizeof(st_wipe));
    ota_status_json_get(raw, "error", st_err, sizeof(st_err));
    {
      char tsbuf[24] = "";
      ota_status_json_get(raw, "ts", tsbuf, sizeof(tsbuf));
      ts = (long)strtoll(tsbuf, 0, 10);
    }
  }
  if (!state[0]) state[0] = 'i', state[1] = 'd', state[2] = 'l', state[3] = 'e', state[4] = 0;
  if (strcmp(state, "running") == 0 && ts > 0 && (long)time(NULL) - ts > 1800) {
    strcpy(state, "failed");
    if (!st_err[0]) strcpy(st_err, "stale_worker");
  }
  off = snprintf(ota_json, sizeof(ota_json),
    "{\"state\":\"%s\",\"phase\":\"%s\",\"version\":\"%s\",\"source\":\"%s\","
    "\"target\":\"%s\",\"wipe\":%s,\"error\":",
    state, phase, st_ver, source, target, st_wipe[0] ? st_wipe : "0");
  if (st_err[0])
    off += snprintf(ota_json + off, sizeof(ota_json) - (size_t)off, "\"%s\"", st_err);
  else
    off += snprintf(ota_json + off, sizeof(ota_json) - (size_t)off, "null");
  snprintf(ota_json + off, sizeof(ota_json) - (size_t)off, ",\"ts\":%ld}", ts);
  ota_read_boot_id(boot_id);
  snprintf(out, sizeof(out),
           "{\"ok\":true,\"boot\":\"%s\",\"run_version\":\"%s\","
           "\"boot_id\":\"%s\",\"data_free\":%ld,\"ota\":%s}",
           boot, run_version, boot_id, data_free, ota_json);
  send_json(fd, out);
}

/* Returns 1 if this route has consumed/closed the connection. Called as soon
 * as headers arrive, BEFORE the ordinary full-body realloc. */
static int ota_http_begin(conn_t *c) {
  static const char request_v[] = "POST /api/upgrade/verify ";
  static const char request_i[] = "POST /api/upgrade/install ";
  int install = 0, streamed;
  char cookie[256], custom[64], type[128], origin[512], host[256],
       expected[512], site[64], wipe[8], receipt[64];
  char saved;
  LX06_STATBUF dst;
  struct statvfs sv;

  if (strncmp(c->buf, request_v, sizeof(request_v) - 1) == 0) {
    install = 0;
  } else if (strncmp(c->buf, request_i, sizeof(request_i) - 1) == 0) {
    install = 1;
  } else {
    return 0;
  }
  /* Never search untrusted binary payload bytes for HTTP header values. */
  saved = c->buf[c->head_end]; c->buf[c->head_end] = 0;
  hdr_get(c->buf, "Cookie", cookie, sizeof(cookie));
  hdr_get(c->buf, "X-LX06-Upgrade", custom, sizeof(custom));
  hdr_get(c->buf, "Content-Type", type, sizeof(type));
  hdr_get(c->buf, "Origin", origin, sizeof(origin));
  hdr_get(c->buf, "Host", host, sizeof(host));
  hdr_get(c->buf, "Sec-Fetch-Site", site, sizeof(site));
  hdr_get(c->buf, "X-LX06-Wipe", wipe, sizeof(wipe));
  hdr_get(c->buf, "X-LX06-Receipt", receipt, sizeof(receipt));
  c->buf[c->head_end] = saved;
  if (!sess_ok(cookie))
    return ota_http_reject(c, 401, "Unauthorized", "{\"ok\":false,\"error\":\"请先登录\"}");
  /* A custom header makes browser cross-origin uploads preflight; no CORS
   * permission is granted. Also reject foreign/null Origin explicitly. */
  {
    const char *want = install ? "install-v1" : "verify-v1";
    streamed = !strcmp(custom, install ? "install-v2" : "verify-v2");
    if (!streamed && strcmp(custom, want))
      return ota_http_reject(c, 403, "Forbidden", "{\"ok\":false,\"error\":\"请从本设备管理页面上传\"}");
    snprintf(expected, sizeof(expected), "http://%s", host);
    if (!*host || !strcmp(site, "cross-site") || (*origin && strcmp(origin, expected))) {
      snprintf(expected, sizeof(expected), "https://%s", host);
      if (!*host || !strcmp(site, "cross-site") || !*origin || strcmp(origin, expected))
        return ota_http_reject(c, 403, "Forbidden", "{\"ok\":false,\"error\":\"请从本设备管理页面上传\"}");
    }
  }
  if (strcmp(type, "application/octet-stream"))
    return ota_http_reject(c, 415, "Unsupported Media Type", "{\"ok\":false,\"error\":\"需要LX06升级包原始文件\"}");
  if (install && wipe[0]) {
    if (strcmp(wipe, "0") != 0 && strcmp(wipe, "1") != 0)
      return ota_http_reject(c, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad_wipe_flag\"}");
    c->ota_wipe = (wipe[0] == '1');
  }
  if (c->bodywant < 4096 + 192 || c->bodywant > OTA_BODY_MAX)
    return ota_http_reject(c, 413, "Too Large", "{\"ok\":false,\"error\":\"升级包长度不合法\"}");
  if (ota_active)
    return ota_http_reject(c, 409, "Conflict", "{\"ok\":false,\"error\":\"已有升级包正在上传\"}");
  if (install && streamed) {
    char session[sizeof(ota_verified.session)];
    if (!ota_verified.token[0] || strcmp(receipt, ota_verified.token) ||
        ota_verified.expires_ms <= lk_now_ms() || ota_verified.length != (size_t)c->bodywant ||
        sess_token_from_cookie(cookie, session, sizeof(session)) || strcmp(session, ota_verified.session))
      return ota_http_reject(c, 409, "Conflict", "{\"ok\":false,\"error\":\"receipt_invalid\"}");
    if (c->buflen - c->head_end > (size_t)c->bodywant)
      return ota_http_reject(c, 400, "Bad Request", "{\"ok\":false,\"error\":\"上传长度与请求不符\"}");
  }
  if (install && !ota_staging_memory_ok(streamed ? OTA_BLOCK_BYTES : (size_t)c->bodywant))
    return ota_http_reject(c, 503, "Service Unavailable",
                           "{\"ok\":false,\"error\":\"insufficient_memory\"}");
  /* rc25.6 预门禁：全部环境检查在上传前无压窗完成（拒绝即返回, 锁由
     conn_reset 自动释放）；manifest 尺寸校验留在 commit */
  if (install && ota_http_install_pregate(c))
    return 1;
  snprintf(c->ota_cookie, sizeof(c->ota_cookie), "%s", cookie);
  if (install && streamed) {
    ota_http_install_commit(c, &ota_verified.manifest, 1);
    return 1;
  }
  if (install) {
    static unsigned long upload_seq;
    if (statvfs("/tmp", &sv) != 0 ||
        (unsigned long long)sv.f_bavail * (unsigned long)sv.f_bsize <
            (unsigned long long)c->bodywant + 4ULL*1024*1024)
      return ota_http_reject(c, 507, "Insufficient Storage",
                             "{\"ok\":false,\"error\":\"staging_full\"}");
    if (mkdir(OTA_UPLOAD_DIR, 0700) != 0 && LX06_STAT(OTA_UPLOAD_DIR, &dst) != 0)
      return ota_http_reject(c, 507, "Insufficient Storage",
                             "{\"ok\":false,\"error\":\"staging_unavailable\"}");
    snprintf(c->ota_file, sizeof(c->ota_file),
             OTA_UPLOAD_DIR "/%d-%lu-%lu.lx06ota",
             (int)getpid(), (unsigned long)time(NULL), ++upload_seq);
    c->ota_fd = open(c->ota_file, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (c->ota_fd < 0) {
      c->ota_file[0] = 0;
      return ota_http_reject(c, 507, "Insufficient Storage",
                             "{\"ok\":false,\"error\":\"staging_unavailable\"}");
    }
  }
  c->ota = ota_stream_new();
  if (!c->ota) {
    c->ota_install = 0;
    return ota_http_reject(c, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"校验内存不足\"}");
  }
  c->ota_install = install;
  c->ota_prepare = streamed;
  c->last_act = time(NULL);
  ota_active = c;
  ota_http_data(c, c->buf + c->head_end, c->buflen - c->head_end);
  return 1;
}

/* P0.5 wake-crosstalk handlers. WT1 should leave this include in place. */
static int ww_clear_recent_diagnostics(void) {
  return lx06_log_append(AT_FDCWD, WWLOGF, WW_DIAGNOSTICS_CLEAR_LINE, sizeof(WW_DIAGNOSTICS_CLEAR_LINE) - 1);
}
#include "wake_p05_api.h"
#include "wake_collection_api.h"

/* --- 主循环 --- */
static void route(conn_t *c) {
  char *b = c->buf;
  char *body = b + c->head_end;
  char method[8] = "", path[512] = "", q[1024], ck[1024];
  char *sp1, *sp2, *qp;
  int authed, i;
  sp1 = b;
  for (i = 0; i < 7 && sp1[i] && sp1[i] != ' '; i++) method[i] = sp1[i];
  method[i < 7 ? i : 7] = 0;
  sp1 = strchr(b, ' ');
  if (!sp1) return;
  sp1++;
  sp2 = strchr(sp1, ' ');
  if (!sp2) return;
  if ((size_t)(sp2 - sp1) > 511) return;
  memcpy(path, sp1, (size_t)(sp2 - sp1));
  path[sp2 - sp1] = 0;
  /* 注意：不得 *sp2=0（会截断 buffer，使 hdr_get 的 strlen 在请求行就停止，
     从而丢失 Cookie 等头）。query 从 path 内 '?' 提取。 */
  q[0] = 0;
  qp = strchr(path, '?');
  if (qp) {
    size_t ql = strlen(qp + 1);
    if (ql > 1023) ql = 1023;
    *qp = 0;
    memcpy(q, qp + 1, ql);
    q[ql] = 0;
  }
  hdr_get(b, "Cookie", ck, sizeof(ck));
  authed = sess_ok(ck);
  /* 请求日志上下文（send_simple 统一落盘） */
  req_ctx_ip = c->cip;
  req_ctx_method = method;
  req_ctx_path = path;
  if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
    send_index(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/favicon.ico") == 0) {
    send_favicon(c->fd);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/login") == 0) {
    h_login(c->fd, body);
  } else if ((strcmp(method, "HEAD") == 0 || strcmp(method, "GET") == 0) &&
             strcmp(path, "/api/health") == 0) {
    /* R7 健康探测（前端 3s 轮询）：webd 活着即 200（空 body）。
       设备重启/断网 → 探测失败 → 前端把 WebAdmin 标红。 */
    send_simple(c->fd, 200, "OK", "text/plain", "", 0, NULL);
  } else if ((strcmp(method, "HEAD") == 0 || strcmp(method, "GET") == 0) &&
             strcmp(path, "/api/version") == 0) {
    /* V20261019.5：固件版本（免登录）。页脚据此动态显示 → 刷机成功可辨。 */
    h_version(c->fd);
  } else if (!authed) {
    send_err(c->fd, 401, "Unauthorized", "{\"ok\":false,\"error\":\"请先登录\"}");
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/logout") == 0) {
    {   /* 多会话：只删本会话令牌（按 cookie），不影响其他在线会话 */
      char mytok[128];
      if (sess_token_from_cookie(ck, mytok, sizeof(mytok)) == 0) sess_remove(mytok);
    }
    send_json(c->fd, "{\"ok\":true}");
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/autostart") == 0) {
    h_autostart(c->fd, NULL, 0);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/autostart") == 0) {
    h_autostart(c->fd, body, (size_t)c->bodywant);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
    h_status(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/system-stats") == 0) {
    h_systemstats(c->fd);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/password") == 0) {
    h_password(c->fd, body);
  } else if ((strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0) &&
             strcmp(path, "/api/logconf") == 0) {
    h_logconf(c->fd, body, (size_t)c->bodywant);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/reboot") == 0) {
    h_reboot(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/upgrade/status") == 0) {
    /* A/B 槽位 + 运行版本 + DATA 空间 + 安装状态（登录后） */
    h_upgrade_status(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/apps/extension") == 0) {
    h_extension_schema(c->fd, q, b);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/apps/extension/state") == 0) {
    h_extension_state(c->fd, q);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/extension/action") == 0) {
    h_extension_action(c->fd, b, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps") == 0) {
    h_apps_upload(c->fd, b, body, c->buflen - c->head_end, q);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/logs") == 0) {
    h_apps_log_feed(c->fd, body);
  } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/apps/log", 13) == 0) {
    h_apps_log(c->fd, q);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/run") == 0) {
    h_apps_run(c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/kill") == 0) {
    h_apps_kill(c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/uninstall") == 0) {
    h_apps_uninstall(c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/keeper/recover") == 0) {
    h_keeper_recover(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/logs") == 0) {
    h_logs(c->fd, q);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/logs/ctrl") == 0) {
    h_logctrl(c->fd, body);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/diag") == 0) {
    h_diag(c->fd, NULL);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/diag") == 0) {
    h_diag(c->fd, body);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/core") == 0) {
    h_core(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/syslog") == 0) {
    h_syslog(c->fd, q);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/crash") == 0) {
    h_crash(c->fd, q);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword") == 0) {
    h_ww_list(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword/history") == 0) {
    h_ww_history(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword/diagnostics") == 0) {
    ww_send_wake_log(c->fd, 1);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword/state") == 0) {
    h_ww_state(c->fd);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword/rec") == 0) {
    h_ww_rec_get(c->fd, q);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/add") == 0) {
    ww_modify(h_ww_add, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/rename") == 0) {
    ww_modify(h_ww_rename, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/del") == 0) {
    ww_modify(h_ww_del, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/update") == 0) {
    ww_modify(h_ww_update, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/record") == 0) {
    ww_modify(h_ww_record, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/retrain") == 0) {
    ww_modify(h_ww_retrain, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/rec-del") == 0) {
    ww_modify(h_ww_rec_del, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/play") == 0) {
    ww_modify(h_ww_rec_play, c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/service") == 0) {
    h_ww_service(c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/official") == 0) {
    h_ww_official(c->fd, body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/wakeword/official-bind") == 0) {
    h_ww_official_bind(c->fd, body);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword/samples") == 0) {
    wake_collection_get(c->fd, q, 0);
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/wakeword/samples/export") == 0) {
    wake_collection_get(c->fd, q, 1);
  } else if (wake_p05_try(method, path, c->fd, q, body)) {
    /* P0.5 forensics, cloud verify, pinyin warning, cross-test. */
  } else {
    send_err(c->fd, 404, "Not Found", "{\"ok\":false,\"error\":\"未找到\"}");
  }
}

/* Same idle boundary as the select loop; explicit time enables offline tests. */
static void conn_expire_idle(long now) {
  int i;
  for (i = 0; i < MAXCONN; i++)
    if (conns[i].fd >= 0 && now - conns[i].last_act > CONN_IDLE_TIMEOUT)
      conn_close(&conns[i]);
}

/* One readable socket step, shared by the select loop and offline socket tests.
 * Keep OTA interception before ordinary full-body allocation. */
static void conn_receive_ready(conn_t *c) {
  char rbuf[4096];
  ssize_t r;
  size_t space;
  req_ctx_ip = c->cip; req_ctx_method = req_ctx_path = "-";
  if (c->ota) {
    r = read(c->fd, rbuf, sizeof(rbuf));
    if (r < 0 && (errno == EINTR || errno == EAGAIN)) return;
    if (r <= 0) { conn_close(c); return; }
    c->last_act = time(NULL);
    ota_http_data(c, rbuf, (size_t)r);
    return;
  }
  /* 先预算/分配，再从 socket 取字节；绝不读后截断丢数据。 */
  if (!c->buf || c->buflen == c->bufcap) {
    size_t want = !c->buf ? 4096 : c->bufcap * 2;
    if (!c->have_head && want > MAXHDR) want = MAXHDR;
    if (want <= c->buflen) {
      send_err(c->fd, 431, "Request Header Fields Too Large", "{\"ok\":false,\"error\":\"请求头过长\"}");
      conn_close(c); return;
    }
    if (conn_grow(c, want) != 0) {
      send_err(c->fd, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"共享接收内存预算不足，请稍后重试\"}");
      conn_close(c); return;
    }
  }
  space = c->bufcap - c->buflen;
  r = read(c->fd, rbuf, space < sizeof(rbuf) ? space : sizeof(rbuf));
  if (r <= 0) { conn_close(c); return; }
  c->last_act = time(NULL);
  memcpy(c->buf + c->buflen, rbuf, (size_t)r);
  c->buflen += (size_t)r; c->buf[c->buflen] = 0;
  if (!c->have_head) {
    char *he = strstr(c->buf, "\r\n\r\n");
    size_t content_length, need;
    if (!he) {
      if (c->buflen >= MAXHDR) {
        send_err(c->fd, 431, "Request Header Fields Too Large", "{\"ok\":false,\"error\":\"请求头过长\"}");
        conn_close(c);
      }
      return;
    }
    if (http_content_length(c->buf, &content_length) != 0) {
      int code = errno == E2BIG ? 413 : 400;
      send_err(c->fd, code, code == 413 ? "Too Large" : "Bad Request", "{\"ok\":false,\"error\":\"Content-Length或请求边界不合法\"}");
      conn_close(c); return;
    }
    http_budget_init(); /* 按当前可用内存重测预算：避免开机低内存期一次性定死预算造成 413 假阳性 */
    c->head_end = (size_t)(he - c->buf) + 4;
    c->bodyorig = c->bodywant = (long)content_length;
    c->have_head = 1;
    if (ota_http_begin(c)) return;
    need = c->head_end + content_length;
    /* 统一累计 conns[].bufcap（含 NUL），不能给四个连接各一整份预算。 */
    if (need > c->bufcap && conn_grow(c, need) != 0) {
      int code = need + 1 > http_buffer_budget ? 413 : 503;
      send_err(c->fd, code, code == 413 ? "Too Large" : "Service Unavailable",
               "{\"ok\":false,\"error\":\"上传超过当前共享接收内存预算，未执行；请缩小文件或稍后重试\"}");
      conn_close(c); return;
    }
  }
  if (c->buflen < c->head_end + (size_t)c->bodywant) return;
  route(c);
  conn_close(c);
}

/* Read-only packaging diagnostic: use the shipped parser, never run scripts or
 * start the server. Counts only; no declaration values/credentials are printed. */
int webd_check_extension(const char *path) {
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC), rc;
  LX06_STATBUF st;
  struct ext_schema *s;
  if (fd < 0) { puts("{\"ok\":false,\"readable\":false}"); return 2; }
  if (LX06_FSTAT(fd, &st) || !S_ISREG(st.st_mode)) {
    close(fd); puts("{\"ok\":false,\"readable\":false}"); return 2;
  }
  s = calloc(1, sizeof(*s));
  if (!s) { close(fd); return 3; }
  rc = ext_read(fd, s);
  close(fd);
  printf("{\"ok\":%s,\"declared\":%s,\"fields\":%d,\"actions\":%d}\n",
         rc == 1 ? "true" : "false", rc != 0 ? "true" : "false",
         rc == 1 ? s->nf : 0, rc == 1 ? s->na : 0);
  free(s);
  return rc == 1 ? 0 : 1;
}

/* ---- 测试入口：WEBD_SELFTEST=1 时校验 MD5/MD5-crypt 后退出（qemu 冒烟用）---- */
#ifdef WEBD_SELFTEST
int main(void) {
  unsigned char d[16];
  char h1[128], h2[128];
  const char *v1 = "DB_COOPER";
  const char *v2 = "p@ss:w\"o\\rd\nx";
  md5_digest("abc", 3, d);
  printf("MD5(abc)=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
         d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7],d[8],d[9],d[10],d[11],d[12],d[13],d[14],d[15]);
  md5crypt(v1, "lx06def2", h1);
  printf("MD5CRYPT(DB_COOPER|lx06def2)=%s\n", h1);
  if (strcmp(h1, "$1$lx06def2$JIIifmrY3cYiEGmwCqs4E.") != 0) {
    printf("FAIL: DB_COOPER hash 不匹配（期望 $1$lx06def2$JIIifmrY3cYiEGmwCqs4E.）\n");
    return 1;
  }
  md5crypt(v2, "Ab3$xY9z", h2);
  printf("MD5CRYPT(pw2|Ab3$xY9z)=%s\n", h2);
  printf("SELFTEST_OK\n");
  return 0;
}
#else
/* ---- standalone 自管理（V23：web88 独立 procd 服务，不再是 dropbear 子进程）----
 *
 * 日志自管：procd 拉起时 fd1/2 指 /dev/null（老版 procd 的 stdout/stderr 参数
 * 是布尔），perror/启动日志全丢 → 启动时（非 TTY）重定向 fd1/2 到 WEBDLOG
 * （append，>64K 原地轮转 .1，与 unified 父进程版同语义）；conf log_webd=0
 * → 丢 /dev/null（日志开关=关，文件截回 0 保 inode）。前台 TTY 调试
 * （ssh 上直接跑 /usr/sbin/lx06webd）保留控制台输出，现场可见。
 * 幂等：unified 集成构建里父进程已重定向过 → 再重定向到同一文件，无副作用。
 *
 * 单实例护栏：flock /var/run/lx06webd.lock。并发 start / procd respawn 竞态
 * 时后到者等 4s 仍拿不到即退出（start_pre 已先清 stale 88 持有者；持锁者
 * 存活 = 88 已有服务，退出收敛）。fd 带 CLOEXEC：webd 派生的 app 子进程
 * 不继承（长驻 app 不死锁后继实例），webd 自身退出/被 KILL 内核自动放锁。 */
static int webd_log_conf_on(const char *key)
{
  char k[40], buf[2048], path[] = "/data/lx06-ssh.conf";
  int fd, on = 1;
  long got = 0, i;

  snprintf(k, sizeof(k), "%s=", key);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;
  while (got < (long)sizeof(buf) - 1) {
    ssize_t r = read(fd, buf + got, (size_t)(sizeof(buf) - 1 - got));
    if (r <= 0)
      break;
    got += r;
  }
  close(fd);
  buf[got] = 0;
  i = 0;
  while (i <= got) {
    long j, len;
    for (j = i; j < got; j++)
      if (buf[j] == '\n')
        break;
    len = j - i;
    if (len > (long)strlen(k) && strncmp(buf + i, k, strlen(k)) == 0) {
      if (buf[i + strlen(k)] == '0')
        on = 0;
    }
    if (j >= got)
      break;
    i = j + 1;
  }
  return on;
}

/* Keep Web88 responsive if the diagnostic sink is unavailable. */
static int webd_log_sink(const char *key)
{
  int p[2];
  pid_t child;
  if (access("/usr/bin/lx06-keeper", X_OK) || pipe(p)) return -1;
  child = fork();
  if (child == 0) {
    close(p[1]);
    if (dup2(p[0], STDIN_FILENO) < 0) _exit(126);
    if (p[0] != STDIN_FILENO) close(p[0]);
    /* Diagnostic processes must not keep Web sockets or the wake mutation
     * flock alive after the actual recording/calibration/playback worker exits. */
    for (int other = 3; other < 1024; ++other) close(other);
    execl("/usr/bin/lx06-keeper", "lx06-keeper", "log", key, (char *)NULL);
    _exit(127);
  }
  close(p[0]);
  if (child < 0) {
    close(p[1]);
    return -1;
  }
  /* Log pressure must not hold the Web request loop or its children. */
  int flags = fcntl(p[1], F_GETFL);
  if (flags >= 0) (void)fcntl(p[1], F_SETFL, flags | O_NONBLOCK);
  return p[1];
}

static void webd_self_log_redirect(void)
{
  char banner[96];
  int lf;

  if (isatty(STDOUT_FILENO) || isatty(STDERR_FILENO))
    return;   /* 前台调试：保留控制台 */
  mkdir("/data/lx06", 0755);
  mkdir(LOGDIR, 0755);
  mkdir(RUNLOGDIR, 0755);
  lf = webd_log_conf_on("log_webd") ? webd_log_sink("webd") : -1;
  if (lf < 0) lf = open("/dev/null", O_WRONLY);
  if (lf < 0)
    return;   /* 写不进去也不影响 webd 主功能 */
  signal(SIGPIPE, SIG_IGN);
  dup2(lf, 1);
  dup2(lf, 2);
  setvbuf(stderr, NULL, _IONBF, 0);
  setvbuf(stdout, NULL, _IONBF, 0);
  if (lf > 2)
    close(lf);
  if (!webd_log_conf_on("log_webd"))
    return;
#ifdef LX06_WEBD_INTEGRATED
  snprintf(banner, sizeof(banner),
      "webd(HTTP 88) 启动 pid=%d integrated time=%lu\n",
      (int)getpid(), (unsigned long)time(NULL));
#else
  snprintf(banner, sizeof(banner),
      "webd(HTTP 88) 启动 pid=%d standalone time=%lu\n",
      (int)getpid(), (unsigned long)time(NULL));
#endif
  (void)!write(STDOUT_FILENO, banner, strlen(banner));
}

#ifndef WEBD_INSTANCE_LOCK
#define WEBD_INSTANCE_LOCK "/var/run/lx06webd.lock"
#endif
static int webd_instance_lock_fd = -1;

/* 返回 1=持锁，0=他人在持（调用者应退出）。打不开锁文件不阻塞启动。 */
static int webd_instance_acquire(void)
{
  int fd, i, pn;
  char pb[16];

  fd = open(WEBD_INSTANCE_LOCK, O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    return 1;
  for (i = 0; i < 20; i++) {
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
      break;
    if (errno != EWOULDBLOCK && errno != EAGAIN)
      break;
    usleep(200 * 1000);
  }
  if (i >= 20 && errno == EWOULDBLOCK) {
    close(fd);
    return 0;
  }
  webd_instance_lock_fd = fd;
  {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl >= 0)
      fcntl(fd, F_SETFD, fl | FD_CLOEXEC);   /* app 子进程不继承（长驻 app 不放锁） */
  }
  pn = snprintf(pb, sizeof(pb), "%d", (int)getpid());
  (void)!ftruncate(fd, 0);
  (void)!lseek(fd, 0, SEEK_SET);
  (void)!write(fd, pb, (size_t)pn);
  return 1;
}

/* Isolated listen/one-iteration boundaries. webd_main uses these exact paths;
 * offline tests may replace maintenance callbacks, never the HTTP scheduler. */
typedef struct {
  void (*ext_reap_idle)(void);
  void (*deploy_watchdog)(void);
  void (*log_sweep)(void);
  void (*ww_event_poll)(void);
} webd_loop_callbacks;

static int webd_listen(const struct sockaddr_in *sa) {
  int lfd;
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) { perror("socket"); return -1; }
  {
    int fl = fcntl(lfd, F_GETFD, 0);
    if (fl >= 0) fcntl(lfd, F_SETFD, fl | FD_CLOEXEC);   /* 不泄漏给 system()/run_app 子进程 */
  }
  {
    int one = 1, fl;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    fl = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, fl | O_NONBLOCK);   /* accept 非阻塞，空队列立即 EAGAIN */
  }
  /* B3 修复（fix14）：EADDRINUSE 有限重试（旧 webd 未死透/连接 TIME_WAIT 尾巴）。
   * 看门狗侧已保证重拉前端口已空；这里是双保险，最多 20×250ms=5s。 */
  {
    int btry;
    for (btry = 0; btry < 20; btry++) {
      if (bind(lfd, (const struct sockaddr *)sa, sizeof(*sa)) == 0) break;
      if (errno != EADDRINUSE) { perror("bind"); return -1; }
      usleep(250000);
    }
  }
  if (bind(lfd, (const struct sockaddr *)sa, sizeof(*sa)) < 0 &&
      errno == EADDRINUSE) { perror("bind"); return -1; }
  if (listen(lfd, 8) < 0) { perror("listen"); return -1; }
  return lfd;
}

static int wake_route_notify_fd = -1;

static void webd_watch_wake_routes(void) {
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0) return;
  /* Local paired and official-winner mailboxes are published by atomic rename.
   * The existing consumers still own all age, origin and content checks. */
  if (fd >= FD_SETSIZE || inotify_add_watch(fd, "/tmp", IN_MOVED_TO | IN_ONLYDIR) < 0) {
    close(fd);
    return;
  }
  wake_route_notify_fd = fd;
}

static void webd_loop_step(int lfd, const webd_loop_callbacks *callbacks) {
  int i, n;
  fd_set rf;
  struct sockaddr_in ca;
  socklen_t cl;
  conn_t *c;
  /* One bounded sweep shared by all extension actions; no per-script timer. */
  static int64_t ext_reap_next;
  int64_t ext_now=lk_now_ms();
  noise_tick(ext_now);
  if(ext_now>=ext_reap_next){ext_reap_next=ext_now+5000;callbacks->ext_reap_idle();}
  FD_ZERO(&rf);
  FD_SET(lfd, &rf);
  n = lfd;
  if (noise_probe.fd >= 0) {
    FD_SET(noise_probe.fd, &rf);
    if (noise_probe.fd > n) n = noise_probe.fd;
  }
  for (i = 0; i < MAXCONN; i++)
    if (conns[i].fd >= 0) {
      FD_SET(conns[i].fd, &rf);          /* 连接 socket 也要进读集合 */
      if (conns[i].fd > n) n = conns[i].fd;
    }
  if (wake_route_notify_fd >= 0) {
    FD_SET(wake_route_notify_fd, &rf);
    if (wake_route_notify_fd > n) n = wake_route_notify_fd;
  }
  {
    /* Keep polling for legacy producers and pending session handoffs. */
    struct timeval tv = {0, 100000};
    /* On EINTR/error the ready set is unspecified (often still the input
     * set). Never read a blocking accepted socket on that stale readiness.
     * Keep expiry and maintenance/audio dispatch running on this iteration. */
    if (select(n + 1, &rf, NULL, NULL, &tv) < 0) FD_ZERO(&rf);
  }
  if (wake_route_notify_fd >= 0 && FD_ISSET(wake_route_notify_fd, &rf)) {
    char notifications[4096];
    ssize_t received = read(wake_route_notify_fd, notifications, sizeof(notifications));
    if (received < 0 && errno != EINTR && errno != EAGAIN) {
      close(wake_route_notify_fd);
      wake_route_notify_fd = -1;
    }
  }
  if (FD_ISSET(lfd, &rf)) {
    for (i = 0; i < MAXCONN; i++) {
      for (;;) {
        int cfd;
        if (conns[i].fd >= 0) break;
        cl = sizeof(ca);
        cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) break;              /* 无更多 pending，EAGAIN */
        conn_reset(&conns[i]);
        conns[i].fd = cfd;
        conns[i].last_act = time(NULL);
        inet_ntop(AF_INET, &ca.sin_addr, conns[i].cip, sizeof(conns[i].cip));
        {
          int two = 1;
          setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &two, sizeof(two));
        }
        break;                            /* 每槽本轮只放一个，其余下轮 */
      }
    }
  }
  /* 空闲超时回收：对端 RST/断网后 read 未必立即返回（尤其 LAN 内丢包无
     keepalive 时报错），死连接会永久占住 4 槽之一 → accept 无空槽 → 88 全挂
     （「登录响应异常、要刷新才进」的根因）。仅按空闲计时，活动连接不受影响。 */
  conn_expire_idle(time(NULL));
  { /* Maintenance keeps its 1s cadence even when an audio event wakes select. */
    static time_t maintenance_seen;
    time_t now = time(NULL);
    if (now != maintenance_seen) {
      maintenance_seen = now;
      callbacks->deploy_watchdog();
      callbacks->log_sweep();
    }
  }
  callbacks->ww_event_poll();      /* 唤醒技能：本地唤醒词事件 → 直通关联脚本/官方唤醒 */
  for (i = 0; i < MAXCONN; i++) {
    c = &conns[i];
    if (c->fd < 0) continue;
    if (!FD_ISSET(c->fd, &rf)) continue;
    conn_receive_ready(c);
  }
  /* Maintain one clock continuity history in the original global loop.
   * No schedule/config/idle authority is inferred from this observation. */
  webd_system_clock_poll();
}

/* 独立进程入口：由 procd（S65 lx06webd）直接 exec 运行。 */
int webd_main(void) {
  int lfd, i;
  struct sockaddr_in sa;
  static const webd_loop_callbacks callbacks = {
    ext_reap_idle, deploy_watchdog, log_sweep, ww_poll_routes
  };
  webd_self_log_redirect();
  if (!webd_instance_acquire()) {
    perror("webd: instance lock held by another process, exiting");
    return 1;
  }
  signal(SIGCHLD, SIG_IGN);      /* 脚本子进程自动回收 */
  signal(SIGPIPE, SIG_IGN);
  webd_watch_wake_routes();
  http_budget_init();
  ota_boot_env_cache_init();   /* R1b: 高压窗兜底缓存 (尽力而为, 不阻塞起服) */
  for (i = 0; i < MAXCONN; i++) {
    conns[i].fd = -1;             /* 静态数组零初始化 fd=0，须显式置 -1 */
    conns[i].ota_fd = -1;
    conns[i].ota_lock_fd = -1;
  }
  {
    const char *dirs[] = {"/data/lx06", APPDIR, APPPERSIST, APPLOGDIR, BKDIR, LOGDIR, RUNLOGDIR};
    const mode_t modes[] = {0755, 0700, 0755, 0755, 0700, 0755, 0755};
    unsigned di;
    for (di = 0; di < sizeof(dirs) / sizeof(dirs[di]); di++) {
      int df = app_trusted_dir(dirs[di], modes[di], 1);
      if (df < 0) { perror("unsafe management directory"); return 1; }
      close(df);
    }
  }
  /* 常驻名单自愈：规范化 /data/lx06/keeper/resident（旧版 NUL 空洞文件）
     并把托管块同步进 /data/init.sh（刷机后旧模板无块的断档修复）。
     尽力而为：失败不阻塞 webd 起服，下次 resident_sync 再试。 */
  resident_heal_boot();
  {
    FILE *tf;
    /* fix15：日志是页面查看对象 → 只在「fix15 首次启动」清一次旧版残留日志
       （旧逻辑每次启动都删 webd-req.log / 截断 ssh-auth.log，会把用户刚要看的
       现场清掉）。flag 文件在 /data（重启保留），之后重拉（看门狗/开关重启）
       不再清。ssh-auth.log 的 fd 被 dbwrap 持有（dropbear 父进程）→ 截断为 0
       （删除会让 fd 指向已删 inode，dbwrap 到下次 dropbear 重启才重建）。 */
    if (access("/data/lx06/.logv15", F_OK) != 0) {
      remove(REQLOG);
      remove(REQLOG ".1");
      tf = fopen(SSHLOG, "w");
      if (tf) fclose(tf);
      tf = fopen(SSHLOG ".1", "w");
      if (tf) fclose(tf);
      tf = fopen("/data/lx06/.logv15", "w");
      if (tf) {
        fputs("1\n", tf);
        fclose(tf);
      }
    }
  }
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  sa.sin_port = htons(PORT);
  lfd = webd_listen(&sa);
  if (lfd < 0) return 1;
  /* listening 行 = webd 就绪的权威证据（看门狗/排障判 88 是否真正起服）。 */
  {
    char lline[96];
    int ln = snprintf(lline, sizeof(lline),
        "webd(HTTP 88) listening pid=%d time=%lu\n",
        (int)getpid(), (unsigned long)time(NULL));
    if (ln > 0)
      (void)!write(STDOUT_FILENO, lline, (size_t)ln);
  }
  /* 原生桥实时读取用户开关；缺文件默认开，启动不写 DATA 默认值。 */
  {
    int on0 = ww_official_read();
    ww_official_apply(on0, 0);
  }
  for (;;) {
    webd_loop_step(lfd, &callbacks);
  }
  return 0;
}
#endif   /* WEBD_SELFTEST */

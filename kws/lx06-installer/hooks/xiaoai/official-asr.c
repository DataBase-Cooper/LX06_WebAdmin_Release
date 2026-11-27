/* official-asr — 官方问句的 TTS 登记。
 *
 * speech 帧送到 LAB2 之后，mico 会自己生成对话号并拿到云端回答，
 * 但不会写 ai service record。播放器查不到号就 keep slient。
 * common.usock 不能收 speech 帧，原样转发会 unpack failed。
 *
 * 登记走 mibrain ai_service。看到 instruction.log 里的最终识别结果，
 * 且当前路由是官方，就用识别文本再走一次这条已经对上的路径。
 * 原来那条未登记的对话仍然静音，用户只听到这一次。
 *
 * 四字开关关掉时不代答。本地词链官方会留下 /tmp/lx06-official-wake，
 * 开关关掉但这个标记还新，仍然代答。脚本要求静音时不代答。
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __arm__
#include "glibc_ni_trap.h"
#endif

#define INSTR "/tmp/mico_aivs_lab/instruction.log"
#define ROUTE "/tmp/xiaoai-agent/route"
#define RDEF  "/tmp/xiaoai-agent/route.default"
#define SWITCH "/data/wakeword/official_xiaoai"
#define STAMP "/tmp/lx06-official-wake"
#define MUTE  "/tmp/xiaoai-agent/official-reply.mute"
#define NOPLAY "/tmp/xiaoai-agent/db_noplay"
#define TTSDIR "/tmp/tts"
#define SYSL   "/var/log/messages"
#define ALOG  "/tmp/official-asr.log"

static void note(const char *s)
{
    int fd = open(ALOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    write(fd, s, strlen(s));
    write(fd, "\n", 1);
    close(fd);
}

static int read_token(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY);
    ssize_t k;
    size_t i;
    if (fd < 0 || n < 2)
        return 0;
    k = read(fd, out, n - 1);
    close(fd);
    if (k <= 0)
        return 0;
    out[k] = 0;
    for (i = 0; i < (size_t)k; i++) {
        if (out[i] == '\n' || out[i] == '\r' || out[i] == ' ') {
            out[i] = 0;
            break;
        }
    }
    return out[0] != 0;
}

static int file_is_official(const char *path, int *present)
{
    char tok[32];
    *present = 0;
    if (!read_token(path, tok, sizeof tok))
        return 0;
    *present = 1;
    return strcmp(tok, "official") == 0;
}

static int switch_off(void)
{
    char tok[8];
    if (!read_token(SWITCH, tok, sizeof tok))
        return 0;
    return tok[0] == '0' && tok[1] == 0;
}

static int stamp_fresh(void)
{
    struct stat st;
    if (stat(STAMP, &st) != 0)
        return 0;
    return time(NULL) - st.st_mtime <= 12;
}

static int may_answer(void)
{
    int present = 0;
    struct stat nst;
    if (access(MUTE, F_OK) == 0)
        return 0;
    /* DB_Noplay（门控脚本等）生效中 → 官方回复会被静音，代答也是多余 */
    if (stat(NOPLAY, &nst) == 0 && time(NULL) - nst.st_mtime < 120)
        return 0;
    if (file_is_official(ROUTE, &present)) {
        /* temporary official route */
    } else if (present) {
        return 0;
    } else if (file_is_official(RDEF, &present)) {
        /* default official */
    } else if (present) {
        return 0;
    }
    if (switch_off() && !stamp_fresh())
        return 0;
    return 1;
}

static int hex4(const char *s, unsigned *v)
{
    unsigned n = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        n <<= 4;
        if (c >= '0' && c <= '9')
            n |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            n |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            n |= (unsigned)(c - 'A' + 10);
        else
            return 0;
    }
    *v = n;
    return 1;
}

static int utf8_put(char *out, size_t outn, size_t *o, unsigned cp)
{
    if (cp < 0x80) {
        if (*o + 1 >= outn)
            return 0;
        out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*o + 2 >= outn)
            return 0;
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (*o + 3 >= outn)
            return 0;
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        return 0;
    }
    return 1;
}

static int json_unescape(const char *s, const char *end, char *out, size_t outn)
{
    size_t o = 0;
    while (s < end && *s != '"') {
        unsigned cp;
        if (*s == '\\') {
            if (s + 1 >= end)
                return 0;
            s++;
            if (*s == 'u') {
                if (s + 5 > end || !hex4(s + 1, &cp))
                    return 0;
                s += 5;
                if (!utf8_put(out, outn, &o, cp))
                    return 0;
                continue;
            }
            if (*s == '"' || *s == '\\' || *s == '/')
                cp = (unsigned char)*s;
            else if (*s == 'n')
                cp = '\n';
            else if (*s == 'r')
                cp = '\r';
            else if (*s == 't')
                cp = '\t';
            else
                return 0;
            s++;
            if (!utf8_put(out, outn, &o, cp))
                return 0;
            continue;
        }
        if ((unsigned char)*s < 0x20)
            return 0;
        if (o + 1 >= outn)
            return 0;
        out[o++] = *s++;
    }
    if (s >= end || *s != '"')
        return 0;
    out[o] = 0;
    return o > 0 && o < 160;
}

static const char *find_key(const char *s, const char *end, const char *key)
{
    size_t n = strlen(key);
    const char *p = s;
    while (p + n <= end) {
        if (memcmp(p, key, n) == 0)
            return p;
        p++;
    }
    return NULL;
}

/* 1 = text written, 0 = skip (partial or not a query), -1 = need more bytes. */
static int extract_query(const char *win, size_t n, char *out, size_t outn)
{
    const char *end = win + n;
    const char *fin = find_key(win, end, "\"is_final\"");
    const char *t;
    if (fin) {
        const char *v = fin + 10;
        while (v < end && (*v == ' ' || *v == ':' || *v == '\t'))
            v++;
        if (v + 5 <= end && memcmp(v, "false", 5) == 0)
            return 0;
        if (!(v + 4 <= end && memcmp(v, "true", 4) == 0))
            return -1;
    }
    t = find_key(win, end, "\"text\"");
    if (!t)
        return -1;
    t += 6;
    while (t < end && (*t == ' ' || *t == ':' || *t == '\t'))
        t++;
    if (t >= end || *t != '"')
        return -1;
    if (!json_unescape(t + 1, end, out, outn)) {
        if (t + 1 < end && memchr(t + 1, '"', (size_t)(end - (t + 1))))
            return 0;
        return -1;
    }
    return 1;
}

static int json_escape(const char *in, char *out, size_t outn)
{
    size_t o = 0;
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            if (o + 2 >= outn)
                return 0;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            return 0;
        } else {
            if (o + 1 >= outn)
                return 0;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return 1;
}

static char last_text[160];
static time_t last_when;

/* ---------- 问题1修复：voip filter 卡死时 TTS 流被拒（keep slient），
   但 tts_save:1 已把合成文件落盘 /tmp/tts。文件播放不受 flag 影响
   （16:08 真机实证）。代答后：等落盘 → 从 syslog 判定是否被拒 →
   被拒则播文件 + 事件 8 释放 filter；正常流则只清理文件。 ---------- */

static void syslog_mark(off_t *off)
{
    struct stat st;
    if (stat(SYSL, &st) == 0)
        *off = st.st_size;
}

/* 统计 off 之后新增 syslog 中 TTS 流被拒次数（"rejected by voip filter!"
   或 "keep slient"）。轮转后偏移越界 → 从头读新文件（最多 64KB，
   轻微多计只影响「是否播文件」，不影响释放正确性）。 */
static int syslog_rejects(off_t *off)
{
    int fd, rej = 0;
    char buf[8192];
    struct stat st;
    off_t start = 0, len, k;
    if (stat(SYSL, &st) != 0)
        return 0;
    if (*off <= (off_t)st.st_size)
        start = *off;
    len = (off_t)st.st_size - start;
    if (len > 65536) {
        start = (off_t)st.st_size - 65536;
        len = 65536;
    }
    *off = (off_t)st.st_size;
    if (len <= 0)
        return 0;
    fd = open(SYSL, O_RDONLY);
    if (fd < 0)
        return 0;
    lseek(fd, start, SEEK_SET);
    while ((k = read(fd, buf, sizeof buf)) > 0) {
        char *p;
        size_t n = (size_t)k;
        for (p = buf; p && p + 1 < buf + n; p++) {
            if (!strncmp(p, "rejected by voip filter", 23)) {
                rej++;
                p += 23;
            } else if (!strncmp(p, "keep slient", 11))
                rej++, p += 11;
        }
    }
    close(fd);
    return rej;
}

/* t0 后落盘的最新 tts_*.mp3（tts_save:1 产物；FIFO 跳过）。 */
static int saved_tts(time_t t0, char *out, size_t outn)
{
    DIR *d;
    struct dirent *de;
    struct stat st;
    time_t best = t0 - 2;
    int have = 0;
    d = opendir(TTSDIR);
    if (!d)
        return 0;
    while ((de = readdir(d))) {
        char p[300];
        if (de->d_name[0] != 't')
            continue;
        snprintf(p, sizeof p, "%s/%s", TTSDIR, de->d_name);
        if (stat(p, &st) != 0 || S_ISFIFO(st.st_mode))
            continue;
        if (st.st_mtime >= best) {
            best = st.st_mtime;
            snprintf(out, outn, "%s", p);
            have = 1;
        }
    }
    closedir(d);
    return have;
}

static void run_ubus(const char *svc, const char *mth, const char *arg)
{
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/ubus", "ubus", "-t", "20", "call", svc, mth, arg,
              (char *)NULL);
        _exit(127);
    }
    if (p > 0) {
        int st;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR)
            ;
    }
}

static void play_file(const char *file)
{
    char arg[340];
    snprintf(arg, sizeof arg, "{\"url\":\"file://%s\",\"type\":1}", file);
    run_ubus("mediaplayer", "player_play_url", arg);
}

static void release_voip(void)
{
    run_ubus("pnshelper", "event_notify", "{\"src\":3,\"event\":8}");
    note("voip released (event 8)");
}

static void answer(const char *text)
{
    char esc[320];
    char json[512];
    char line[200];
    pid_t pid;
    time_t now = time(NULL);
    if (!strcmp(text, "小爱同学"))
        return;
    if (!strcmp(text, last_text) && now - last_when < 8)
        return;
    if (!may_answer()) {
        note("skip gate");
        return;
    }
    if (!json_escape(text, esc, sizeof esc))
        return;
    snprintf(json, sizeof json,
             "{\"nlp_text\":\"%s\",\"tts\":1,\"nlp\":1,\"nlp_execute\":1,"
             "\"caller\":\"wakeup\",\"tts_play\":1,\"tts_save\":1}",
             esc);
    snprintf(line, sizeof line, "answer %s", text);
    note(line);
    memcpy(last_text, text, strlen(text) + 1);
    last_when = now;
    {
        off_t off;
        int rej, waited;
        char file[300] = "";
        syslog_mark(&off);
        pid = fork();
        if (pid < 0)
            return;
        if (pid == 0) {
            execl("/bin/ubus", "ubus", "-t", "8", "call", "mibrain",
                  "ai_service", json, (char *)NULL);
            _exit(127);
        }
        waited = 0;
        while (waited < 15) {
            sleep(1);
            waited++;
            if (saved_tts(last_when, file, sizeof file))
                break;
        }
        if (file[0])
            sleep(2); /* 等 syslog 拒绝标记落盘 */
        rej = syslog_rejects(&off);
        if (file[0]) {
            if (rej) {
                note("stream rejected -> play saved file");
                play_file(file);
                release_voip();
            }
            unlink(file);
        } else if (rej) {
            note("rejected, no saved file -> release only");
            release_voip();
        }
    }
}

static void scan(char *buf, size_t *len)
{
    size_t i = 0;
    const char *mark = "\"name\":\"RecognizeResult\"";
    const char *mark2 = "\"name\": \"RecognizeResult\"";
    while (i + 24 < *len) {
        const char *hit = NULL;
        size_t hit_at = 0;
        size_t j;
        for (j = i; j + 24 < *len; j++) {
            if (buf[j] == '"' && j + 24 < *len &&
                memcmp(buf + j, mark, 24) == 0) {
                hit = buf + j;
                hit_at = j;
                break;
            }
            if (buf[j] == '"' && j + 26 < *len &&
                memcmp(buf + j, mark2, 26) == 0) {
                hit = buf + j;
                hit_at = j;
                break;
            }
        }
        if (!hit)
            break;
        {
            size_t avail = *len - hit_at;
            size_t win = avail < 1600 ? avail : 1600;
            char text[160];
            int got = extract_query(hit, win, text, sizeof text);
            note("hit");
            if (got < 0 && avail < 1600) {
                memmove(buf, buf + hit_at, avail);
                *len = avail;
                return;
            }
            if (got)
                answer(text);
        }
        i = hit_at + 26;
    }
    if (*len > 2048) {
        memmove(buf, buf + *len - 512, 512);
        *len = 512;
    }
}

#ifdef OFFICIAL_ASR_FILETEST
int main(int argc, char **argv)
{
    char buf[8192];
    size_t len = 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 9;
    len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    scan(buf, &len);
    return 0;
}
#elif defined OFFICIAL_ASR_TEST
int main(void)
{
    char out[160];
    const char *ok =
        "{\"header\":{\"name\":\"RecognizeResult\"},"
        "\"payload\":{\"is_final\":true,\"results\":[{\"text\":\"\\u51e0\\u70b9\\u4e86\"}]}}";
    const char *partial =
        "{\"header\":{\"name\":\"RecognizeResult\"},"
        "\"payload\":{\"is_final\":false,\"results\":[{\"text\":\"几\"}]}}";
    const char *plain =
        "{\"header\":{\"name\":\"RecognizeResult\"},\"payload\":{\"text\":\"几点了\"}}";
    if (!extract_query(ok, strlen(ok), out, sizeof out) || strcmp(out, "几点了"))
        return 1;
    if (extract_query(partial, strlen(partial), out, sizeof out) > 0)
        return 2;
    if (extract_query(plain, strlen(plain), out, sizeof out) != 1 || strcmp(out, "几点了"))
        return 3;
    return 0;
}
#else
int main(void)
{
    char buf[8192];
    size_t len = 0;
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    note("official-asr up");
    for (;;) {
        int fd;
        struct stat st;
        off_t pos;
        fd = open(INSTR, O_RDONLY);
        if (fd < 0) {
            sleep(1);
            continue;
        }
        if (fstat(fd, &st) == 0)
            lseek(fd, st.st_size, SEEK_SET);
        pos = lseek(fd, 0, SEEK_CUR);
        for (;;) {
            ssize_t k;
            struct stat now;
            if (fstat(fd, &now) == 0 && now.st_size < pos) {
                close(fd);
                len = 0;
                break;
            }
            k = read(fd, buf + len, sizeof buf - len - 1);
            if (k > 0) {
                len += (size_t)k;
                buf[len] = 0;
                pos += k;
                scan(buf, &len);
            } else {
                usleep(200000);
            }
            if (len + 64 >= sizeof buf) {
                memmove(buf, buf + len - 512, 512);
                len = 512;
            }
        }
    }
}
#endif

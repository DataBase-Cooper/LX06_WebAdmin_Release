#define _GNU_SOURCE
#include "account.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ACCOUNT_RETIRED_CAP 8u
#define ACCOUNT_READ_BUDGET 4096u
#define ACCOUNT_TIMEOUT_MS UINT64_C(180000)
#define ACCOUNT_CANCEL_GRACE_MS UINT64_C(80)
/* New cross headers redirect plain stat to post-2.25 symbols. Match the
 * existing firmware's explicit ARM large-file ABI; never change its libc. */
#if defined(__arm__) && __SIZEOF_POINTER__ == 4
extern int __lxstat64(int, const char *, struct stat64 *);
extern int __fxstat64(int, int, struct stat64 *);
#define ACCOUNT_STATBUF struct stat64
#define ACCOUNT_LSTAT(path, out) __lxstat64(3, (path), (out))
#define ACCOUNT_FSTAT(fd, out) __fxstat64(3, (fd), (out))
#else
#define ACCOUNT_STATBUF struct stat
#define ACCOUNT_LSTAT(path, out) lstat((path), (out))
#define ACCOUNT_FSTAT(fd, out) fstat((fd), (out))
#endif
struct account_pair { char question[2048], answer[3073]; };
struct account_retired { pid_t pid; uint64_t kill_at; int killed; };
int dc_account_parse_declaration(const void *bytes, size_t length) {
    static const char identity[] = "# LX06-ACCOUNT-IDENTITY: v1";
    static const char keyword[] = "# LX06-INTERRUPTION: keyword";
    static const char voice[] = "# LX06-INTERRUPTION: voice";
    static const char iprefix[] = "# LX06-ACCOUNT-IDENTITY";
    static const char mprefix[] = "# LX06-INTERRUPTION";
    const unsigned char *p = bytes;
    if (!p || !length || length > DC_ACCOUNT_DECLARATION_INPUT_CAP || memchr(p, 0, length)) return -1;
    size_t at = 0; unsigned line = 0, ident = 0, mode = 0; int shebang = 0;
    while (at < length) {
        size_t begin = at;
        while (at < length && p[at] != '\n') ++at;
        size_t n = at - begin; int terminated = at < length;
        if (terminated) ++at;
        ++line;
        if (line == 1 && terminated && n == 9 && !memcmp(p + begin, "#!/bin/sh", 9)) {
            shebang = 1; continue;
        }
        /* Header is the initial run of blank/comment lines. Conservatively
         * recognize high bytes as possible invisible prefixes, without locale
         * or Unicode normalization; no executable shell text is scanned. */
        size_t q = begin;
        while (q < begin + n && (p[q] == ' ' || (p[q] >= 9 && p[q] <= 13) || p[q] >= 128)) ++q;
        if (q == begin + n) continue;
        if (p[q] != '#') break;
        size_t tag = ++q;
        while (tag < begin + n && (p[tag] == ' ' || (p[tag] >= 9 && p[tag] <= 13) || p[tag] >= 128)) ++tag;
        size_t remaining = begin + n - tag;
        int ilike = remaining >= sizeof iprefix - 3 && !memcmp(p + tag, iprefix + 2, sizeof iprefix - 3);
        int mlike = remaining >= sizeof mprefix - 3 && !memcmp(p + tag, mprefix + 2, sizeof mprefix - 3);
        if (ilike && (q != begin + 1 || tag != begin + 2 || p[begin + 1] != ' ')) return -1;
        if (mlike && (q != begin + 1 || tag != begin + 2 || p[begin + 1] != ' ')) return -1;
        if (ilike) {
            if (line != 2 || !shebang || !terminated || n != sizeof identity - 1 ||
                memcmp(p + begin, identity, n) || ident) return -1;
            ident = 1;
        } else if (mlike) {
            if (line != 3 || !ident || !terminated || mode) return -1;
            if (n == sizeof keyword - 1 && !memcmp(p + begin, keyword, n)) mode = DC_ACCOUNT_KEYWORD;
            else if (n == sizeof voice - 1 && !memcmp(p + begin, voice, n)) mode = DC_ACCOUNT_VOICE;
            else return -1;
        }
    }
    return ident == (mode != 0) ? (int)mode : -1;
}
struct dc_account {
    dc_tag tag;
    int sealed_fd, sealed_requested;
    uint64_t sealed_epoch;
    pid_t pid;
    int output_fd, input_fd, eof, protocol_done, successful, invalidated;
    uint64_t deadline, first_delta, pending_since, next_sequence;
    size_t input_at, input_len, line_len, read_at, read_len, answer_len, phrase_at;
    char input[DC_ACCOUNT_WIRE_CAP], line[DC_ACCOUNT_LINE_CAP];
    unsigned char read_buf[ACCOUNT_READ_BUDGET];
    char answer[DC_ACCOUNT_ANSWER_CAP], question[DC_ACCOUNT_QUESTION_CAP];
    struct account_pair history[DC_ACCOUNT_HISTORY_PAIRS];
    unsigned history_count;
    struct account_retired retired[ACCOUNT_RETIRED_CAP];
};

static int same_reply(dc_tag a, dc_tag b) {
    return a.session_epoch == b.session_epoch &&
           a.reply_generation == b.reply_generation;
}
static size_t utf8_cut(const char *s, size_t length, size_t limit) {
    size_t n = length < limit ? length : limit;
    if (n < length) while (n && ((unsigned char)s[n] & 0xc0) == 0x80) --n;
    return n;
}
/* Validate complete UTF-8, rejecting overlong encodings, surrogate codepoints
 * and embedded NUL. Normalized provider JSON strings must be complete strings,
 * even if read(2) split the surrounding NDJSON record. */
static int valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++], cp; size_t extra;
        if (!c) return 0;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31u; extra = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15u; extra = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7u; extra = 3; }
        else return 0;
        if (i + extra > n) return 0;
        for (size_t k = 0; k < extra; ++k) {
            unsigned b = s[i++]; if ((b & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (b & 63u);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10ffff ||
            (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    }
    return 1;
}
struct json_cursor { const unsigned char *s; size_t n, at; };
static void json_space(struct json_cursor *c) {
    while (c->at < c->n && (c->s[c->at] == ' ' || c->s[c->at] == '\t' ||
           c->s[c->at] == '\r' || c->s[c->at] == '\n')) ++c->at;
}
static int json_byte(struct json_cursor *c, unsigned wanted) {
    json_space(c);
    return c->at < c->n && c->s[c->at++] == wanted ? 0 : -1;
}
static int hex4(struct json_cursor *c, unsigned *v) {
    if (c->n - c->at < 4) return -1;
    *v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned ch = c->s[c->at++], digit;
        if (ch >= '0' && ch <= '9') digit = ch - '0';
        else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
        else return -1;
        *v = (*v << 4) | digit;
    }
    return 0;
}
static int append_cp(char *out, size_t cap, size_t *used, unsigned cp) {
    unsigned char bytes[4]; size_t n;
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
    if (cp < 0x80) { bytes[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) {
        bytes[0] = 0xc0 | (unsigned char)(cp >> 6);
        bytes[1] = 0x80 | (unsigned char)(cp & 63); n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = 0xe0 | (unsigned char)(cp >> 12);
        bytes[1] = 0x80 | (unsigned char)((cp >> 6) & 63);
        bytes[2] = 0x80 | (unsigned char)(cp & 63); n = 3;
    } else {
        bytes[0] = 0xf0 | (unsigned char)(cp >> 18);
        bytes[1] = 0x80 | (unsigned char)((cp >> 12) & 63);
        bytes[2] = 0x80 | (unsigned char)((cp >> 6) & 63);
        bytes[3] = 0x80 | (unsigned char)(cp & 63); n = 4;
    }
    if (*used + n >= cap) return -1;
    memcpy(out + *used, bytes, n); *used += n; return 0;
}
/* A small, complete parser for the normalized single-member NDJSON schema.
 * It decodes JSON strings/escapes rather than finding keys with strstr; this
 * avoids a 1024-token, ~28 KiB parser allocation on the audio-control stack.
 * Duplicate/unknown/mixed fields are rejected, not guessed as a success. */
static int json_string(struct json_cursor *c, char *out, size_t cap, size_t *length) {
    size_t used = 0;
    if (json_byte(c, '"')) return -1;
    while (c->at < c->n) {
        unsigned ch = c->s[c->at++];
        if (ch == '"') {
            if (!valid_utf8((unsigned char *)out, used)) return -1;
            out[used] = 0; *length = used; return 0;
        }
        if (ch < 32) return -1;
        if (ch != '\\') {
            if (used + 1 >= cap) return -1;
            out[used++] = (char)ch; continue;
        }
        if (c->at >= c->n) return -1;
        ch = c->s[c->at++];
        if (ch == 'u') {
            unsigned cp, low;
            if (hex4(c, &cp)) return -1;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (c->n - c->at < 6 || c->s[c->at++] != '\\' || c->s[c->at++] != 'u' ||
                    hex4(c, &low) || low < 0xdc00 || low > 0xdfff) return -1;
                cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            }
            if (append_cp(out, cap, &used, cp)) return -1;
            continue;
        }
        if (ch == 'b') ch = '\b'; else if (ch == 'f') ch = '\f';
        else if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r';
        else if (ch == 't') ch = '\t';
        else if (ch != '"' && ch != '\\' && ch != '/') return -1;
        if (used + 1 >= cap) return -1;
        out[used++] = (char)ch;
    }
    return -1;
}
static int json_literal(struct json_cursor *c, const char *value) {
    json_space(c); size_t n = strlen(value);
    if (c->n - c->at < n || memcmp(c->s + c->at, value, n)) return -1;
    c->at += n; return 0;
}
static int parse_line(const char *line, size_t length, dc_account_event *event) {
    struct json_cursor c = {(const unsigned char *)line, length, 0};
    char key[9]; size_t key_length;
    if (memchr(line, 0, length) || json_byte(&c, '{') ||
        json_string(&c, key, sizeof key, &key_length) || json_byte(&c, ':')) return -1;
    if (!strcmp(key, "delta")) {
        event->type = DC_ACCOUNT_DELTA;
        if (json_string(&c, event->text, sizeof event->text, &event->length)) return -1;
    } else if (!strcmp(key, "done")) {
        event->type = DC_ACCOUNT_DONE; json_space(&c);
        if (c.at < c.n && c.s[c.at] == '1') { if (json_literal(&c, "1")) return -1; }
        else if (json_literal(&c, "true")) return -1;
    } else if (!strcmp(key, "error") || !strcmp(key, "message")) {
        /* Existing account-common.sh fail() emits {message:string}; the real
         * model-tool SSE parser can emit {error:true}. Neither is an answer. */
        event->type = DC_ACCOUNT_ERROR; event->error = DC_ACCOUNT_ERR_ADAPTER;
        json_space(&c);
        if (!strcmp(key, "error") && c.at < c.n && c.s[c.at] == 't') {
            if (json_literal(&c, "true")) return -1;
        } else {
            if (json_string(&c, event->text, 241, &event->length) || !event->length) return -1;
            for (size_t i = 0; i < event->length; ++i)
                if ((unsigned char)event->text[i] < 32 || event->text[i] == 127) return -1;
        }
    } else return -1;
    if (json_byte(&c, '}')) return -1;
    json_space(&c); return c.at == c.n ? 0 : -1;
}

struct json_writer { char *s; size_t capacity, used; int failed; };
static void putn(struct json_writer *w, const char *s, size_t n) {
    if (w->failed || n >= w->capacity - w->used) { w->failed = 1; return; }
    memcpy(w->s + w->used, s, n); w->used += n; w->s[w->used] = 0;
}
static void puts_json(struct json_writer *w, const char *s) { putn(w, s, strlen(s)); }
static void quote_json(struct json_writer *w, const char *s) {
    puts_json(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        char escaped[7];
        if (*p == '"' || *p == '\\') { escaped[0] = '\\'; escaped[1] = (char)*p; putn(w, escaped, 2); }
        else if (*p < 32) { snprintf(escaped, sizeof escaped, "\\u%04x", *p); putn(w, escaped, 6); }
        else putn(w, (const char *)p, 1);
    }
    puts_json(w, "\"");
}
static int make_request(dc_account *a, const char *question, const char *model) {
    struct json_writer w = {a->input, sizeof a->input, 0, 0};
    puts_json(&w, "{\"values\":{\"model\":"); quote_json(&w, model);
    puts_json(&w, "},\"input\":[");
    for (unsigned i = 0; i < a->history_count; ++i) {
        puts_json(&w, "{\"role\":\"user\",\"content\":"); quote_json(&w, a->history[i].question);
        puts_json(&w, "},{\"role\":\"assistant\",\"content\":"); quote_json(&w, a->history[i].answer);
        puts_json(&w, "},");
    }
    puts_json(&w, "{\"role\":\"user\",\"content\":"); quote_json(&w, question);
    puts_json(&w, "}]}\n");
    if (w.failed) { errno = EMSGSIZE; return -1; }
    a->input_len = w.used; a->input_at = 0; return 0;
}
static void close_io(dc_account *a) {
    if (a->output_fd >= 0) close(a->output_fd);
    if (a->input_fd >= 0) close(a->input_fd);
    a->output_fd = a->input_fd = -1;
}
unsigned dc_account_retired(const dc_account *a) {
    unsigned count = 0;
    if (a) for (unsigned i = 0; i < ACCOUNT_RETIRED_CAP; ++i) count += a->retired[i].pid > 0;
    return count;
}
void dc_account_tick(dc_account *a, uint64_t now) {
    if (!a) return;
    for (unsigned i = 0; i < ACCOUNT_RETIRED_CAP; ++i) {
        struct account_retired *r = &a->retired[i];
        if (!r->pid) continue;
        /* Keep the unreaped group leader until KILL. Even if it exited after
         * TERM, its PID cannot be reused before this exact owned-group kill. */
        if (!r->killed && now >= r->kill_at) { kill(-r->pid, SIGKILL); r->killed = 1; }
        if (r->killed) {
            pid_t done = waitpid(r->pid, NULL, WNOHANG);
            if (done == r->pid || (done < 0 && errno == ECHILD)) memset(r, 0, sizeof *r);
        }
    }
}
static void retire_active(dc_account *a, uint64_t now) {
    close_io(a);
    if (a->pid > 0) {
        /* start reserves one slot for the only active child. */
        for (unsigned i = 0; i < ACCOUNT_RETIRED_CAP; ++i) if (!a->retired[i].pid) {
            a->retired[i].pid = a->pid; a->retired[i].kill_at = now + ACCOUNT_CANCEL_GRACE_MS;
            a->retired[i].killed = 0; kill(-a->pid, SIGTERM); a->pid = 0; break;
        }
    }
}
dc_account *dc_account_create(void) {
    dc_account *a = calloc(1, sizeof *a);
    if (a) a->input_fd = a->output_fd = a->sealed_fd = -1;
    return a;
}
int dc_account_bind_sealed_v1(dc_account *a, uint64_t epoch, int fd) {
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    ACCOUNT_STATBUF st, reopened;
    char path[64]; int copy = -1, check = -1, mode = -1;
    unsigned char *bytes = NULL;
    if (!a || a->sealed_requested || a->tag.session_epoch || a->pid) { errno = EINVAL; return -1; }
    a->sealed_requested = 1;
    if (!epoch || fd < 0) { errno = EINVAL; return -1; }
    int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & required) != required || ACCOUNT_FSTAT(fd, &st) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > DC_ACCOUNT_DECLARATION_INPUT_CAP) { errno = EINVAL; return -1; }
    copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (copy < 0) goto fail;
    snprintf(path, sizeof path, "/proc/self/fd/%d", copy);
    check = open(path, O_RDONLY | O_CLOEXEC);
    if (check < 0 || ACCOUNT_FSTAT(check, &reopened) || st.st_dev != reopened.st_dev || st.st_ino != reopened.st_ino) goto fail;
    bytes = malloc((size_t)st.st_size);
    if (!bytes) goto fail;
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = pread(check, bytes + used, (size_t)st.st_size - used, (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto fail;
        used += (size_t)n;
    }
    mode = dc_account_parse_declaration(bytes, used);
    if (mode != DC_ACCOUNT_KEYWORD && mode != DC_ACCOUNT_VOICE) { errno = EINVAL; mode = -1; goto fail; }
    free(bytes); close(check); a->sealed_fd = copy; a->sealed_epoch = epoch; return mode;
fail:
    { int saved = errno ? errno : EIO; free(bytes); if (check >= 0) close(check); if (copy >= 0) close(copy); errno = saved; return -1; }
}
int dc_account_destroy(dc_account *a) {
    if (!a) return 0;
    if (a->pid || dc_account_retired(a)) { errno = EBUSY; return -1; }
    close_io(a); if (a->sealed_fd >= 0) close(a->sealed_fd); free(a); return 0;
}
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
static int above_stdio(int *fd) {
    if (*fd > STDERR_FILENO) return 0;
    int moved = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) return -1;
    close(*fd); *fd = moved; return 0;
}
int dc_account_start(dc_account *a, dc_tag tag, const char *question,
                     const char *script, const char *model, uint64_t now) {
    if (!a || !question || !*question || !script || script[0] != '/' || !model || !*model ||
        !tag.session_epoch || !tag.reply_generation || !tag.utterance_id) { errno = EINVAL; return -1; }
    if (a->sealed_requested && a->sealed_fd < 0) { errno = EPERM; return -1; }
    size_t qn = strnlen(question, DC_ACCOUNT_QUESTION_CAP), mn = strnlen(model, 257);
    if (qn >= DC_ACCOUNT_QUESTION_CAP || mn > 256) { errno = EMSGSIZE; return -1; }
    if (!valid_utf8((const unsigned char *)question, qn) ||
        !valid_utf8((const unsigned char *)model, mn)) { errno = EINVAL; return -1; }
    if (a->pid || (a->tag.session_epoch == tag.session_epoch &&
                   tag.reply_generation <= a->tag.reply_generation)) { errno = EBUSY; return -1; }
    dc_account_tick(a, now);
    if (dc_account_retired(a) >= ACCOUNT_RETIRED_CAP) { errno = EBUSY; return -1; }
    if (a->sealed_fd >= 0) {
        if (tag.session_epoch != a->sealed_epoch) { errno = EINVAL; return -1; }
    } else {
        ACCOUNT_STATBUF st;
        if (ACCOUNT_LSTAT(script, &st) || !S_ISREG(st.st_mode)) { errno = EINVAL; return -1; }
    }
    if (a->tag.session_epoch != tag.session_epoch) {
        a->history_count = 0; memset(a->history, 0, sizeof a->history);
    }
    if (make_request(a, question, model)) return -1;
    int in[2], out[2];
    /* Socket input makes parent writes MSG_NOSIGNAL-safe even if an adapter
     * exits before reading. Do not change the controller's signal handlers. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, in)) return -1;
    if (pipe2(out, O_CLOEXEC)) { int saved = errno; close(in[0]); close(in[1]); errno = saved; return -1; }
    if (above_stdio(&in[0]) || above_stdio(&in[1]) || above_stdio(&out[0]) || above_stdio(&out[1]) ||
        set_nonblock(in[1]) || set_nonblock(out[0])) {
        int saved = errno; close(in[0]); close(in[1]); close(out[0]); close(out[1]); errno = saved; return -1;
    }
    pid_t parent = getpid(), pid = fork();
    if (pid < 0) { int saved = errno; close(in[0]); close(in[1]); close(out[0]); close(out[1]); errno = saved; return -1; }
    if (!pid) {
        struct rlimit cores = {0, 0};
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGPIPE, SIG_DFL);
        if (setpgid(0, 0) || prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent ||
            setrlimit(RLIMIT_CORE, &cores) || dup2(in[0], STDIN_FILENO) < 0 ||
            dup2(out[1], STDOUT_FILENO) < 0) _exit(126);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        if (a->sealed_fd >= 0) {
            char path[64];
            if (fcntl(a->sealed_fd, F_SETFD, 0)) _exit(126);
            snprintf(path, sizeof path, "/proc/self/fd/%d", a->sealed_fd);
            execl("/bin/sh", "sh", path, "--model-request", (char *)NULL);
        } else execl("/bin/sh", "sh", script, "--model-request", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]); setpgid(pid, pid);
    a->pid = pid; a->input_fd = in[1]; a->output_fd = out[0]; a->tag = tag;
    a->deadline = now + ACCOUNT_TIMEOUT_MS; a->eof = a->protocol_done = a->successful = a->invalidated = 0;
    a->first_delta = a->pending_since = 0; a->next_sequence = 1;
    a->line_len = a->read_at = a->read_len = a->answer_len = a->phrase_at = 0;
    a->answer[0] = 0; memcpy(a->question, question, qn + 1);
    return 0;
}
int dc_account_fd(const dc_account *a) { return a ? a->output_fd : -1; }
int dc_account_write_fd(const dc_account *a) { return a ? a->input_fd : -1; }
int dc_account_busy(const dc_account *a) { return a && a->pid > 0; }
int dc_account_drained(const dc_account *a) {
    return !a || ((!a->pid || a->protocol_done) && a->phrase_at == a->answer_len);
}
unsigned dc_account_history_count(const dc_account *a) { return a ? a->history_count : 0; }
size_t dc_account_storage_bytes(void) { return sizeof(dc_account); }
int dc_account_cancel(dc_account *a, dc_tag tag, uint64_t now) {
    if (!a || a->invalidated || !same_reply(a->tag, tag)) return 0;
    /* Invalidate user-visible output before signalling any slow child. */
    a->answer_len = a->phrase_at = a->line_len = a->read_at = a->read_len = 0;
    a->answer[0] = 0; a->successful = a->protocol_done = 0; a->invalidated = 1;
    a->pending_since = 0; retire_active(a, now); return 1;
}
static int emit_error(dc_account *a, uint64_t now, dc_account_event *event,
                       enum dc_account_error error) {
    if (event->type != DC_ACCOUNT_ERROR || error != DC_ACCOUNT_ERR_ADAPTER) {
        event->length = 0; event->text[0] = 0;
    }
    event->type = DC_ACCOUNT_ERROR; event->error = error; event->tag = a->tag; event->at_ms = now;
    a->answer_len = a->phrase_at = 0; a->answer[0] = 0;
    a->successful = a->protocol_done = 0; a->invalidated = 1; retire_active(a, now); return 1;
}
static void commit_history(dc_account *a) {
    if (a->history_count == DC_ACCOUNT_HISTORY_PAIRS) {
        memmove(a->history, a->history + 1, (DC_ACCOUNT_HISTORY_PAIRS - 1) * sizeof a->history[0]);
        --a->history_count;
    }
    struct account_pair *pair = &a->history[a->history_count++];
    memcpy(pair->question, a->question, strlen(a->question) + 1);
    size_t n = utf8_cut(a->answer, a->answer_len, sizeof pair->answer - 1);
    memcpy(pair->answer, a->answer, n); pair->answer[n] = 0;
}
int dc_account_poll(dc_account *a, uint64_t now, dc_account_event *event) {
    if (!a || !event) { errno = EINVAL; return -1; }
    memset(event, 0, sizeof *event); dc_account_tick(a, now);
    if (!a->pid) return 0;
    if (now >= a->deadline) return emit_error(a, now, event, DC_ACCOUNT_ERR_TIMEOUT);
    if (a->input_fd >= 0) {
        size_t n = a->input_len - a->input_at;
        if (n > ACCOUNT_READ_BUDGET) n = ACCOUNT_READ_BUDGET;
        ssize_t sent = send(a->input_fd, a->input + a->input_at, n, MSG_NOSIGNAL);
        if (sent > 0) a->input_at += (size_t)sent;
        else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return emit_error(a, now, event, DC_ACCOUNT_ERR_IO);
        if (a->input_at == a->input_len) { close(a->input_fd); a->input_fd = -1; }
    }
    unsigned budget = ACCOUNT_READ_BUDGET;
    while (budget) {
        if (a->read_at == a->read_len) {
            a->read_at = a->read_len = 0;
            if (a->eof) break;
            ssize_t n = read(a->output_fd, a->read_buf, budget);
            if (n > 0) a->read_len = (size_t)n;
            else if (!n) { a->eof = 1; break; }
            else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            else return emit_error(a, now, event, DC_ACCOUNT_ERR_IO);
        }
        while (a->read_at < a->read_len && budget) {
            unsigned ch = a->read_buf[a->read_at++]; --budget;
            if (a->protocol_done) return emit_error(a, now, event, DC_ACCOUNT_ERR_PROTOCOL);
            if (ch != '\n') {
                if (!ch || a->line_len + 1 >= sizeof a->line)
                    return emit_error(a, now, event, DC_ACCOUNT_ERR_LIMIT);
                a->line[a->line_len++] = (char)ch; continue;
            }
            if (parse_line(a->line, a->line_len, event))
                return emit_error(a, now, event, DC_ACCOUNT_ERR_PROTOCOL);
            a->line_len = 0; event->tag = a->tag; event->at_ms = now;
            if (event->type == DC_ACCOUNT_ERROR) return emit_error(a, now, event, event->error);
            if (event->type == DC_ACCOUNT_DONE) { a->protocol_done = 1; continue; }
            if (event->length) {
                if (a->answer_len + event->length >= sizeof a->answer)
                    return emit_error(a, now, event, DC_ACCOUNT_ERR_LIMIT);
                if (!a->first_delta) a->first_delta = now;
                if (a->phrase_at == a->answer_len) a->pending_since = now;
                memcpy(a->answer + a->answer_len, event->text, event->length + 1);
                a->answer_len += event->length;
            }
            return 1;
        }
    }
    if (a->eof) {
        if (a->line_len || !a->protocol_done || !a->answer_len)
            return emit_error(a, now, event, DC_ACCOUNT_ERR_PROTOCOL);
        /* Peek without reaping: a failure still needs an owned-group cancel.
         * Its unreaped leader prevents a delayed kill from reaching PID reuse. */
        siginfo_t info; memset(&info, 0, sizeof info);
        int ready = waitid(P_PID, (id_t)a->pid, &info, WEXITED | WNOHANG | WNOWAIT);
        if (ready && errno == EINTR) return 0;
        if (!ready && !info.si_pid) return 0;
        if (ready || info.si_code != CLD_EXITED || info.si_status)
            return emit_error(a, now, event, DC_ACCOUNT_ERR_ADAPTER);
        kill(-a->pid, SIGKILL); /* harmless to the zombie; closes leftover descendants */
        if (waitpid(a->pid, NULL, WNOHANG) != a->pid)
            return emit_error(a, now, event, DC_ACCOUNT_ERR_IO);
        a->pid = 0; close_io(a); a->successful = 1;
        commit_history(a); event->type = DC_ACCOUNT_DONE; event->tag = a->tag; event->at_ms = now;
        return 1;
    }
    return 0;
}
/* Find sentence punctuation only at decoded UTF-8 boundaries. */
static size_t sentence_boundary(const char *s, size_t n, int first) {
    size_t last = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned ch = (unsigned char)s[i];
        if (ch < 128 && (strchr(".!?;\n", (int)ch) || (first && i >= 18 && ch == ',')))
            last = i + 1;
        if (i + 3 <= n && (!memcmp(s + i, "。", 3) || !memcmp(s + i, "？", 3) ||
            !memcmp(s + i, "！", 3) || (first && i >= 18 && !memcmp(s + i, "，", 3)))) {
            last = i + 3; if (first) break;
        }
    }
    return last;
}
int dc_account_phrase(dc_account *a, uint64_t now, dc_account_event *event) {
    if (!a || !event) { errno = EINVAL; return -1; }
    if (a->phrase_at >= a->answer_len) return 0;
    size_t available = a->answer_len - a->phrase_at;
    size_t limit = a->phrase_at ? 240 : 72;
    const char *s = a->answer + a->phrase_at;
    size_t n = utf8_cut(s, available, limit), boundary = sentence_boundary(s, n, !a->phrase_at);
    if (boundary) n = boundary;
    else if (!a->protocol_done && !a->successful && available < limit &&
             !(available >= 18 && now >= a->pending_since && now - a->pending_since >= 250)) return 0;
    if (!n) return 0;
    memset(event, 0, sizeof *event); event->type = DC_ACCOUNT_PHRASE; event->tag = a->tag;
    event->at_ms = now; event->segment_sequence = a->next_sequence++;
    event->length = n; memcpy(event->text, s, n); event->text[n] = 0; a->phrase_at += n;
    /* The next unconsumed suffix is already available; give only newly sparse
     * text a fresh 250 ms wait. Never use a two-second smooth-mode delay. */
    if (a->phrase_at == a->answer_len) a->pending_since = 0;
    return 1;
}

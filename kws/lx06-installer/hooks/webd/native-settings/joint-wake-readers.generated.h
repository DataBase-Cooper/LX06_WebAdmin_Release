/* Exact original A readers. */
#ifndef LX06_NATIVE_TARGET_SETTINGS
#error "wake review is fixture-only"
#endif
#define WW_MAXWORDS 6
static const char *wake_review_conf_path;
#define WWCONF wake_review_conf_path

/* Observe only I/O failure; ORIGINAL parsing/default semantics stay below. */
static int wake_review_io_failed;
static FILE *wake_review_fopen(const char *path,const char *mode) {
    FILE *f=fopen(path,mode);
    if(!f&&errno!=ENOENT) wake_review_io_failed=1;
    return f;
}
static int wake_review_fclose(FILE *f) {
    if(ferror(f)) wake_review_io_failed=1;
    int rc=fclose(f);if(rc) wake_review_io_failed=1;return rc;
}
static int wake_review_open(const char *path,int flags) {
    int fd=open(path,flags);if(fd<0&&errno!=ENOENT) wake_review_io_failed=1;return fd;
}
static ssize_t wake_review_read(int fd,void *bytes,size_t count) {
    ssize_t n=read(fd,bytes,count);if(n<0) wake_review_io_failed=1;return n;
}
static int wake_review_close(int fd) {
    int rc=close(fd);if(rc) wake_review_io_failed=1;return rc;
}
#define fopen wake_review_fopen
#define fclose wake_review_fclose
#define open wake_review_open
#define read wake_review_read
#define close wake_review_close
typedef struct {
  char name[24];    /* UTF-8，2–6 汉字（≤18 字节） */
  double th;        /* Feature-specific distance; six decimals survive edits. */
  char feature[64]; /* Explicit threshold calibration identity, never inferred. */
  char model_sha256[65]; /* Calibration belongs to these exact trained bytes. */
  char link[65];    /* 关联脚本名（app_name_safe），可为空 */
  int enabled;
  int p05_conflict, p05_near, p05_pending, p05_requested;
} ww_word_t;
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
static inline int lx06_bind_name_ok(const char *s) {
    size_t n, i;
    if (!s) return 0;
    n = strlen(s);
    if (!n || n > 64 || s[0] == '.') return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static inline int lx06_official_kws_enabled_at(const char *path) {
    char c = '1';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 1;
    if (read(fd, &c, 1) < 0) c = '1';
    close(fd);
    return c != '0';
}

static inline int lx06_official_bind_read_at(const char *path, char *out, size_t cap) {
    char buf[80];
    ssize_t n;
    int fd;
    if (!out || cap < 2) return -1;
    out[0] = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
    buf[n] = 0;
    if (!buf[0]) return 0;
    if (!lx06_bind_name_ok(buf) || (size_t)n >= cap) return -1;
    memcpy(out, buf, (size_t)n + 1);
    return 1;
}

#undef WWCONF
#undef fopen
#undef fclose
#undef open
#undef read
#undef close

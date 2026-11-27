/* Generated from pinned webd; fixture only. */
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
static int appmeta_count(const struct ctl_selection *selection) {
  char buf[16384];
  long n = ctl_selection_meta_read(selection, buf, sizeof(buf));
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
static int appmeta_get(const struct ctl_selection *selection, int idx, char *name, size_t namesz, long *mt) {
  char buf[16384];
  long n = ctl_selection_meta_read(selection, buf, sizeof(buf));
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

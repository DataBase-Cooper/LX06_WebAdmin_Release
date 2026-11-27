#define WW_EPOCH_EVENT WWEVENT ".epoch"
#define WW_EPOCH_TTL_MS 3000u
#ifndef LX06_NATIVE_REQUEST
#define LX06_NATIVE_REQUEST "/tmp/lx06-native-request"
#endif

static int ww_write_route(const char *path, const char *message) {
  char temporary[192];
  int len = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
  if (len < 0 || (size_t)len >= sizeof(temporary)) return -1;
  int fd = mkostemp(temporary, O_CLOEXEC);
  if (fd < 0) return -1;
  int result = write_all(fd, message, strlen(message));
  if (close(fd)) result = -1;
  if (!result && !rename(temporary, path)) return 0;
  unlink(temporary);
  return -1;
}

static void ww_epoch_route(const ww_word_t *word, double score, const char *source,
                           const char *epoch, uint64_t first, uint64_t end,
                           uint64_t expires, int paired) {
  char message[192];
  if (!word->link[0]) {
    if (paired)
      snprintf(message, sizeof(message), "wake3 %s %s %llu %llu %llu\n", epoch, source,
               (unsigned long long)first, (unsigned long long)end, (unsigned long long)expires);
    else
      snprintf(message, sizeof(message), "wake2 %s %s %llu %llu\n", epoch, source,
               (unsigned long long)end, (unsigned long long)expires);
    if (!ww_write_route(LX06_NATIVE_REQUEST, message))
      ww_log("TRIGGER-QUEUED label=%s action=OFFICIAL-NATIVE", word->name);
    else ww_log("TRIGGER action=OFFICIAL-REQUEST-FAIL label=%s", word->name);
    return;
  }
  unsigned required = APP_LOCAL_WAKEWORD | APP_RUN_ROOT | APP_SESSION_MANAGED | APP_LOCAL_ADMISSION;
  if ((app_file_marker_flags(word->link) & required) != required ||
      app_phone_sender(word->link) != 0 || lx06_local_channel_armed_at(APPPERSIST, word->link)) {
    ww_log("TRIGGER action=GUARDED-SCRIPT-REQUIRED label=%s link=%s", word->name, word->link);
    return;
  }
  if (app_running(word->link)) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/lx06-wake-%s.event", word->link);
    snprintf(message, sizeof(message), "%s %.6f %.3f %s:%llu\n", word->name, score,
             (expires - WW_EPOCH_TTL_MS) / 1000.0, epoch, (unsigned long long)expires);
    if (!ww_write_route(path, message))
      ww_log("TRIGGER-QUEUED label=%s action=RESIDENT link=%s", word->name, word->link);
    else ww_log("TRIGGER action=RESIDENT-FAIL label=%s link=%s", word->name, word->link);
  } else {
    snprintf(message, sizeof(message), "--wakeup %s %s:%llu", word->name, epoch,
             (unsigned long long)expires);
    if (run_app_profile(word->link, message, LK_ROOT | APP_LOCAL_ADMISSION) > 0)
      ww_log("TRIGGER-QUEUED label=%s action=RUN link=%s", word->name, word->link);
    else ww_log("TRIGGER action=RUN-FAIL label=%s link=%s", word->name, word->link);
  }
}

/* Only the local, root-owned matcher publishes .paired after checking its
 * two synchronized signals. Labels or raw-only receipts cannot enter here. */
static int ww_pair_number(const char *s, uint64_t *out) {
  uint64_t value=0;
  if (!*s) return 0;
  while (*s) {
    if (*s<'0' || *s>'9') return 0;
    unsigned digit=(unsigned)(*s++-'0');
    if (value>(UINT64_MAX-digit)/10u) return 0;
    value=value*10u+digit;
  }
  *out=value;
  return 1;
}

static void ww_epoch_poll(void) {
  int paired=access(WWHOME "/paired-capture-v1", R_OK)==0 && !ww_epoch_capture_enabled();
  if (!paired && !ww_epoch_capture_enabled()) return;
  const char *event=paired ? WWEVENT ".paired" : WW_EPOCH_EVENT;
  const char *claimed=paired ? WWEVENT ".paired.webd" : WW_EPOCH_EVENT ".webd";
  if (rename(event, claimed)) return;
  int fd = open(claimed, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  unlink(claimed);
  if (fd < 0) return;
  LX06_STATBUF st;
  char record[320];
  ssize_t n = -1;
  if (!LX06_FSTAT(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
      !st.st_nlink && !(st.st_mode & 0077))
    n = read(fd, record, sizeof(record));
  close(fd);
  if (n < 2 || n >= (ssize_t)sizeof(record) || n != st.st_size ||
      record[n - 1] != '\n' || memchr(record, 0, (size_t)n)) return;
  record[n - 1] = 0;
  char *fields[10], *cursor=record;
  int count=paired ? 10 : 7;
  for (int i=0; i<count; ++i) {
    fields[i]=strsep(&cursor," ");
    if (!fields[i] || !fields[i][0]) return;
  }
  int cloud_near = paired && cursor && !strcmp(cursor, "cloud");
  int local_near = paired && cursor && !strcmp(cursor, "local-near");
  const char separated_tag[] = "local-near-separated ";
  int separated = paired && cursor && !strncmp(cursor, separated_tag, sizeof(separated_tag)-1);
  if (cursor && !cloud_near && !local_near && !separated) return;
  const char *label=fields[0], *source=fields[paired ? 5 : 2], *epoch=fields[paired ? 6 : 3];
  uint64_t first, end, available;
  char *tail;
  double score;
  unsigned char identity[LX06_IDLE_EPOCH_BYTES], expected[LX06_IDLE_EPOCH_BYTES];
  errno=0; score=strtod(fields[1],&tail);
  if (errno || tail==fields[1] || *tail || !ww_name_ok(label) || !isfinite(score) || score < 0 ||
      !ww_pair_number(fields[count-3],&first) || !ww_pair_number(fields[count-2],&end) ||
      !ww_pair_number(fields[count-1],&available) || end<=first ||
      (end - first) % 320 || lx06_idle_epoch_parse(source, identity) ||
      lx06_idle_epoch_parse(epoch, expected)) return;
  if (paired) {
    uint64_t stock_first, stock_end;
    unsigned char stock_source[LX06_IDLE_EPOCH_BYTES];
    if (lx06_idle_epoch_parse(fields[2],stock_source) || !memcmp(stock_source,identity,sizeof(identity)) ||
        !ww_pair_number(fields[3],&stock_first) || !ww_pair_number(fields[4],&stock_end) ||
        stock_first%320 || stock_end<=stock_first || stock_end-stock_first!=end-first ||
        end-first<9600 || end-first>40000) return;
  }
  uint64_t now = lx06_idle_epoch_now_ms();
  if (!available || available > now || now - available >= WW_EPOCH_TTL_MS ||
      ww_recording_active() || ww_official_priority() || ww_preview_active() || lx06_listen_alive()) return;
  ww_word_t word;
  if (!ww_target(label, &word)) return;
  if (cloud_near || local_near || separated) {
    float ceiling = cloud_near ? WAKE_CLOUD_NEAR_CEILING : WAKE_LOCAL_NEAR_CEILING;
    float ratio_limit = 1.f + WAKE_NORM_BAND;
    if (separated) {
      const char *raw = cursor + sizeof(separated_tag)-1;
      errno = 0;
      double runner = strtod(raw, &tail);
      if (*raw < '0' || *raw > '9' || errno || *tail || !isfinite(runner) ||
          runner-score < 0.025) return;
      ceiling = 0.265f;
      if (score <= 0.260 && runner-score >= 0.040) ratio_limit = 1.22f;
    }
    if (ww_cloud_on() != cloud_near || !(word.th > 0) || (float)score > ceiling ||
        (float)score/(float)word.th > ratio_limit) return;
  } else if (score > word.th) return;
  /* Queue against the observed idle generation alongside the capture reader.
   * The native callback or session-run takes it exclusively at consumption. */
  int lease = lx06_idle_epoch_observe(expected);
  if (lease < 0) return;
  close(lease);
  ww_epoch_route(&word, score, source, epoch, first, end, available + WW_EPOCH_TTL_MS, paired);
}

static void ww_poll_routes(void) {
  ww_event_poll();
  ww_epoch_poll();
}

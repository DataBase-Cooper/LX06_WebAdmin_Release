/* P0.5 runtime glue. Included only from wakeword_template.c. */
#include <sys/file.h>
#include <sys/socket.h>
/* At most one killed helper may await scheduling. Never block the detector in
 * waitpid: retry on each existing PCM block, or before another verification. */
static pid_t p05_cloud_pending;
static void p05_cloud_reap(void) {
    if (p05_cloud_pending > 0) {
        int status;
        pid_t got = waitpid(p05_cloud_pending, &status, WNOHANG);
        if (got == p05_cloud_pending || (got < 0 && errno == ECHILD)) p05_cloud_pending = 0;
    }
}
static int p05_raw_policy;
static char p05_sha[MAX_MODELS][65];
/* Retain the longest model and confirmation span through the decision wait
 * and the existing near-miss report interval. Only the selected clip is saved. */
#define P05_NEAR_WAIT_BLOCKS 75
/* Let the same speech progress from the near band into its own gate before
 * taking a diagnostic snapshot. A passing candidate never waits for this. */
#define P05_NEAR_SETTLE_BLOCKS 40
#define P05_CONFIRM_LEAD (2 * WW_SCORE_HOP_BLOCKS * BLOCK_SAMPLES)
#define P05_HISTORY_SAMPLES (MAX_UTT_SAMPLES + P05_CONFIRM_LEAD + \
    (PRE_BLOCKS + P05_NEAR_WAIT_BLOCKS + WW_DECISION_WAIT_BLOCKS + WW_SCORE_HOP_BLOCKS)*BLOCK_SAMPLES)
#define P05_SPEECH_GAP_BLOCKS 25
#define P05_NEAR_EPISODES (P05_HISTORY_SAMPLES / (BLOCK_SAMPLES * P05_SPEECH_GAP_BLOCKS) + 2)
#define P05_MAX_CAPTURE (4 * SAMPLE_RATE)
static int16_t p05_ring[P05_HISTORY_SAMPLES];
static unsigned p05_voiced[P05_HISTORY_SAMPLES / BLOCK_SAMPLES];
static unsigned p05_ring_pos, p05_ring_have;
static uint64_t p05_stream_end, p05_clip_start;
static unsigned p05_episode, p05_silence, p05_near_seen[P05_NEAR_EPISODES], p05_near_next;
static int16_t p05_capture[P05_MAX_CAPTURE];
static int p05_capture_n, p05_capture_pre, p05_post_left;
static int p05_mute_until;
static unsigned p05_seq;
static struct p05_event {
    int live, audio, sent, elapsed_ms, delay_ms, nwords,local_near,phrase_fast;
    char id[40], result[24], dispatched[48], verdict[24], text[160], pinyin[160];
    float raw_margin, norm_margin;
    double uptime; long long wall;
    char label[MAX_MODELS][48];
    char sha[MAX_MODELS][65];
    float score[MAX_MODELS], gate[MAX_MODELS], ratio[MAX_MODELS], rms[MAX_MODELS], trail[MAX_MODELS][3];
    int window[MAX_MODELS], trail_end_ms[MAX_MODELS];
    int winner_refs, winner_missing;
    uint64_t receipt_id;
    uint64_t near_first, near_end;
    unsigned near_episode;
} p05_ev;

/* Last-written slot is step%3; export oldest to newest, never repeat a
 * summary score to invent confirmations. A snapshot stays with its own window. */
static void p05_save_trail(float out[3], const float ring[3], int step) {
    for (int j = 0; j < 3; j++) out[j] = ring[(step + 1 + j) % 3];
}
/* Keep every duration's confirmed result for only the PCM history retained
 * by the detector. A prior utterance's best score cannot be used as the
 * runner-up for a new borderline word. */
#define P05_SCORE_SLOTS (P05_HISTORY_SAMPLES/(WW_SCORE_HOP_BLOCKS*BLOCK_SAMPLES)+1)
struct p05_observation {
    float score,rms,trail[3];
    int width;
};
static int p05_hop_ms[P05_SCORE_SLOTS];
static struct p05_observation p05_hops[P05_SCORE_SLOTS][MAX_MODELS][WW_MAX_WINDOWS];
static void p05_observe_score(int step,int end_ms,int model,int duration,int width,
                              float score,float rms,const float trail[3]) {
    int slot=step%P05_SCORE_SLOTS;
    p05_hop_ms[slot]=end_ms;
    struct p05_observation *o=&p05_hops[slot][model][duration];
    o->width=width;o->score=score;o->rms=rms;
    if(score<99.f) p05_save_trail(o->trail,trail,step);
}
static const struct p05_observation *p05_rival_window(int model,int target_end,int target_width,int *best_end) {
    const struct p05_observation *best=NULL;
    const int64_t end=(int64_t)target_end*16,first=end-target_width;
    for(int s=0;s<P05_SCORE_SLOTS;s++) {
        int64_t rival_end=(int64_t)p05_hop_ms[s]*16;
        if(rival_end<=first) continue;
        for(int w=0;w<WW_MAX_WINDOWS;w++) {
            const struct p05_observation *o=&p05_hops[s][model][w];
            if(o->width<=0 || o->score>=99.f || (best && o->score>=best->score)) continue;
            int64_t a=rival_end-o->width;
            int64_t overlap=(rival_end<end ? rival_end : end)-(a>first ? a : first);
            /* Both scored windows must cover the same portion of speech.
             * All overlapping competing durations enter on equal footing. */
            if(2*overlap<(target_width>o->width ? target_width : o->width)) continue;
            best=o;*best_end=p05_hop_ms[s];
        }
    }
    return best;
}
static void p05_ring_push(const int16_t *block) {
    p05_cloud_reap();
    double power = 0;
    for (int i = 0; i < BLOCK_SAMPLES; ++i) power += (double)block[i] * block[i];
    unsigned voiced = 0;
    if (power > WW_QUIET_RMS * WW_QUIET_RMS * BLOCK_SAMPLES) {
        if (!p05_episode || p05_silence >= P05_SPEECH_GAP_BLOCKS) ++p05_episode;
        p05_silence = 0;
        voiced = p05_episode;
    } else if (p05_silence < P05_SPEECH_GAP_BLOCKS) ++p05_silence;
    p05_voiced[p05_ring_pos / BLOCK_SAMPLES] = voiced;
    memcpy(p05_ring + p05_ring_pos, block, BLOCK_SAMPLES * sizeof(*block));
    p05_ring_pos = (p05_ring_pos + BLOCK_SAMPLES) % P05_HISTORY_SAMPLES;
    if (p05_ring_have < P05_HISTORY_SAMPLES) p05_ring_have += BLOCK_SAMPLES;
    p05_stream_end += BLOCK_SAMPLES;
    if (p05_post_left > 0 && p05_capture_n < P05_MAX_CAPTURE) {
        int room = P05_MAX_CAPTURE - p05_capture_n;
        int take = BLOCK_SAMPLES < room ? BLOCK_SAMPLES : room;
        if (take > p05_post_left) take = p05_post_left;
        memcpy(p05_capture + p05_capture_n, block, (size_t)take * sizeof(int16_t));
        p05_capture_n += take;
        p05_post_left -= take;
    }
}
static const char *p05_data_root(void) {
    const char *root = getenv("LX06_WAKE_DATA");
    return root && *root ? root : WAKE_DATA_DIR;
}
static const char *p05_forensics_dir(void) {
    const char *dir = getenv("LX06_WAKE_FORENSICS_DIR");
    return dir && *dir ? dir : WAKE_FORENSICS_DIR;
}
static int p05_forensics_on(void) {
    char path[512];
    const char *env = getenv("LX06_WAKE_FORENSICS");
    if (env && !strcmp(env, "0")) return 0;
    if (env && !strcmp(env, "1")) return 1;
    snprintf(path, sizeof(path), "%s/forensics-enabled", p05_data_root());
    return wake_forensics_enabled(path);
}
static int p05_cloud_on(void) {
    char value[16];
    const char *env = getenv("LX06_WAKE_CLOUD_VERIFY");
    if (env) return !strcmp(env, "on") || !strcmp(env, "1");
    if (wake_settings_read(p05_data_root(), "cloud-verify", value, sizeof(value)) != 0) return 0;
    return !strcmp(value, "on");
}
static void p05_json_escape(const char *in, char *out, size_t cap) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 2 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; if (j + 1 < cap) out[j++] = (char)c; }
        else if (c >= 32) out[j++] = (char)c;
    }
    out[j] = 0;
}
#include "wake-cloud-instructions.inc"
static void p05_official_state(struct wake_official_view *view,int check_log) {
    unsigned serial = 0; long long mt = 0;
    char kind[32];
    const char *native = getenv("LX06_WAKE_NATIVE");
    const char *router = getenv("LX06_WAKE_ROUTER_STATE");
    const char *session = getenv("LX06_WAKE_SESSION");
    time_t now = time(NULL);
    memset(view, 0, sizeof(*view));
    if (!native || !*native) native = WAKE_NATIVE_EVENT;
    if (!router || !*router) router = WAKE_ROUTER_STATE;
    if (!session || !*session) session = WAKE_SESSION_FILE;
    if (wake_native_snapshot(native, &serial, &mt, kind, sizeof(kind)) == 0 &&
        now >= (time_t)mt && now - (time_t)mt <= WAKE_CLOUD_IDLE_SEC)
        view->native_recent = 1;
    if(check_log) {
        struct p05_instruction_cursor cursor;
        if(!p05_log_snapshot(&cursor) || (cursor.exists && now-cursor.state.st_mtime<=WAKE_CLOUD_IDLE_SEC &&
           !(p05_last_aux.exists && p05_same_log(&cursor.state,&p05_last_aux.state)))) view->router_busy=1;
    }
    {
        FILE *file = fopen(router, "r");
        char line[64] = {0};
        if (file) {
            if (fgets(line, sizeof(line), file) &&
                (strstr(line, "busy") || strstr(line, "recognize") || strstr(line, "session")))
                view->router_busy = 1;
            fclose(file);
        }
    }
    {
        int fd=lx06_session_lock_idle_at(session,LOCK_SH,0);
        if(fd>=0) close(fd);
        else if(errno!=ENOENT) view->session_busy=1;
    }
}
static void p05_official(struct wake_official_view *view) { p05_official_state(view,1); }
static int p05_cloud_elapsed(const struct timespec *start) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return WAKE_CLOUD_BUDGET_MS;
    return (int)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}
static int p05_cloud_interrupted(const char *native, unsigned serial, long long mtime,
                                 struct p05_instruction_cursor *log,const char *owned) {
    struct wake_official_view view;
    unsigned current = 0; long long changed = 0;
    wake_native_snapshot(native, &current, &changed, NULL, 0);
    if (wake_native_changed(serial, mtime, current, changed)) {
        if(paired) fputs("CLOUD_CANCEL native-change\n",stderr);
        return 1;
    }
    p05_official_state(&view,log==NULL);
    if(wake_official_busy(&view)) {
        if(paired) fprintf(stderr,"CLOUD_CANCEL official native=%d router=%d owner=%d\n",view.native_recent,view.router_busy,view.session_busy);
        return 1;
    }
    if(log && !p05_check_instructions(log,owned)) {
        if(paired) fprintf(stderr,"CLOUD_CANCEL instruction has_ready=%d\n",*owned!=0);
        return 1;
    }
    return 0;
}
/* Returns 1 with text, 0 empty, -1 unavailable, -2 timeout, -3 cancelled. */
static int p05_verify_clip(const int16_t *clip, int samples, char *text, int cap, int *elapsed_ms) {
    const char *helper = getenv("LX06_WAKE_CLOUD_HELPER");
    const char *sock = getenv("LX06_WAKE_CLOUD_SOCKET");
    const char *native = getenv("LX06_WAKE_NATIVE");
    unsigned before_serial = 0;
    char owned_dialog[96]={0};
    struct p05_instruction_cursor log;
    long long before_m = 0;
    int channel[2] = {-1, -1}, status = 0, done = 0, eof = 0, rc = -1;
    pid_t pid = -1;
    size_t sent = 0, received = 0;
    char response[512];
    struct timespec start;
    /* Reserve shutdown time inside the existing 800ms, never add a new wait
     * after that deadline. This is a local helper lifecycle detail. */
    const int grace_ms = 10;
    if (elapsed_ms) *elapsed_ms = 0;
    if (!text || cap < 2) return -1;
    text[0] = 0;
    if (!clip || wake_cloud_clip_samples(samples, 0) < 0) return -1;
    p05_cloud_reap();
    if (p05_cloud_pending || clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    if (!helper || !*helper) helper = WAKE_CLOUD_HELPER;
    if (!sock || !*sock) sock = WAKE_CLOUD_SOCKET;
    if (!native || !*native) native = WAKE_NATIVE_EVENT;
    if (access(helper, X_OK) != 0) goto finish;
    wake_native_snapshot(native, &before_serial, &before_m, NULL, 0);
    if (!p05_log_snapshot(&log) || p05_cloud_interrupted(native,before_serial,before_m,NULL,"")) { rc = -3; goto finish; }
    /* Same stdin PCM / stdout line interface. A private full-duplex socketpair
     * permits half-close and MSG_NOSIGNAL without changing process SIGPIPE. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel)) goto finish;
    for (int i = 0; i < 2; i++) {
        if (channel[i] <= STDERR_FILENO) {
            int copy = fcntl(channel[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (copy < 0) goto finish;
            close(channel[i]); channel[i] = copy;
        }
    }
    int flags = fcntl(channel[0], F_GETFL);
    if (flags < 0 || fcntl(channel[0], F_SETFL, flags | O_NONBLOCK)) goto finish;
    int send_buffer = 4096;
    if (setsockopt(channel[0], SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer))) goto finish;
    if (p05_cloud_elapsed(&start) >= WAKE_CLOUD_BUDGET_MS - grace_ms) { rc = -2; goto finish; }
    pid = fork();
    if (pid < 0) goto finish;
    if (!pid) {
        close(channel[0]);
        if (dup2(channel[1], STDIN_FILENO) < 0 || dup2(channel[1], STDOUT_FILENO) < 0) _exit(127);
        close(channel[1]);
        if(setenv("LX06_WAKE_REPORT_DIALOG","1",1)) _exit(127);
        execl(helper, helper, sock, (char *)NULL);
        _exit(127);
    }
    close(channel[1]); channel[1] = -1;
    for (;;) {
        int left = WAKE_CLOUD_BUDGET_MS - grace_ms - p05_cloud_elapsed(&start);
        if (left <= 0) { rc = -2; break; }
        if (!done) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) done = 1;
            else if (got < 0 && errno != EINTR) { if (errno == ECHILD) done = 1; break; }
            if (done && (!WIFEXITED(status) || WEXITSTATUS(status))) {
                rc = WIFEXITED(status) && WEXITSTATUS(status) == 4 ? -2 :
                     WIFEXITED(status) && WEXITSTATUS(status) == 3 ? -3 : -1;
                break;
            }
        }
        if (!eof) {
            char buf[512];
            ssize_t n = recv(channel[0], buf, sizeof(buf), MSG_DONTWAIT);
            if (n > 0) {
                if (memchr(buf, 0, (size_t)n) || (size_t)n > sizeof(response) - received) break;
                memcpy(response + received, buf, (size_t)n); received += (size_t)n;
            } else if (!n) eof = 1;
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if(received && response[0]==0x1e) {
                char *end=memchr(response,'\n',received);
                if(end) {
                    size_t idlen=(size_t)(end-response)-1;
                    if(owned_dialog[0] || !idlen || idlen>=sizeof(owned_dialog)) break;
                    memcpy(owned_dialog,response+1,idlen);
                    if(strncmp(owned_dialog,"lx06-ctrl-",10)) break;
                    received-=idlen+2;
                    memmove(response,end+1,received);
                }
            }
        }
        if (p05_cloud_interrupted(native,before_serial,before_m,&log,owned_dialog)) { rc = -3; break; }
        if (sent < (size_t)samples * 2) {
            int lease=-1;
            if(paired && (lease=pair_ticket_lease(&cloud_ticket))<0) {
                fputs("CLOUD_CANCEL physical-send\n",stderr);rc=-3;break;
            }
            ssize_t n = send(channel[0], (const unsigned char *)clip + sent,
                             (size_t)samples * 2 - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
            int send_errno=errno;
            if(lease>=0) close(lease);
            errno=send_errno;
            if (n > 0) {
                sent += (size_t)n;
                if (sent == (size_t)samples * 2 && shutdown(channel[0], SHUT_WR)) break;
            } else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) break;
        }
        if (done && eof) {
            while (received && (response[received - 1] == '\n' || response[received - 1] == '\r')) received--;
            if (sent != (size_t)samples * 2 || received >= (size_t)cap ||
                memchr(response, '\n', received) || memchr(response, '\r', received)) break;
            /* Complete output and successful exit alone do not authorize a
             * late or preempted result. Recheck before publishing any text. */
            if (p05_cloud_interrupted(native,before_serial,before_m,&log,owned_dialog)) { rc = -3; break; }
            if (p05_cloud_elapsed(&start) >= WAKE_CLOUD_BUDGET_MS - grace_ms) { rc = -2; break; }
            memcpy(text, response, received); text[received] = 0;
            rc = received ? 1 : 0; break;
        }
        left = WAKE_CLOUD_BUDGET_MS - grace_ms - p05_cloud_elapsed(&start);
        if (left <= 0) { rc = -2; break; }
        struct pollfd pfd = {.fd = channel[0], .events = (short)(eof ? 0 : POLLIN)};
        if (sent < (size_t)samples * 2) pfd.events |= POLLOUT;
        /* EOF can precede process exit. Avoid spinning on a permanent HUP. */
        if (eof) pfd.fd = -1;
        int pr = poll(&pfd, 1, left > 20 ? 20 : left);
        if (pr < 0 && errno != EINTR) break;
    }
finish:
    for (int i = 0; i < 2; i++) if (channel[i] >= 0) close(channel[i]);
    if (pid > 0 && !done) {
        kill(pid, SIGTERM); /* existing helper sends C; grace is inside 800ms */
        int stop_at = p05_cloud_elapsed(&start) + grace_ms;
        if (stop_at > WAKE_CLOUD_BUDGET_MS) stop_at = WAKE_CLOUD_BUDGET_MS;
        for (;;) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid || (got < 0 && errno == ECHILD)) { done = 1; break; }
            if (p05_cloud_elapsed(&start) >= stop_at) break;
            poll(NULL, 0, 1);
        }
        if (!done) {
            kill(pid, SIGKILL);
            p05_cloud_pending = pid;
            p05_cloud_reap();
        }
    }
    if (rc < 0) text[0] = 0;
    if (elapsed_ms) *elapsed_ms = p05_cloud_elapsed(&start);
    return rc;
}
static int p05_receipt_pending(void) {
    return p05_ev.receipt_id && p05_ev.receipt_id==queued_receipt_id &&
           ((pending_after && pair_now_ms()-pending_ticket.stamp<3000) ||
            (pair_retry.samples && pair_now_ms()<=pair_retry.expires));
}
static void p05_flush_event(void) {
    char json[12000], escaped[200], spelled[160];
    int keep, mem, allow, n;
    char keep_path[512];
    if (!p05_ev.live || !p05_forensics_on()) { p05_ev.live = 0; p05_post_left = 0; return; }
    if(p05_ev.receipt_id && p05_ev.receipt_id!=published_receipt_id) {
        snprintf(p05_ev.result,sizeof(p05_ev.result),"unpaired");
        p05_ev.dispatched[0]=0;
    }
    mem = wake_mem_available_kib(getenv("LX06_WAKE_MEMINFO"));
    allow = p05_ev.audio && mem >= WAKE_FORENSICS_MEM_FLOOR_KIB && p05_capture_n > 0;
    p05_json_escape(p05_ev.text, escaped, sizeof(escaped));
    wake_pinyin_phrase(p05_ev.text, spelled, sizeof(spelled));
    p05_json_escape(spelled, p05_ev.pinyin, sizeof(p05_ev.pinyin));
    n = snprintf(json, sizeof(json),
        "{\"id\":\"%s\",\"uptime_s\":%.3f,\"wall\":%lld,\"result\":\"%s\",\"dispatched\":\"%s\","
        "\"policy\":\"%s\",\"raw_margin\":%.6f,\"norm_margin\":%.6f,\"delay_ms\":%d,\"local_near\":%s,"
        "\"audio\":%s,\"post\":\"%s\",\"pre_samples\":%d,\"post_samples\":%d,"
        "\"clip_start_sample\":%llu,\"phrase_fast\":%s,\"words\":[",
        p05_ev.id, p05_ev.uptime, p05_ev.wall, p05_ev.result, p05_ev.dispatched,
        p05_raw_policy ? "raw030" : WW_CALIBRATION_POLICY, p05_ev.raw_margin, p05_ev.norm_margin,
        p05_ev.delay_ms, p05_ev.local_near && !strcmp(p05_ev.result,"dispatch") ? "true" : "false", allow ? "true" : "false",
        !allow ? "unavailable" : p05_post_left > 0 ? "partial" : "full",
        allow ? p05_capture_pre : 0, allow ? p05_capture_n - p05_capture_pre : 0,
        allow ? (unsigned long long)p05_clip_start : 0ULL, p05_ev.phrase_fast ? "true" : "false");
    for (int i = 0; i < p05_ev.nwords && n > 0 && n < (int)sizeof(json); i++) {
        char trail[96];
        if (p05_ev.window[i] > 0)
            snprintf(trail, sizeof(trail), "[%.6f,%.6f,%.6f]", p05_ev.trail[i][0], p05_ev.trail[i][1], p05_ev.trail[i][2]);
        else snprintf(trail, sizeof(trail), "[null,null,null]");
        n += snprintf(json + n, sizeof(json) - (size_t)n,
            "%s{\"label\":\"%s\",\"score\":%.6f,\"gate\":%.6f,\"ratio\":%.6f,\"window\":%d,"
            "\"trail\":%s,\"trail_end_ms\":%d,\"trail_hop_ms\":%d,\"rms\":%.2f,\"sha256\":\"%s\"}",
            i ? "," : "", p05_ev.label[i], p05_ev.score[i], p05_ev.gate[i], p05_ev.ratio[i],
            p05_ev.window[i], trail, p05_ev.trail_end_ms[i], p05_ev.phrase_fast ? 20 : 80, p05_ev.rms[i], p05_ev.sha[i]);
    }
    if (n > 0 && n < (int)sizeof(json))
        snprintf(json + n, sizeof(json) - (size_t)n,
            "],\"cloud\":{\"enabled\":%s,\"sent\":%s,\"mode\":\"%s\",\"text\":\"%s\",\"pinyin\":\"%s\","
            "\"verdict\":\"%s\",\"elapsed_ms\":%d,\"winner_refs\":%d,\"winner_missing\":%d},"
            "\"label\":{\"should_be\":\"\",\"pinned\":false}}\n",
            p05_cloud_on() ? "true" : "false", p05_ev.sent ? "true" : "false", wake_cloud_send_mode(),
            escaped, p05_ev.pinyin, p05_ev.verdict, p05_ev.elapsed_ms,
            p05_ev.winner_refs, p05_ev.winner_missing);
    snprintf(keep_path, sizeof(keep_path), "%s/forensics-keep", p05_data_root());
    /* Read N under the same lock as web config/pinning and admission. */
    if (!wake_forensics_lock(p05_forensics_dir())) {
        keep = wake_forensics_keep(keep_path);
        wake_forensics_commit_pcm(p05_forensics_dir(), p05_ev.id, json,
                              allow ? p05_capture : NULL, allow ? p05_capture_n : 0, allow,
                              keep, WAKE_FORENSICS_MAX_BYTES, WAKE_FORENSICS_MAX_PINNED);
        wake_forensics_unlock();
    }
    p05_ev.live = 0;
    p05_post_left = 0;
}
struct p05_span { uint64_t first, end; };

static int p05_candidate_span(int samples, int end_ms, struct p05_span *span) {
    if (samples <= 0 || samples > MAX_UTT_SAMPLES || end_ms <= 0 ||
        end_ms % (1000 * BLOCK_SAMPLES / SAMPLE_RATE) || samples % BLOCK_SAMPLES) return 0;
    const uint64_t end = (uint64_t)end_ms * (SAMPLE_RATE / 1000);
    const unsigned width = (unsigned)samples + P05_CONFIRM_LEAD;
    if (end < width || end > p05_stream_end || end - width < p05_stream_end - p05_ring_have)
        return 0;
    span->first = end - width;
    span->end = end;
    return 1;
}

static unsigned p05_ring_index(uint64_t sample) {
    return (p05_ring_pos + P05_HISTORY_SAMPLES - (unsigned)(p05_stream_end - sample)) % P05_HISTORY_SAMPLES;
}
static int16_t *p05_scored_copy(int samples,int end_ms) {
    struct p05_span span;
    if(!p05_candidate_span(samples,end_ms,&span)) return NULL;
    unsigned start=p05_ring_index(span.end-(unsigned)samples);
    unsigned first=P05_HISTORY_SAMPLES-start;
    if(first>(unsigned)samples) first=(unsigned)samples;
    int16_t *clip=malloc((size_t)samples*sizeof(*clip));
    if(!clip) return NULL;
    memcpy(clip,p05_ring+start,first*sizeof(*clip));
    memcpy(clip+first,p05_ring,(samples-first)*sizeof(*clip));
    return clip;
}
/* Only the original scored samples may be tried again. Two new consecutive
 * physical packets, with unchanged source and idle epoch, must supply the
 * missing edge. Reapply every waveform, clock and ownership check once. */
static void pair_retry_finish(void) {
    int samples=pair_retry.samples;
    pair_retry.samples=0;
    int16_t *clip=p05_scored_copy(samples,pair_retry.end_ms);
    if(!clip) return;
    struct pair_ticket ticket;
    int ok=pair_prepare(&ticket,clip,samples,pair_retry.end_ms);
    free(clip);
    int lease=ok ? pair_ticket_lease(&ticket) : -1;
    if(lease<0) return;
    ticket.local_near=pair_retry.local_near;
    ticket.rival_score=pair_retry.rival_score;
    pair_publish(&ticket,pair_retry.word,pair_retry.score);
    published_receipt_id=queued_receipt_id;
    close(lease);
}
static int p05_emit_paired(const char *label,float score,int samples,int end_ms,int local_near,float rival_score,
                           const struct pair_ticket *prepared) {
    if(!paired) return 0;
    if(cloud_attempted) return pair_cloud_finish(label,score,samples,end_ms);
    struct pair_ticket ticket;
    int ok;
    if (prepared) {
        ticket = *prepared;
        if (!pair_ticket_valid(&ticket)) return 0;
        ok = 1;
    } else {
        int16_t *clip=p05_scored_copy(samples,end_ms);
        if(!clip) return 0;
        ok=pair_prepare(&ticket,clip,samples,end_ms);
        free(clip);
    }
    if(ok) {
        int lease=local_near ? pair_ticket_lease(&ticket) : -1;
        if(local_near && lease<0) return 0;
        ticket.local_near=local_near;
        ticket.rival_score=rival_score;
        pair_publish(&ticket,label,score);
        if(lease>=0) close(lease);
    }
    else if(ticket.wait_for_tail && paired->next<=UINT64_MAX-2*BLOCK_SAMPLES) {
        pair_retry.samples=samples;pair_retry.end_ms=end_ms;
        pair_retry.through=paired->next+2*BLOCK_SAMPLES;
        pair_retry.expires=pair_now_ms()+120;
        pair_retry.score=score;
        pair_retry.local_near=local_near;
        pair_retry.rival_score=rival_score;
        snprintf(pair_retry.word,sizeof(pair_retry.word),"%s",label);
        ++queued_receipt_id;
        return 1;
    }
    return ok;
}

/* Send exactly the winning scoring window, even after the arbitration wait. */
static int p05_verify_scored_window(int samples, int end_ms, char *heard, size_t cap, int *elapsed) {
    cloud_attempted=paired!=NULL;
    int16_t *clip=p05_scored_copy(samples,end_ms);
    if(!clip) return -1;
    int rc=-1,first=0;
    if(paired) {
        if(!pair_prepare(&cloud_ticket,clip,samples,end_ms)) goto done;
        int lease=pair_ticket_lease(&cloud_ticket);
        if(lease<0) {memset(&cloud_ticket,0,sizeof(cloud_ticket));goto done;}
        unsigned at=(unsigned)(cloud_ticket.first-paired->first);
        for(int j=0;j<samples;j++,at++) clip[j]=paired->pcm[(paired->start+at/BLOCK_SAMPLES)%PAIR_BLOCKS][at%BLOCK_SAMPLES];
        close(lease);
    }
    int n=wake_cloud_scored_clip(clip,samples,&first);
    if(n<0 || wake_cloud_clip_samples(n,0)<0) {rc=-4;goto done;}
    rc=p05_verify_clip(clip+first,n,heard,cap,elapsed);
    if(paired && rc>=0) {
        int lease=pair_ticket_lease(&cloud_ticket);
        if(lease<0) {fputs("CLOUD_CANCEL physical-result\n",stderr);heard[0]=0;rc=-3;} else close(lease);
    }
done:
    free(clip);
    return rc;
}

/* Dedupe only the stored near-miss event. The latest voiced episode in
 * its scored audio, with at least 60 ms of voice, is independent of when
 * the decision was published. Later speech beyond the window is irrelevant. */
static unsigned p05_span_episode(const struct p05_span *span) {
    unsigned episode = 0, voiced = 0;
    for (uint64_t sample = span->end - BLOCK_SAMPLES; ; sample -= BLOCK_SAMPLES) {
        unsigned id = p05_voiced[p05_ring_index(sample) / BLOCK_SAMPLES];
        if (id) {
            if (episode != id) { episode = id; voiced = 0; }
            if (++voiced >= 3) return episode;
        }
        if (sample - span->first < BLOCK_SAMPLES) return 0;
    }
}
static int p05_duplicate_near(const struct p05_span *span) {
    unsigned episode=p05_span_episode(span);
    if(!episode) return 0;
    for (unsigned i = 0; i < P05_NEAR_EPISODES; ++i)
        if (p05_near_seen[i] == episode) return 1;
    p05_near_seen[p05_near_next] = episode;
    p05_near_next = (p05_near_next + 1) % P05_NEAR_EPISODES;
    return 0;
}

static void p05_begin_capture(const struct p05_span *span) {
    uint64_t earliest = p05_stream_end - p05_ring_have;
    uint64_t available_lead = span->first - earliest;
    unsigned lead = PRE_BLOCKS * BLOCK_SAMPLES;
    p05_clip_start = available_lead < lead ? earliest : span->first - lead;
    uint64_t limit = span->end + 2 * SAMPLE_RATE;
    if (limit - p05_clip_start > P05_MAX_CAPTURE) limit = p05_clip_start + P05_MAX_CAPTURE;
    uint64_t copy_end = limit < p05_stream_end ? limit : p05_stream_end;
    p05_capture_pre = (int)(span->end - p05_clip_start);
    p05_capture_n = (int)(copy_end - p05_clip_start);
    p05_post_left = (int)(limit - copy_end);
    unsigned start = p05_ring_index(p05_clip_start);
    unsigned first = P05_HISTORY_SAMPLES - start;
    if (first > (unsigned)p05_capture_n) first = (unsigned)p05_capture_n;
    memcpy(p05_capture, p05_ring + start, first * sizeof(*p05_capture));
    memcpy(p05_capture + first, p05_ring, ((unsigned)p05_capture_n - first) * sizeof(*p05_capture));
    p05_ev.audio = 1;
}
static int p05_normal_confirmations(int samples,int end_ms) {
    struct p05_span span;
    if(!p05_candidate_span(samples,end_ms,&span)) return 0;
    for(unsigned j=0;j<3;j++) {
        uint64_t first=span.first+j*WW_SCORE_HOP_BLOCKS*BLOCK_SAMPLES;
        unsigned pos=p05_ring_index(first);
        int64_t power=0;
        for(int n=0;n<samples;n++) {
            int x=p05_ring[pos];
            power+=(int64_t)x*x;
            if(++pos==P05_HISTORY_SAMPLES) pos=0;
        }
        if(power<=(int64_t)samples*(int)(WW_NORMAL_RMS*WW_NORMAL_RMS)) return 0;
    }
    return 1;
}
static int p05_wide_candidate(float score,float ratio) {
    return ratio>1.f+WAKE_NORM_BAND && ratio<=WAKE_LOCAL_STRONG_SEPARATION_RATIO &&
           score<=WAKE_LOCAL_STRONG_SEPARATION_CEILING;
}
static void p05_decide_with_pair(Model *models, int count, const float *seen, const float *rms, const int *wins,
                       const float snapshots[][3], const int *trail_end_ms,
                       const int *weak, const float *gates, int used,
                       int blocks, int *cooldown, int *used_io, const struct pair_ticket *prepared) {
    cloud_attempted=0;
    memset(&cloud_ticket,0,sizeof(cloud_ticket));
    int cloud_on=p05_cloud_on();
    int best = p05_raw_policy ? wake_candidate_select_raw030(seen, gates, count)
                              : wake_candidate_select(seen, gates, count);
    int local_best = best, nearest = -1;
    int passers = 0, banded = 0, competitor = 0, gray = 0;
    float raw1 = 99.f, raw2 = 99.f, win_ratio = 99.f, second_ratio = 99.f;
    char heard[160] = "", label[48];
    struct wake_cloud_word words[MAX_MODELS];
    struct wake_cloud_request request;
    enum wake_cloud_outcome outcome = WAKE_CLOUD_LOCAL;
    int asr_rc = 0, elapsed = 0;
    const char *result = "near_miss";
    struct p05_event next = {0};
    next.nwords = count;
    next.phrase_fast=prepared && prepared->phrase_fast;
    memset(words, 0, sizeof(words));
    for (int i = 0; i < count; i++) {
        float ratio = gates[i] > 0.f ? seen[i] / gates[i] : 99.f;
        snprintf(next.label[i], sizeof(next.label[i]), "%s", models[i].label);
        snprintf(next.sha[i], sizeof(next.sha[i]), "%s", p05_sha[i]);
        next.score[i] = seen[i]; next.gate[i] = gates[i]; next.ratio[i] = ratio;
        next.rms[i] = rms[i]; next.window[i] = wins[i];
        memcpy(next.trail[i], snapshots[i], sizeof(next.trail[i]));
        next.trail_end_ms[i] = trail_end_ms[i];
        snprintf(words[i].label, sizeof(words[i].label), "%s", models[i].label);
        words[i].ratio = ratio;
        if (gates[i] > 0.f && ratio <= 1.f) {
            passers++;
            if (seen[i] < raw1) { raw2 = raw1; raw1 = seen[i]; }
            else if (seen[i] < raw2) raw2 = seen[i];
        }
        if (ratio <= 1.f + WAKE_NORM_BAND ||
            (paired && !p05_raw_policy && !cloud_on && p05_wide_candidate(seen[i],ratio))) {
            banded++;
            if (nearest < 0 || ratio < next.ratio[nearest]) nearest = i;
        }
    }
    /* With verification off, choose the lowest normalized score among the
     * words that individually passed. Equal ratios retain configured order. */
    if(!p05_raw_policy && !cloud_on && passers) best=nearest;
    int local_near=0;
    float near_rival=0;
    if(paired && !p05_raw_policy && !cloud_on && !passers && nearest>=0) {
        for(int i=0;i<count;i++) {
            int wide=p05_wide_candidate(seen[i],next.ratio[i]);
            if((next.ratio[i]>1.f+WAKE_NORM_BAND && !wide) || seen[i]>WAKE_LOCAL_SEPARATED_CEILING ||
               (best>=0 && next.ratio[i]>=next.ratio[best]) || rms[i]<=WW_NORMAL_RMS) continue;
            float rival=0;
            if(wide || seen[i]>WAKE_LOCAL_NEAR_CEILING) {
                if(count<2) continue;
                rival=99.f;
                for(int j=0;j<count;j++) if(j!=i) {
                    int end=0;
                    const struct p05_observation *o=p05_rival_window(j,trail_end_ms[i],wins[i],&end);
                    if(!o || gates[j]<=0.f || o->score/gates[j]<next.ratio[i]) {rival=0;break;}
                    rival=fminf(rival,o->score);
                }
                float margin=wide ? WAKE_LOCAL_STRONG_SEPARATION_MARGIN : WAKE_LOCAL_SEPARATED_MARGIN;
                if(rival-seen[i]<margin) continue;
            }
            /* seen is the maximum of three consecutive same-duration scores.
             * Verify the source audio of all three before a local near claim. */
            if(p05_normal_confirmations(wins[i],trail_end_ms[i])) {best=i;near_rival=rival;}
        }
        next.local_near=local_near=best>=0;
        if(best>=0 && near_rival>0) for(int j=0;j<count;j++) if(j!=best) {
            int end=0;
            const struct p05_observation *o=p05_rival_window(j,trail_end_ms[best],wins[best],&end);
            next.score[j]=o->score;next.ratio[j]=o->score/gates[j];next.rms[j]=o->rms;
            next.window[j]=o->width;next.trail_end_ms[j]=end;
            memcpy(next.trail[j],o->trail,sizeof(o->trail));
        }
    }
    int scored_word = best >= 0 ? best : passers && !p05_raw_policy ? nearest : -1;
    if (scored_word >= 0) {
        win_ratio = next.ratio[scored_word];
        for (int i = 0; i < count; i++) if (i != scored_word) {
            if (next.ratio[i] < second_ratio) second_ratio = next.ratio[i];
            if (next.ratio[i] <= 1.f + WAKE_NORM_BAND) competitor = 1;
        }
        gray = best < 0 || competitor || gates[scored_word] > WAKE_AUTO_GATE_CEILING ||
               (second_ratio < 90.f && second_ratio - win_ratio < WAKE_GRAY_MARGIN);
        result = best >= 0 ? "dispatch" : "ambiguous";
    } else if (passers > 0) result = "ambiguous";
    else {
        int any_weak = 0;
        for (int i = 0; weak && i < count; i++) if (weak[i]) any_weak = 1;
        /* A weak observation in a different window must not hide a normal
         * level, independently confirmed candidate in the near band. */
        if (banded && nearest >= 0 && rms[nearest] > WW_NORMAL_RMS) result = "near_miss";
        else if (any_weak) result = "weak";
        else if (banded) result = "near_miss";
        else return;
    }
    int rescue = paired && cloud_on && !p05_raw_policy && !passers && nearest >= 0 &&
                 !strcmp(result,"near_miss") && next.ratio[nearest] <= 1.f+WAKE_NORM_BAND &&
                 seen[nearest] <= WAKE_CLOUD_NEAR_CEILING;
    if(rescue) {
        scored_word=nearest;
        win_ratio=next.ratio[nearest];
        gray=1;
    }
    next.raw_margin = raw2 < 90.f ? raw2 - raw1 : 99.f;
    next.norm_margin = second_ratio < 90.f ? second_ratio - win_ratio : 99.f;
    snprintf(next.verdict, sizeof(next.verdict), "%s",
             best < 0 && !strcmp(result, "near_miss") && cloud_on ? "not_eligible" : "off");
    if (scored_word >= 0 && !p05_raw_policy) {
        int samples = wins[scored_word];
        memset(&request, 0, sizeof(request));
        request.enabled = cloud_on;
        request.gray = gray;
        request.clip_samples = samples;
        request.winner_ratio = win_ratio;
        if(request.enabled && request.gray) p05_official(&request.official);
        if (!request.enabled) snprintf(next.verdict, sizeof(next.verdict), "off");
        else if (!request.gray) snprintf(next.verdict, sizeof(next.verdict), "not_gray");
        else if (wake_official_busy(&request.official))
            snprintf(next.verdict, sizeof(next.verdict), "official");
        else if (samples > 0 && used >= samples) {
            asr_rc = p05_verify_scored_window(samples, trail_end_ms[scored_word], heard, sizeof(heard), &elapsed);
            next.sent = asr_rc != -1 && asr_rc != -4;
            next.elapsed_ms = elapsed;
            next.delay_ms = elapsed;
            snprintf(next.text, sizeof(next.text), "%s", heard);
            /* Sample pinyin is the only reference. The model label is a name.
             * Loaded only after the switch, gray zone and official-idle checks
             * have already allowed this one burst. */
            if (asr_rc == 1) {
                for (int i = 0; i < count; i++) {
                    char recdir[320];
                    if (strchr(words[i].label, '/') || strstr(words[i].label, "..")) continue;
                    snprintf(recdir, sizeof(recdir), "%s/recs/%s", p05_data_root(), words[i].label);
                    wake_samples_load(recdir, &words[i]);
                }
            }
            next.winner_refs = words[scored_word].nsamples;
            next.winner_missing = words[scored_word].missing;
            outcome = wake_cloud_classify(asr_rc, heard, models[scored_word].label, words, count, label, sizeof(label));
            if (asr_rc == -4) snprintf(next.verdict, sizeof(next.verdict), "too_long");
            else if (asr_rc == -3) snprintf(next.verdict, sizeof(next.verdict), "cancelled");
            else if (asr_rc < 0) snprintf(next.verdict, sizeof(next.verdict), asr_rc == -2 ? "timeout" : "unavailable");
            else if (outcome == WAKE_CLOUD_DISPATCH) {
                best = scored_word;
                cloud_ticket.cloud_rescued=rescue;
                result = "dispatch";
                snprintf(next.verdict, sizeof(next.verdict), "match");
            }
            else if (rescue) {
                snprintf(next.verdict,sizeof(next.verdict),"%s", outcome==WAKE_CLOUD_LOCAL ? "unconfirmed" : "discard");
                if (outcome != WAKE_CLOUD_LOCAL) result = "discard";
            } else if (outcome == WAKE_CLOUD_REASSIGN) {
                best = -1;
                for (int i = 0; i < count; i++) {
                    if (!strcmp(models[i].label, label) && (!paired || next.ratio[i] <= 1.f))
                        best = i;
                }
                result = best >= 0 ? "reassign" : "discard";
                snprintf(next.verdict, sizeof(next.verdict), "%s", result);
            } else if (outcome == WAKE_CLOUD_DISCARD) {
                snprintf(next.verdict, sizeof(next.verdict), "discard");
                result = "discard"; best = -1;
            } else snprintf(next.verdict, sizeof(next.verdict), "fallback");
        }
    }
    snprintf(next.result, sizeof(next.result), "%s", result);
    if (best >= 0) snprintf(next.dispatched, sizeof(next.dispatched), "%s", models[best].label);
    if (best >= 0) {
        int receipt=p05_emit_paired(models[best].label,seen[best],wins[scored_word],trail_end_ms[scored_word],local_near,near_rival,prepared);
        if(!paired && !receipt)
            emit_wake(models[best].label, seen[best], wins[scored_word], trail_end_ms[scored_word]);
        if(paired) {
            if(!receipt || pending_after || pair_retry.samples) {
                result=receipt ? "pending" : "unpaired";
                if(receipt) next.receipt_id=queued_receipt_id;
                else {
                    snprintf(next.result,sizeof(next.result),"%s",result);
                    next.dispatched[0]=0;
                }
            }
            fprintf(stderr,"DETECTED %s score=%.6f end_ms=%d\n",models[best].label,seen[best],blocks*20);
        } else fprintf(stderr, "HIT %s score=%.6f end_ms=%d\n", models[best].label, seen[best], blocks * 20);
        if (next.sent && (outcome == WAKE_CLOUD_DISPATCH || outcome == WAKE_CLOUD_REASSIGN)) {
            time_t now = time(NULL);
            struct tm local;
            char stamp[20];
            if (localtime_r(&now, &local) && strftime(stamp, sizeof(stamp), "%F %T", &local))
                fprintf(stderr, "%s CLOUD-OK label=%s\n", stamp, models[best].label);
        }
        *cooldown = 75; *used_io = 0;
    }
    printf("DECISION %s %s\n", result, best >= 0 ? models[best].label : "-");
    fflush(stdout);
    if (!p05_forensics_on()) { if (p05_ev.live) p05_flush_event(); return; }
    int word = scored_word >= 0 ? scored_word : local_best >= 0 ? local_best : nearest;
    struct p05_span span;
    int has_span = word >= 0 && p05_candidate_span(wins[word], trail_end_ms[word], &span);
    int near_evidence = !strcmp(result, "near_miss") || (rescue && !strcmp(result, "discard"));
    if (near_evidence) {
        if (!has_span) return;
        /* The relative band also collects candidates for comparing words.
         * Store a near miss only when its raw distance can enter a rescue path. */
        if (paired && !p05_raw_policy &&
            seen[word] > (cloud_on ? WAKE_CLOUD_NEAR_CEILING : WAKE_LOCAL_SEPARATED_CEILING)) return;
        if (p05_duplicate_near(&span)) return;
    }
    /* A previous partial capture may need disk I/O. Dispatch this event first. */
    if (p05_ev.live) {
        int same=0;
        if(best>=0 && has_span && p05_ev.near_episode &&
           p05_span_episode(&span)==p05_ev.near_episode) {
            uint64_t first=span.first>p05_ev.near_first ? span.first : p05_ev.near_first;
            uint64_t end=span.end<p05_ev.near_end ? span.end : p05_ev.near_end;
            uint64_t width=span.end-span.first, prev=p05_ev.near_end-p05_ev.near_first;
            same=end>first && 2*(end-first)>=(width<prev ? width : prev);
        }
        if(same) p05_post_left=0;
        else p05_flush_event();
    }
    if(near_evidence) {
        next.near_episode=p05_span_episode(&span);
        next.near_first=span.first;
        next.near_end=span.end;
    }
    p05_ev = next;
    {
        FILE *up = fopen("/proc/uptime", "r");
        if (up) { if (fscanf(up, "%lf", &p05_ev.uptime) != 1) p05_ev.uptime = 0; fclose(up); }
    }
    p05_ev.wall = (long long)time(NULL);
    snprintf(p05_ev.id, sizeof(p05_ev.id), "%lld-%u", p05_ev.wall, ++p05_seq);
    {
        int mem = wake_mem_available_kib(getenv("LX06_WAKE_MEMINFO"));
        p05_ev.live = 1;
        if (has_span && mem >= WAKE_FORENSICS_MEM_FLOOR_KIB) p05_begin_capture(&span);
        else if(!p05_receipt_pending()) p05_flush_event();
    }
}

static void p05_decide(Model *models, int count, const float *seen, const float *rms, const int *wins,
                       const float snapshots[][3], const int *trail_end_ms,
                       const int *weak, const float *gates, int used,
                       int blocks, int *cooldown, int *used_io) {
    p05_decide_with_pair(models,count,seen,rms,wins,snapshots,trail_end_ms,weak,gates,used,blocks,cooldown,used_io,NULL);
}

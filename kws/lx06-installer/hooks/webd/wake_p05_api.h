/* P0.5 web88 block. Included from webd.c. Do not mix with WT1 route edits.
 * Forensics live in tmpfs. Cloud verify defaults off and is stored on device. */
#include "../wakeword/wake_pinyin.c"
#include "../wakeword/wake_forensics.c"
#include "../wakeword/wake_cloud_verify.c"
#include "../wakeword/wake_sample.c"
#include <dirent.h>
#include <poll.h>
#include <sys/wait.h>

#ifndef WW_STORE_PATH
#define WW_STORE_PATH(suffix) WAKE_DATA_DIR suffix
#define WW_JOINT_ROOT "/tmp/lx06-wake-joint"
#endif
static const char *wake_p05_root(void) {
    const char *root = getenv("LX06_WAKE_DATA");
    return root && *root ? root : WW_STORE_PATH("");
}
static const char *wake_p05_dir(void) {
    const char *dir = getenv("LX06_WAKE_FORENSICS_DIR");
#ifdef WW_RAW_FORENSICS
    if (!dir && ww_epoch_capture_enabled()) return WW_RAW_FORENSICS;
#endif
    return dir && *dir ? dir : WAKE_FORENSICS_DIR;
}
static int wake_p05_query(const char *q, const char *key, char *out, size_t cap) {
    char pat[40];
    const char *p;
    size_t n = 0;
    snprintf(pat, sizeof(pat), "%s=", key);
    p = q ? strstr(q, pat) : NULL;
    if (!p || cap < 2) return -1;
    p += strlen(pat);
    while (*p && *p != '&' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return n ? 0 : -1;
}
static int wake_p05_cloud_on(void) {
    char value[16];
    const char *env = getenv("LX06_WAKE_CLOUD_VERIFY");
    if (env) return !strcmp(env, "on") || !strcmp(env, "1");
    if (wake_settings_read(wake_p05_root(), "cloud-verify", value, sizeof(value)) != 0) return 0;
    return !strcmp(value, "on");
}
static void wake_p05_list(int fd) {
    DIR *dir;
    struct dirent *entry;
    char *json = malloc(262144);
    int n, enabled, keep;
    char path[512];
    if (!json) { app_http_error(fd, 500, "内存不足"); return; }
    if (wake_forensics_lock(wake_p05_dir())) { free(json); app_http_error(fd, 500, "取证锁不可用"); return; }
    struct forensic_inventory inventory;
    if (forensics_inventory(wake_p05_dir(), &inventory)) {
        wake_forensics_unlock(); free(json); app_http_error(fd, 500, "无法核对取证容量"); return;
    }
    dir = opendir(wake_p05_dir());
    snprintf(path, sizeof(path), "%s/forensics-enabled", wake_p05_root());
    enabled = wake_forensics_enabled(path);
    snprintf(path, sizeof(path), "%s/forensics-keep", wake_p05_root());
    keep = wake_forensics_keep(path);
    n = snprintf(json, 262144, "{\"ok\":true,\"enabled\":%d,\"keep\":%d,\"cloud\":%d,"
                 "\"count\":%d,\"pinned\":%d,\"bytes\":%lld,\"max_bytes\":%d,\"dropped\":%u,\"events\":[",
                 enabled, keep, wake_p05_cloud_on(), inventory.count, inventory.pinned,
                 (long long)inventory.bytes, WAKE_FORENSICS_MAX_BYTES, forensics_dropped(wake_p05_dir()));
    if (dir) {
        int first = 1;
        while ((entry = readdir(dir)) && n < 250000) {
            size_t len = strlen(entry->d_name);
            char id[64], file[512], body[8192];
            FILE *in;
            if (len < 6 || len > 53 || strcmp(entry->d_name + len - 5, ".json")) continue;
            memcpy(id, entry->d_name, len - 5); id[len - 5] = 0;
            if (!wake_id_ok(id)) continue;
            snprintf(file, sizeof(file), "%s/%s", wake_p05_dir(), entry->d_name);
            in = fopen(file, "r");
            if (!in) continue;
            len = fread(body, 1, sizeof(body) - 1, in); fclose(in); body[len] = 0;
            n += snprintf(json + n, 262144 - (size_t)n, "%s%s", first ? "" : ",", body);
            first = 0;
        }
        closedir(dir);
    }
    if (n > 0 && n < 262100) snprintf(json + n, 262144 - (size_t)n, "]}");
    wake_forensics_unlock();
    send_json(fd, json);
    free(json);
}
static void wake_p05_tar_header(unsigned char *hdr, const char *name, unsigned size) {
    unsigned sum = 0;
    memset(hdr, 0, 512);
    snprintf((char *)hdr, 100, "%s", name);
    memcpy(hdr + 100, "0000644", 7);
    memcpy(hdr + 108, "0000000", 7);
    memcpy(hdr + 116, "0000000", 7);
    snprintf((char *)hdr + 124, 12, "%011o", size);
    memcpy(hdr + 136, "00000000000", 11);
    memset(hdr + 148, ' ', 8);
    hdr[156] = '0';
    memcpy(hdr + 257, "ustar", 5);
    hdr[262] = '0'; hdr[263] = '0';
    for (int i = 0; i < 512; i++) sum += hdr[i];
    snprintf((char *)hdr + 148, 8, "%06o", sum);
    hdr[154] = '\0'; hdr[155] = ' ';
}
/* Stream bounded files; no multi-MiB web worker allocation. Only event JSON
 * and WAV are exported; lock/stage/unrelated files are never tar members. */
static int wake_p05_write(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) { ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}
static void wake_p05_export(int fd, const char *q) {
    char selected[64] = "";
    if (q && *q && (wake_p05_query(q, "id", selected, sizeof(selected)) || !wake_id_ok(selected))) {
        app_http_error(fd, 400, "编号无效"); return;
    }
    if(selected[0]) {
        char path[512];struct stat st;snprintf(path,sizeof(path),"%s/%s.json",wake_p05_dir(),selected);
        int in=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(in<0 || fstat(in,&st) || !S_ISREG(st.st_mode)) {if(in>=0)close(in);app_http_error(fd,404,"记录不存在");return;}
        close(in);
    }
    DIR *dir = opendir(wake_p05_dir()); struct dirent *entry;
    const char *hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/x-tar\r\n"
        "Content-Disposition: attachment; filename=\"lx06-wake-forensics.tar\"\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    unsigned char block[4096] = {0}; size_t total = 0;
    if (wake_p05_write(fd, hdr, strlen(hdr))) { if (dir) closedir(dir); return; }
    while (dir && (entry = readdir(dir))) {
        char id[64]; size_t len = strlen(entry->d_name), ext = 0;
        if (len > 5 && !strcmp(entry->d_name + len - 5, ".json")) ext = 5;
        if (len > 4 && !strcmp(entry->d_name + len - 4, ".wav")) ext = 4;
        if (!ext || len - ext >= sizeof(id)) continue;
        memcpy(id, entry->d_name, len - ext); id[len-ext] = 0;
        if (!wake_id_ok(id) || (selected[0] && strcmp(id, selected))) continue;
        int in = openat(dirfd(dir), entry->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        if (in < 0) continue;
        if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 200000 ||
            total + 512 + ((st.st_size+511)/512)*512 > WAKE_FORENSICS_MAX_BYTES + 32768) { close(in); continue; }
        wake_p05_tar_header(block, entry->d_name, (unsigned)st.st_size);
        if (wake_p05_write(fd, block, 512)) { close(in); break; }
        off_t left = st.st_size;
        while (left > 0) {
            ssize_t n = read(in, block, left > 4096 ? 4096 : (size_t)left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || wake_p05_write(fd, block, (size_t)n)) break;
            left -= n;
        }
        close(in);
        if (left) break;
        size_t padding = (512 - (size_t)st.st_size % 512) % 512;
        memset(block, 0, sizeof(block));
        if (wake_p05_write(fd, block, padding)) break;
        total += 512 + (size_t)st.st_size + padding;
    }
    if (dir) closedir(dir);
    memset(block, 0, 1024); (void)wake_p05_write(fd, block, 1024);
}
static void wake_p05_audio(int fd, const char *q) {
    char id[64], path[512];
    int file;
    struct stat st;
    if (wake_p05_query(q, "id", id, sizeof(id)) != 0 || !wake_id_ok(id)) {
        app_http_error(fd, 400, "编号无效"); return;
    }
    snprintf(path, sizeof(path), "%s/%s.wav", wake_p05_dir(), id);
    file = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0 || fstat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 200000) {
        if (file >= 0) close(file);
        app_http_error(fd, 404, "没有这段音频"); return;
    }
    {
        char hdr[160];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %ld\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n", (long)st.st_size);
        (void)write(fd, hdr, (size_t)hl);
        for (;;) {
            char chunk[4096];
            ssize_t r = read(file, chunk, sizeof(chunk));
            if (r <= 0) break;
            if (write(fd, chunk, (size_t)r) != r) break;
        }
        close(file);
    }
}
static int wake_p05_allowed_label(const char *label) {
    ww_word_t words[WW_MAXWORDS];
    int n, i;
    if (strchr(label, '"') || strchr(label, '\\') || strchr(label, '\n')) return 0;
    if (!label[0] || !strcmp(label, "不是唤醒词") || !strcmp(label, "漏唤醒")) return 1;
    n = ww_read_conf(words, WW_MAXWORDS);
    for (i = 0; i < n; i++) if (!strcmp(words[i].name, label)) return 1;
    return 0;
}
static void wake_p05_label_unlocked(int fd, const char *body) {
    char id[64], should[64], pin[8], sample[8], path[512], raw[8192], out[8192];
    char *mark;
    FILE *file;
    int pinned, n;
    if (app_json_str(body, "id", id, sizeof(id)) != 0 || !wake_id_ok(id) ||
        app_json_str(body, "should_be", should, sizeof(should)) != 0 ||
        !wake_p05_allowed_label(should)) {
        app_http_error(fd, 400, "标注无效"); return;
    }
    pin[0] = 0;
    app_json_str(body, "pin", pin, sizeof(pin));
    sample[0] = 0; app_json_str(body, "cross_sample", sample, sizeof(sample));
    pinned = pin[0] == '1' || !strcmp(pin, "true");
    snprintf(path, sizeof(path), "%s/%s.json", wake_p05_dir(), id);
    int rawfd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    file = rawfd < 0 ? NULL : fdopen(rawfd, "r");
    if (!file) { app_http_error(fd, 404, "记录不存在"); return; }
    n = (int)fread(raw, 1, sizeof(raw) - 1, file);
    int too_big = !feof(file); fclose(file); raw[n] = 0;
    if (too_big) { app_http_error(fd, 413, "记录过大"); return; }
    if (!pin[0]) pinned = pinned_json(raw);
    int cross_sample = sample[0] ? sample[0] == '1' : strstr(raw, "\"cross_sample\":true") != NULL;
    if (cross_sample) {
        snprintf(path, sizeof(path), "%s/%s.wav", wake_p05_dir(), id);
        struct stat st;
        if (lstat(path, &st) || !S_ISREG(st.st_mode)) { app_http_error(fd, 409, "没有音频，无法加入交叉样本"); return; }
        if (!should[0] || !strcmp(should, "漏唤醒")) { app_http_error(fd, 400, "请先标注实际说的词或不是唤醒词"); return; }
        pinned = 1;
    }
    if (pinned && !pinned_json(raw)) {
        DIR *d = opendir(wake_p05_dir()); struct dirent *e; int pins = 0;
        while (d && (e = readdir(d))) {
            char buf[8192]; size_t l = strlen(e->d_name);
            if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
            int in = openat(dirfd(d), e->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            if (in < 0) continue;
            ssize_t r = read(in, buf, sizeof(buf)-1); close(in);
            if (r > 0) { buf[r] = 0; if (pinned_json(buf)) pins++; }
        }
        if (d) closedir(d);
        if (pins >= WAKE_FORENSICS_MAX_PINNED) { app_http_error(fd, 409, "最多钉住10条，请先取消一条"); return; }
    }
    mark = strstr(raw, "\"label\":{");
    if (!mark) { app_http_error(fd, 500, "记录格式无法标注"); return; }
    *mark = 0;
    int outn = snprintf(out, sizeof(out), "%s\"label\":{\"should_be\":\"%s\",\"pinned\":%s,\"cross_sample\":%s,\"purpose\":\"diagnostic\"}}\n",
             raw, should, pinned ? "true" : "false", cross_sample ? "true" : "false");
    if(outn < 0 || (size_t)outn >= sizeof(out)){app_http_error(fd,413,"标注后记录过大");return;}
    char keep_path[512];
    snprintf(keep_path, sizeof(keep_path), "%s/forensics-keep", wake_p05_root());
    int commit = wake_forensics_update_json(wake_p05_dir(), id, out, wake_forensics_keep(keep_path),
                                           WAKE_FORENSICS_MAX_BYTES, WAKE_FORENSICS_MAX_PINNED);
    if (commit == 2) { app_http_error(fd, 409, "钉住或容量已满，请先解除钉住或导出清理"); return; }
    if (commit < 0) {
        app_http_error(fd, 500, "标注没有写入"); return;
    }
    send_json(fd, "{\"ok\":true}");
}
static void wake_p05_label(int fd,const char *body) {
    if(wake_forensics_lock(wake_p05_dir())){app_http_error(fd,500,"取证锁不可用");return;}
    wake_p05_label_unlocked(fd,body);wake_forensics_unlock();
}
static void wake_p05_clear_unlocked(int fd) {
    DIR *dir = opendir(wake_p05_dir());
    struct dirent *entry;
    if (!dir && errno != ENOENT) { app_http_error(fd, 500, "无法读取诊断目录"); return; }
    int failed = 0;
    while (dir && (entry = readdir(dir))) {
        char id[64];
        size_t len = strlen(entry->d_name);
        const char *suffix = NULL;
        if (len > 5 && !strcmp(entry->d_name + len - 5, ".json")) suffix = ".json";
        if (len > 4 && !strcmp(entry->d_name + len - 4, ".wav")) suffix = ".wav";
        if (!suffix || len - strlen(suffix) >= sizeof(id)) continue;
        memcpy(id, entry->d_name, len - strlen(suffix));
        id[len - strlen(suffix)] = 0;
        if (!wake_id_ok(id)) continue;
        if (unlinkat(dirfd(dir), entry->d_name, 0) && errno != ENOENT) failed = 1;
    }
    if (dir) {
        if (unlinkat(dirfd(dir), ".dropped", 0) && errno != ENOENT) failed = 1;
        closedir(dir);
    }
    if (failed || ww_clear_recent_diagnostics() != 0) {
        app_http_error(fd, 500, "诊断记录未能全部清空");
        return;
    }
    send_json(fd, "{\"ok\":true}");
}
static void wake_p05_clear(int fd) {
    if(wake_forensics_lock(wake_p05_dir())){app_http_error(fd,500,"取证锁不可用");return;}
    wake_p05_clear_unlocked(fd);wake_forensics_unlock();
}
static void wake_p05_config_unlocked(int fd, const char *body) {
    char enabled[8], keep[8], value[16];
    enabled[0] = keep[0] = 0;
    int enabled_rc = app_json_str(body, "enabled", enabled, sizeof(enabled));
    int keep_rc = app_json_str(body, "keep", keep, sizeof(keep));
    if ((enabled_rc && strstr(body, "\"enabled\"")) || (keep_rc && strstr(body, "\"keep\""))) {
        app_http_error(fd, 400, "取证设置格式无效"); return;
    }
    int requested = 0;
    struct forensic_inventory inventory;
    if (keep[0]) {
        char *end; long n = strtol(keep, &end, 10);
        if (*end || n < WAKE_FORENSICS_KEEP_MIN || n > WAKE_FORENSICS_KEEP_MAX) {
            app_http_error(fd, 400, "保留条数须在 5 到 30"); return;
        }
        if (forensics_inventory(wake_p05_dir(), &inventory)) { app_http_error(fd, 500, "无法核对钉住数"); return; }
        if (n < inventory.pinned) { app_http_error(fd, 409, "保留条数少于已钉住数，请先解除钉住"); return; }
        requested = (int)n;
    }
    if (enabled[0]) {
        if (wake_settings_write(wake_p05_root(), "forensics-enabled", enabled[0] == '0' ? "0" : "1") != 0) {
            app_http_error(fd, 500, "无法保存取证开关"); return;
        }
    }
    if (keep[0]) {
        snprintf(value, sizeof(value), "%d", requested);
        if (wake_settings_write(wake_p05_root(), "forensics-keep", value) != 0) {
            app_http_error(fd, 500, "无法保存保留条数"); return;
        }
        int remaining = inventory.count;
        for (int i = 0; i < inventory.count && remaining > requested; i++) {
            if (inventory.items[i].pinned) continue;
            if (remove_id(wake_p05_dir(), inventory.items[i].id)) { app_http_error(fd, 500, "保留条数已保存，旧记录清理失败，请刷新核对"); return; }
            remaining--;
        }
    }
    send_json(fd, "{\"ok\":true}");
}
static void wake_p05_config(int fd, const char *body) {
    if (wake_forensics_lock(wake_p05_dir())) { app_http_error(fd, 500, "取证锁不可用"); return; }
    wake_p05_config_unlocked(fd, body); wake_forensics_unlock();
}
static void wake_p05_cloud_get(int fd) {
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"on\":%d}", wake_p05_cloud_on());
    send_json(fd, json);
}
static void wake_p05_cloud_set(int fd, const char *body) {
    char on[8], confirm[8];
    on[0] = confirm[0] = 0;
    if (app_json_str(body, "on", on, sizeof(on)) != 0) { app_http_error(fd, 400, "缺少开关"); return; }
    app_json_str(body, "confirm", confirm, sizeof(confirm));
    if (on[0] == '1' || !strcmp(on, "on")) {
        if (!(confirm[0] == '1' || !strcmp(confirm, "yes"))) {
            send_err(fd, 409, "Conflict",
                "{\"ok\":false,\"need_confirm\":true,\"error\":\"开启后，分不清的唤醒会把这段唤醒词录音发到小米云端识别；这类唤醒会慢约 0.5-0.75 秒\"}");
            return;
        }
        if (wake_settings_write(wake_p05_root(), "cloud-verify", "on") != 0) {
            app_http_error(fd, 500, "无法保存云端核对开关"); return;
        }
    } else if (wake_settings_write(wake_p05_root(), "cloud-verify", "off") != 0) {
        app_http_error(fd, 500, "无法保存云端核对开关"); return;
    }
    send_json(fd, "{\"ok\":true}");
}
static void wake_p05_pinyin(int fd, const char *body) {
    char name[64], json[2048];
    ww_word_t words[WW_MAXWORDS];
    int nwords, i, n, warn = 0;
    const char *fixed = "小爱同学";
    if (app_json_str(body, "name", name, sizeof(name)) != 0 || !ww_name_ok(name)) {
        app_http_error(fd, 400, "名称须为 2-6 个汉字"); return;
    }
    nwords = ww_read_conf(words, WW_MAXWORDS);
    n = snprintf(json, sizeof(json), "{\"ok\":true,\"threshold\":%.2f,\"hits\":[", WAKE_PINYIN_WARN);
    for (i = 0; i < nwords && n < (int)sizeof(json) - 80; i++) {
        float score = wake_pinyin_warn_similarity(name, words[i].name);
        if (score >= WAKE_PINYIN_WARN) warn = 1;
        n += snprintf(json + n, sizeof(json) - (size_t)n, "%s{\"name\":\"%s\",\"score\":%.3f}",
                      i ? "," : "", words[i].name, score);
    }
    {
        float score = wake_pinyin_warn_similarity(name, fixed);
        if (score >= WAKE_PINYIN_WARN) warn = 1;
        if (n < (int)sizeof(json) - 80)
            n += snprintf(json + n, sizeof(json) - (size_t)n, "%s{\"name\":\"%s\",\"score\":%.3f}",
                          nwords ? "," : "", fixed, score);
    }
    if (n > 0 && n < (int)sizeof(json) - 40)
        snprintf(json + n, sizeof(json) - (size_t)n, "],\"warn\":%s}", warn ? "true" : "false");
    send_json(fd, json);
}
static void wake_p05_cross_get(int fd) {
    FILE *file = fopen("/tmp/lx06-wake-cross-last.txt", "r");
    char *buf = malloc(16384);
    size_t n;
    if (!buf) { app_http_error(fd, 500, "内存不足"); return; }
    if (!file) { free(buf); send_json(fd, "{\"ok\":true,\"status\":\"empty\",\"text\":\"还没有交叉测试结果\"}"); return; }
    n = fread(buf, 1, 16000, file); fclose(file); buf[n] = 0;
    {
        char *json = malloc(20000);
        size_t j = 0;
        if (!json) { free(buf); app_http_error(fd, 500, "内存不足"); return; }
        j += (size_t)snprintf(json, 20000, "{\"ok\":true,\"status\":\"ready\",\"text\":\"");
        for (size_t i = 0; i < n && j + 8 < 20000; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c == '"' || c == '\\') json[j++] = '\\', json[j++] = (char)c;
            else if (c == '\n') json[j++] = '\\', json[j++] = 'n';
            else if (c >= 32) json[j++] = (char)c;
        }
        snprintf(json + j, 20000 - j, "\"}");
        send_json(fd, json);
        free(json);
    }
    free(buf);
}
static void wake_p05_cross_post(int fd) {
    const char *script = "/usr/lib/lx06/wakeword/ww-cross-test.sh";
    pid_t pid;
    if (access(script, X_OK) != 0) {
        app_http_error(fd, 503, "交叉测试脚本不在本机，检测器命令为 wakeword_template matrix");
        return;
    }
    pid = fork();
    if (pid < 0) { app_http_error(fd, 500, "无法启动交叉测试"); return; }
    if (!pid) {
        int out = open("/tmp/lx06-wake-cross-last.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); close(out); }
        execl("/bin/sh", "sh", script, (char *)NULL);
        _exit(127);
    }
    send_json(fd, "{\"ok\":true,\"status\":\"started\"}");
}
/* Read-only recalculation. Does not call ww-train.sh or ww_write_conf, so the
 * ownership pause stays in place and no threshold is stored or loosened. */
static void wake_p05_recal_preview(int fd, const char *body) {
    char name[24], model[160], json[1024];
    char own[10][160], cross[40][160];
    char *argv[64];
    int nown = 0, ncross = 0, argn = 0, i, status = 0;
    int outp[2] = {-1, -1}, errp[2] = {-1, -1};
    pid_t pid;
    const char *bin = getenv("LX06_WAKE_CALIBRATE");
    ww_word_t words[WW_MAXWORDS];
    int nwords, saved_found = 0;
    double saved = 0, computed = 0;
    char stdout_buf[2048], stderr_buf[4096];
    size_t so = 0, se = 0;
    if (!bin || !*bin) bin = WWHOME "/wakeword_calibrate";
    if (app_json_str(body, "name", name, sizeof(name)) != 0 || !ww_name_ok(name)) {
        app_http_error(fd, 400, "唤醒词无效"); return;
    }
    if (access(bin, X_OK) != 0) {
        app_http_error(fd, 503, "校准程序不在本机。训练暂停没有解除，门限不会写入"); return;
    }
    snprintf(model, sizeof(model), "%s/%s.lwt", WWMODEL, name);
    if (access(model, R_OK) != 0) { app_http_error(fd, 404, "没有这个词的已有模型"); return; }
    for (i = 1; i <= 10 && nown < 10; i++) {
        snprintf(own[nown], sizeof(own[nown]), "%s/%s/%d.wav", WWRECS, name, i);
        if (access(own[nown], R_OK) == 0) nown++;
    }
    if (nown < 3) { app_http_error(fd, 400, "已有录音少于 3 条，无法计算"); return; }
    nwords = ww_read_conf(words, WW_MAXWORDS);
    for (i = 0; i < nwords; i++) if (!strcmp(words[i].name, name)) { saved = words[i].th; saved_found = 1; }
    for (i = 0; i < nwords && ncross < 40; i++) {
        int r;
        if (!strcmp(words[i].name, name)) continue;
        for (r = 1; r <= 10 && ncross < 40; r++) {
            snprintf(cross[ncross], sizeof(cross[ncross]), "%s/%s/%d.wav", WWRECS, words[i].name, r);
            if (access(cross[ncross], R_OK) == 0) ncross++;
        }
    }
    argv[argn++] = (char *)bin;
    argv[argn++] = model;
    for (i = 0; i < nown && argn < 60; i++) argv[argn++] = own[i];
    if (ncross && argn < 60) {
        argv[argn++] = "--cross";
        for (i = 0; i < ncross && argn < 62; i++) argv[argn++] = cross[i];
    }
    argv[argn] = NULL;
    if (pipe(outp) || pipe(errp)) { app_http_error(fd, 500, "无法计算"); return; }
    pid = fork();
    if (pid < 0) { app_http_error(fd, 500, "无法计算"); return; }
    if (!pid) {
        dup2(outp[1], 1); dup2(errp[1], 2);
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        execv(bin, argv);
        _exit(127);
    }
    close(outp[1]); close(errp[1]);
    for (;;) {
        struct pollfd pfd[2] = {{outp[0], POLLIN, 0}, {errp[0], POLLIN, 0}};
        int live = (outp[0] >= 0) + (errp[0] >= 0);
        if (!live) break;
        if (poll(pfd, 2, 1000) < 0) { if (errno == EINTR) continue; break; }
        if (outp[0] >= 0 && (pfd[0].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(outp[0], stdout_buf + so, sizeof(stdout_buf) - 1 - so);
            if (n > 0) so += (size_t)n; else { close(outp[0]); outp[0] = -1; }
            if (so >= sizeof(stdout_buf) - 1) { close(outp[0]); outp[0] = -1; }
        }
        if (errp[0] >= 0 && (pfd[1].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(errp[0], stderr_buf + se, sizeof(stderr_buf) - 1 - se);
            if (n > 0) se += (size_t)n; else { close(errp[0]); errp[0] = -1; }
            if (se >= sizeof(stderr_buf) - 1) { close(errp[0]); errp[0] = -1; }
        }
    }
    stdout_buf[so] = 0; stderr_buf[se] = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (strstr(stderr_buf, "CALIBRATION_CONFLICT")) {
        snprintf(json, sizeof(json),
            "{\"ok\":true,\"saved\":false,\"conflict\":true,\"recordings\":%d,"
            "\"note\":\"交叉负例会触发这个词，自动门限收不紧。没有放宽，也没有写入。训练仍暂停，这个词现在不会被加载。\"}", nown);
        send_json(fd, json); return;
    }
    {
        char *line = strstr(stdout_buf, "CALIBRATED ");
        if (!line || sscanf(line, "CALIBRATED %lf", &computed) != 1 || !(computed > 0)) {
            app_http_error(fd, 500, "校准没有给出门限，配置未改"); return;
        }
    }
    if (saved_found && computed > saved + 0.0000005) {
        snprintf(json, sizeof(json),
            "{\"ok\":true,\"saved\":false,\"loosened\":true,\"old_threshold\":%.6f,\"new_threshold\":%.6f,"
            "\"note\":\"算出的门限比已保存的更宽，已拒绝。没有写入。训练仍暂停，这个词现在不会被加载。\"}",
            saved, computed);
        send_json(fd, json); return;
    }
    snprintf(json, sizeof(json),
        "{\"ok\":true,\"saved\":false,\"loosened\":false,\"old_threshold\":%.6f,\"new_threshold\":%.6f,\"recordings\":%d,"
        "\"note\":\"新门限没有比已保存的更宽，但训练暂停，结果没有写入 wakeword.conf。监听只加载新策略标识，所以这个词现在仍然不会被唤醒。录音和模型未改。\"}",
        saved_found ? saved : 0, computed, nown);
    send_json(fd, json);
}
static void wake_p05_joint_get(int fd) {
    char run[64], path[512], status[32]="incomplete", original[8192]="null", calibrated[8192]="null", json[18000];
    const char *root=WW_JOINT_ROOT;
    if(wake_settings_read(root,"latest",run,sizeof(run)) || strncmp(run,"run-",4) || strlen(run)!=10 || !wake_id_ok(run)) {
        send_json(fd,"{\"ok\":true,\"status\":\"empty\",\"original\":null,\"calibrated\":null}");return;
    }
    snprintf(path,sizeof(path),"%s/%s",root,run);wake_settings_read(path,"status",status,sizeof(status));
    const char *names[]={"original.json","calibrated.json"}; char *buffers[]={original,calibrated};
    for(int i=0;i<2;i++) {
        snprintf(path,sizeof(path),"%s/%s/%s",root,run,names[i]);
        int in=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(in<0)continue;
        ssize_t n=read(in,buffers[i],8191);close(in);
        if(n<=0 || n>=8191)strcpy(buffers[i],"null");else buffers[i][n]=0;
    }
    if(strcmp(status,"complete") && strcmp(status,"original") && strcmp(status,"disabled"))strcpy(status,"incomplete");
    snprintf(json,sizeof(json),"{\"ok\":true,\"status\":\"%s\",\"run\":\"%s\",\"original\":%s,\"calibrated\":%s}",status,run,original,calibrated);
    send_json(fd,json);
}
static void wake_p05_joint_post(int fd,const char *body) {
    (void)body;
    if(ww_joint_start("","")) {app_http_error(fd,503,"无法启动全词校准");return;}
    send_json(fd,"{\"ok\":true,\"pending\":1}");
}
static int wake_p05_try(const char *method, const char *path, int fd, const char *q, const char *body) {
    if (!strcmp(method, "GET") && !strcmp(path, "/api/wakeword/joint-report")) { wake_p05_joint_get(fd); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/joint-recalibrate")) { ww_modify(wake_p05_joint_post,fd,body); return 1; }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/wakeword/forensics")) { wake_p05_list(fd); return 1; }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/wakeword/forensics/export")) { wake_p05_export(fd, q); return 1; }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/wakeword/forensics/audio")) { wake_p05_audio(fd, q); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/forensics/label")) { wake_p05_label(fd, body); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/forensics/clear")) { wake_p05_clear(fd); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/forensics/config")) { wake_p05_config(fd, body); return 1; }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/wakeword/cloud-verify")) { wake_p05_cloud_get(fd); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/cloud-verify")) { wake_p05_cloud_set(fd, body); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/pinyin-check")) { wake_p05_pinyin(fd, body); return 1; }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/wakeword/cross-test")) { wake_p05_cross_get(fd); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/cross-test")) { wake_p05_cross_post(fd); return 1; }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/wakeword/recalibrate-preview")) { wake_p05_recal_preview(fd, body); return 1; }
    return 0;
}

#include "wake_forensics.h"
#include "wake_policy.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

int wake_mem_available_kib(const char *meminfo_path) {
    FILE *file = fopen(meminfo_path ? meminfo_path : "/proc/meminfo", "r");
    char line[256];
    int kib = -1;
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemAvailable: %d", &kib) == 1) break;
        kib = -1;
    }
    fclose(file);
    return kib;
}

int wake_forensics_enabled(const char *flag_file) {
    FILE *file;
    int ch;
    if (!flag_file || !flag_file[0]) return 1;
    file = fopen(flag_file, "r");
    if (!file) return 1;
    ch = fgetc(file);
    fclose(file);
    return ch == '0' ? 0 : 1;
}

int wake_forensics_keep(const char *keep_file) {
    FILE *file;
    int keep = WAKE_FORENSICS_KEEP_DEFAULT;
    if (!keep_file || !keep_file[0]) return keep;
    file = fopen(keep_file, "r");
    if (!file) return keep;
    if (fscanf(file, "%d", &keep) != 1) keep = WAKE_FORENSICS_KEEP_DEFAULT;
    fclose(file);
    if (keep < WAKE_FORENSICS_KEEP_MIN) keep = WAKE_FORENSICS_KEEP_MIN;
    if (keep > WAKE_FORENSICS_KEEP_MAX) keep = WAKE_FORENSICS_KEEP_MAX;
    return keep;
}

int wake_id_ok(const char *id) {
    size_t n;
    if (!id || !id[0]) return 0;
    n = strlen(id);
    if (n > 48) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int pinned_json(const char *json) {
    const char *p = json;
    while (p && (p = strstr(p, "\"pinned\""))) {
        p += 8;
        while (isspace((unsigned char)*p)) p++;
        if (*p != ':') continue;
        p++;
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "true", 4) && (p[4] == ',' || p[4] == '}' || isspace((unsigned char)p[4]))) return 1;
    }
    return 0;
}

int wake_settings_read(const char *dir, const char *name, char *out, size_t cap) {
    char path[512];
    FILE *file;
    if (!out || cap < 2 || !dir || !name || strchr(name, '/') || strstr(name, "..")) return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "r");
    out[0] = 0;
    if (!file) return -1;
    if (!fgets(out, (int)cap, file)) out[0] = 0;
    fclose(file);
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

int wake_settings_write(const char *dir, const char *name, const char *value) {
    char path[512], tmp[540];
    FILE *file;
    if (!dir || !name || !value || strchr(name, '/') || strstr(name, "..")) return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    file = fopen(tmp, "w");
    if (!file) return -1;
    if (fprintf(file, "%s\n", value) < 0 || fclose(file) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Compact public-domain SHA-256 (FIPS 180-4), file helper only. */
static unsigned rotr(unsigned x, unsigned n) { return (x >> n) | (x << (32 - n)); }
int wake_sha256_file(const char *path, char out[65]) {
    static const unsigned k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    unsigned h0=0x6a09e667,h1=0xbb67ae85,h2=0x3c6ef372,h3=0xa54ff53a;
    unsigned h4=0x510e527f,h5=0x9b05688c,h6=0x1f83d9ab,h7=0x5be0cd19;
    unsigned char block[64];
    unsigned long long bits = 0;
    size_t filled = 0;
    FILE *file = fopen(path, "rb");
    if (!file || !out) { if (file) fclose(file); return -1; }
    for (;;) {
        size_t n = fread(block + filled, 1, 64 - filled, file);
        filled += n; bits += (unsigned long long)n * 8;
        if (filled < 64) break;
        unsigned w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((unsigned)block[i*4] << 24) | ((unsigned)block[i*4+1] << 16) |
                   ((unsigned)block[i*4+2] << 8) | block[i*4+3];
        for (int i = 16; i < 64; i++) {
            unsigned s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,hh=h7;
        for (int i = 0; i < 64; i++) {
            unsigned S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            unsigned ch = (e & f) ^ (~e & g);
            unsigned t1 = hh + S1 + ch + k[i] + w[i];
            unsigned S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            unsigned maj = (a & b) ^ (a & c) ^ (b & c);
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+S0+maj;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e; h5+=f; h6+=g; h7+=hh;
        filled = 0;
    }
    fclose(file);
    block[filled++] = 0x80;
    if (filled > 56) { while (filled < 64) block[filled++] = 0; filled = 0;
        unsigned w[64] = {0};
        for (int i = 0; i < 16; i++)
            w[i] = ((unsigned)block[i*4] << 24) | ((unsigned)block[i*4+1] << 16) |
                   ((unsigned)block[i*4+2] << 8) | block[i*4+3];
        for (int i = 16; i < 64; i++) {
            unsigned s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,hh=h7;
        for (int i = 0; i < 64; i++) {
            unsigned S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            unsigned ch = (e & f) ^ (~e & g);
            unsigned t1 = hh + S1 + ch + k[i] + w[i];
            unsigned S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            unsigned maj = (a & b) ^ (a & c) ^ (b & c);
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+S0+maj;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e; h5+=f; h6+=g; h7+=hh;
        memset(block, 0, 64); filled = 0;
    }
    while (filled < 56) block[filled++] = 0;
    for (int i = 7; i >= 0; i--) block[filled++] = (unsigned char)(bits >> (i * 8));
    {
        unsigned w[64] = {0};
        for (int i = 0; i < 16; i++)
            w[i] = ((unsigned)block[i*4] << 24) | ((unsigned)block[i*4+1] << 16) |
                   ((unsigned)block[i*4+2] << 8) | block[i*4+3];
        for (int i = 16; i < 64; i++) {
            unsigned s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,hh=h7;
        for (int i = 0; i < 64; i++) {
            unsigned S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            unsigned ch = (e & f) ^ (~e & g);
            unsigned t1 = hh + S1 + ch + k[i] + w[i];
            unsigned S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            unsigned maj = (a & b) ^ (a & c) ^ (b & c);
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+S0+maj;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e; h5+=f; h6+=g; h7+=hh;
    }
    snprintf(out, 65, "%08x%08x%08x%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4, h5, h6, h7);
    return 0;
}

static int write_file(const char *path, const void *data, size_t n) {
    char tmp[576];
    FILE *file;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    file = fopen(tmp, "wb");
    if (!file) return -1;
    if (n && fwrite(data, 1, n, file) != n) { int saved = errno; fclose(file); unlink(tmp); errno = saved; return -1; }
    if (fclose(file) != 0) { int saved = errno; unlink(tmp); errno = saved; return -1; }
    if (rename(tmp, path) != 0) { int saved = errno; unlink(tmp); errno = saved; return -1; }
    return 0;
}

struct forensic_item { char id[64]; time_t mtime; off_t bytes; int pinned; };
struct forensic_inventory {
    struct forensic_item items[128];
    int count, pinned;
    off_t bytes;
};
/* Fixed-size saturated counter, outside the event namespace/export. Reserve
 * its maximum 10 digits + LF even before the first drop. */
#define FORENSICS_COUNTER_BYTES 11
static unsigned forensics_dropped(const char *dir) {
    char path[576], text[32]; unsigned n = 0;
    snprintf(path, sizeof(path), "%s/.dropped", dir);
    FILE *file = fopen(path, "r");
    if (file) {
        if (fgets(text, sizeof(text), file)) {
            char *end; errno = 0;
            unsigned long value = strtoul(text, &end, 10);
            if (end != text && !errno && value <= UINT_MAX && (*end == '\n' || !*end)) n = (unsigned)value;
        }
        fclose(file);
    }
    return n;
}
static int forensics_drop(const char *dir) {
    char path[576], text[FORENSICS_COUNTER_BYTES + 1];
    unsigned n = forensics_dropped(dir);
    if (n < UINT_MAX) n++;
    snprintf(path, sizeof(path), "%s/.dropped", dir);
    int size = snprintf(text, sizeof(text), "%010u\n", n);
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    /* Same lock, fixed length, no second counter file at the hard byte cap. */
    int failed = ftruncate(fd, size) || pwrite(fd, text, (size_t)size, 0) != size;
    if (close(fd)) failed = 1;
    return failed ? -1 : 2;
}

static int cmp_item(const void *a, const void *b) {
    const struct forensic_item *x = a, *y = b;
    if (x->mtime < y->mtime) return -1;
    if (x->mtime > y->mtime) return 1;
    return strcmp(x->id, y->id);
}

static int remove_id(const char *dir, const char *id) {
    char path[576];
    snprintf(path, sizeof(path), "%s/%s.wav", dir, id);
    if (unlink(path) && errno != ENOENT) return -1;
    snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    return unlink(path) && errno != ENOENT ? -1 : 0;
}

/* Caller holds the existing forensics lock. Unknown/oversized inventories
 * fail closed rather than treating unreadable evidence as unpinned. */
static int forensics_inventory(const char *dir, struct forensic_inventory *inventory) {
    DIR *directory = opendir(dir);
    struct dirent *entry;
    memset(inventory, 0, sizeof(*inventory));
    inventory->bytes = FORENSICS_COUNTER_BYTES;
    if (!directory) return -1;
    int rc = -1, files = 0;
    while ((entry = readdir(directory))) {
        size_t len = strlen(entry->d_name);
        char id[64], path[576];
        struct stat st;
        FILE *file;
        char head[8192];
        int is_json = len > 5 && !strcmp(entry->d_name + len - 5, ".json");
        int is_wav = len > 4 && !strcmp(entry->d_name + len - 4, ".wav");
        if (!is_json && !is_wav) continue;
        size_t suffix = is_json ? 5 : 4;
        if (len - suffix > 48) continue;
        memcpy(id, entry->d_name, len - suffix); id[len - suffix] = 0;
        if (!wake_id_ok(id)) continue;
        if (++files > 256) goto done;
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            st.st_size > WAKE_FORENSICS_MAX_BYTES) goto done;
        inventory->bytes += st.st_size;
        if (is_wav) continue; /* includes orphan audio in the byte budget */
        int n = inventory->count;
        if (n >= 128 || st.st_size >= (off_t)sizeof(head)) goto done;
        file = fopen(path, "r");
        if (!file) goto done;
        size_t got = fread(head, 1, sizeof(head) - 1, file);
        int failed = ferror(file); fclose(file); head[got] = 0;
        if (failed || got != (size_t)st.st_size || strlen(head) != got) goto done;
        struct forensic_item *item = &inventory->items[n];
        item->mtime = st.st_mtime; item->bytes = st.st_size;
        item->pinned = pinned_json(head);
        snprintf(path, sizeof(path), "%s/%s.wav", dir, id);
        if (!lstat(path, &st)) {
            if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > WAKE_FORENSICS_MAX_BYTES) goto done;
            item->bytes += st.st_size;
        } else if (errno != ENOENT) goto done;
        strcpy(item->id, id);
        inventory->count++; inventory->pinned += item->pinned;
    }
    qsort(inventory->items, (size_t)inventory->count, sizeof(inventory->items[0]), cmp_item);
    rc = 0;
done:
    closedir(directory);
    return rc;
}
/* Plan BEFORE writing or evicting. A refused admission changes only .dropped;
 * updates are explicit user annotations, never detector id replacement. */
static int forensics_admit(const char *dir, const char *id, const char *json,
                          size_t audio_bytes, int update, int keep, int max_bytes, int max_pinned) {
    struct forensic_inventory inventory;
    unsigned char drop[128] = {0};
    if (forensics_inventory(dir, &inventory)) return -1;
    if (keep < WAKE_FORENSICS_KEEP_MIN) keep = WAKE_FORENSICS_KEEP_MIN;
    if (keep > WAKE_FORENSICS_KEEP_MAX) keep = WAKE_FORENSICS_KEEP_MAX;
    if (max_bytes > WAKE_FORENSICS_MAX_BYTES) max_bytes = WAKE_FORENSICS_MAX_BYTES;
    if (max_pinned > WAKE_FORENSICS_MAX_PINNED) max_pinned = WAKE_FORENSICS_MAX_PINNED;
    size_t json_bytes = strnlen(json, 8192);
    if (json_bytes >= 8192) return -1;
    int target = -1, pins = inventory.pinned + pinned_json(json), count = inventory.count + 1;
    off_t bytes = inventory.bytes + (off_t)json_bytes;
    for (int i = 0; i < inventory.count; i++) if (!strcmp(inventory.items[i].id, id)) target = i;
    if (!update && target >= 0) return forensics_drop(dir);
    if (update) {
        if (target < 0) return -1;
        pins -= inventory.items[target].pinned; count--;
        /* Old JSON remains until atomic rename: reserve both versions. The
         * existing WAV is already counted and is never rewritten here. */
    } else {
        char path[576]; struct stat st;
        snprintf(path, sizeof(path), "%s/%s.wav", dir, id);
        if (!lstat(path, &st) || errno != ENOENT) return forensics_drop(dir);
        if (audio_bytes > (size_t)WAKE_FORENSICS_MAX_BYTES) return forensics_drop(dir);
        bytes += (off_t)audio_bytes;
    }
    if (pins > max_pinned || pins > keep) return update ? 2 : forensics_drop(dir);
    for (int i = 0; i < inventory.count && (count > keep || bytes > max_bytes); i++) {
        if (i == target || inventory.items[i].pinned) continue;
        drop[i] = 1; count--; bytes -= inventory.items[i].bytes;
    }
    if (count > keep || bytes > max_bytes) return update ? 2 : forensics_drop(dir);
    for (int i = 0; i < inventory.count; i++)
        if (drop[i] && remove_id(dir, inventory.items[i].id)) return -1;
    return 0;
}

static void ff_put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void ff_put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static int write_wav_pcm(const char *path, const int16_t *pcm, int samples) {
    char tmp[640];
    if (strlen(path) + 5 >= sizeof(tmp)) return -1;
    unsigned char hdr[44];
    unsigned data_bytes = (unsigned)samples * 2;
    FILE *file;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    file = fopen(tmp, "wb");
    if (!file) return -1;
    memcpy(hdr, "RIFF", 4); ff_put32(hdr + 4, 36u + data_bytes); memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4); ff_put32(hdr + 16, 16); ff_put16(hdr + 20, 1); ff_put16(hdr + 22, 1);
    ff_put32(hdr + 24, 16000); ff_put32(hdr + 28, 32000); ff_put16(hdr + 32, 2); ff_put16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4); ff_put32(hdr + 40, data_bytes);
    if (fwrite(hdr, 1, 44, file) != 44 ||
        (samples && fwrite(pcm, 2, (size_t)samples, file) != (size_t)samples)) {
        int saved = errno; fclose(file); unlink(tmp); errno = saved; return -1;
    }
    if (fclose(file) != 0) { int saved = errno; unlink(tmp); errno = saved; return -1; }
    if (rename(tmp, path) != 0) { int saved = errno; unlink(tmp); errno = saved; return -1; }
    return 0;
}

/* Serialize the detector writer and web annotation/eviction. Nested calls in
 * the single-threaded web worker reuse its lock; separate processes flock. */
static int forensics_lock_fd = -1, forensics_lock_depth;
static int wake_forensics_lock(const char *dir) {
    if (forensics_lock_depth) {forensics_lock_depth++;return 0;}
    if(mkdir(dir,0700) && errno!=EEXIST)return -1;
    int d=open(dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(d<0)return -1;
    int lock=openat(d,".lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);close(d);
    if(lock<0)return -1;
    if(flock(lock,LOCK_EX)){close(lock);return -1;}
    forensics_lock_fd=lock;forensics_lock_depth=1;return 0;
}
static void wake_forensics_unlock(void) {
    if(forensics_lock_depth && !--forensics_lock_depth) {close(forensics_lock_fd);forensics_lock_fd=-1;}
}
static int forensics_commit_pcm_unlocked(const char *dir, const char *id, const char *json,
                              const int16_t *pcm, int samples, int allow_audio,
                              int keep, int max_bytes, int max_pinned) {
    char path[576];
    if (!dir || !wake_id_ok(id) || !json) return -1;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    int has_audio = allow_audio && pcm && samples > 0 && samples <= 64000;
    int rc = forensics_admit(dir, id, json, has_audio ? 44u + (size_t)samples * 2 : 0,
                             0, keep, max_bytes, max_pinned);
    if (rc) return rc;
    if (has_audio) {
        snprintf(path, sizeof(path), "%s/%s.wav", dir, id);
        if (write_wav_pcm(path, pcm, samples))
            return errno == ENOSPC || errno == EDQUOT ? forensics_drop(dir) : -1;
    }
    snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    if (write_file(path, json, strlen(json))) {
        int saved = errno;
        snprintf(path, sizeof(path), "%s/%s.wav", dir, id); if (has_audio) unlink(path);
        return saved == ENOSPC || saved == EDQUOT ? forensics_drop(dir) : -1;
    }
    return has_audio ? 0 : 1;
}

static int forensics_commit_unlocked(const char *dir, const char *id, const char *json,
                          const void *wav, size_t wav_bytes,
                          int keep, int max_bytes, int max_pinned) {
    char path[576];
    int audio = wav && wav_bytes;
    if (!dir || !wake_id_ok(id) || !json) return -1;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    int rc = forensics_admit(dir, id, json, audio ? wav_bytes : 0, 0, keep, max_bytes, max_pinned);
    if (rc) return rc;
    if (audio) {
        snprintf(path, sizeof(path), "%s/%s.wav", dir, id);
        if (write_file(path, wav, wav_bytes))
            return errno == ENOSPC || errno == EDQUOT ? forensics_drop(dir) : -1;
    }
    snprintf(path, sizeof(path), "%s/%s.json", dir, id);
    if (write_file(path, json, strlen(json))) {
        int saved = errno;
        snprintf(path, sizeof(path), "%s/%s.wav", dir, id); if (audio) unlink(path);
        return saved == ENOSPC || saved == EDQUOT ? forensics_drop(dir) : -1;
    }
    return audio ? 0 : 1;
}

int wake_forensics_commit_pcm(const char *dir, const char *id, const char *json,
    const int16_t *pcm, int samples, int allow_audio, int keep, int max_bytes, int max_pinned) {
    if(!dir || wake_forensics_lock(dir))return -1;
    int rc=forensics_commit_pcm_unlocked(dir,id,json,pcm,samples,allow_audio,keep,max_bytes,max_pinned);
    wake_forensics_unlock();return rc;
}
int wake_forensics_commit(const char *dir, const char *id, const char *json,
    const void *wav, size_t wav_bytes, int keep, int max_bytes, int max_pinned) {
    if(!dir || wake_forensics_lock(dir))return -1;
    int rc=forensics_commit_unlocked(dir,id,json,wav,wav_bytes,keep,max_bytes,max_pinned);
    wake_forensics_unlock();return rc;
}
int wake_forensics_update_json(const char *dir, const char *id, const char *json,
                              int keep, int max_bytes, int max_pinned) {
    if (!dir || !wake_id_ok(id) || !json || wake_forensics_lock(dir)) return -1;
    int rc = forensics_admit(dir, id, json, 0, 1, keep, max_bytes, max_pinned);
    if (!rc) {
        char path[576]; snprintf(path, sizeof(path), "%s/%s.json", dir, id);
        rc = write_file(path, json, strlen(json));
    }
    wake_forensics_unlock(); return rc;
}

/* Offline WT1 checks: matcher (§4, §9.1 1-8, 13), listen window (§2.3, §9.1 9-12, 24),
 * routing and official-bind decisions (§2.7, §2.9, §9.1 19, 21, 22). */
#define _GNU_SOURCE
#include "template-matcher.c"
#include "listen-window.c"
#include "script-phrase-check.c"
#define LX06_SCRIPTS_ALLOWED_EXTERNAL
static int lx06_scripts_allowed(void) { return 1; }
#include "official-bind.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct lx06_match_candidate cand(const char *script, int line, const char *phrase) {
    struct lx06_match_candidate c = {script, line, phrase};
    return c;
}

static void expect_template(const char *text, int index, const char *var, const char *value) {
    struct lx06_match_candidate rows[8];
    struct lx06_match_result mr;
    int i;
    for (i = 0; i < 8; i++) rows[i] = cand("qishui", i, lx06_qishui_rows[i]);
    assert(lx06_template_match(text, rows, 8, &mr) == 0);
    assert(mr.kind == LX06_MATCH_TEMPLATE);
    assert(mr.index == index);
    if (var) {
        int found = 0;
        for (i = 0; i < mr.var_count; i++)
            if (!strcmp(mr.vars[i].name, var) && !strcmp(mr.vars[i].value, value)) found = 1;
        assert(found);
    }
}

static int g_readers, g_fail_open, g_final_at;
static char g_final[128];
static int g_speech_seen, g_empty_final;
static uint64_t g_speech_started;
static int asr_open(void *ctx) { (void)ctx; if (g_fail_open) return -1; g_readers++; return 0; }
static void asr_close(void *ctx) { (void)ctx; if (g_readers > 0) g_readers--; }
static int asr_feed(void *ctx, const void *pcm, size_t n, uint64_t now) { (void)ctx; (void)pcm; (void)n; (void)now; return 0; }
static int asr_poll(void *ctx, char *buf, size_t cap, uint64_t now) {
    (void)ctx;
    if ((g_final[0] || g_empty_final) && (int)now >= g_final_at) {
        g_empty_final = 0;
        snprintf(buf, cap, "%s", g_final);
        g_final[0] = 0;
        return 1;
    }
    return 0;
}
static int asr_readers(void *ctx) { (void)ctx; return g_readers; }
static int asr_speech_seen(void *ctx, uint64_t *started) {
    (void)ctx; *started = g_speech_started; return g_speech_seen;
}

static struct lx06_listen_window *open_window_timing(struct lx06_match_candidate *rows,
        unsigned ms, unsigned start_ms, unsigned max_ms, uint64_t now) {
    struct lx06_listen_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cands = rows;
    cfg.cand_count = 8;
    cfg.window_ms = ms;
    cfg.start_ms = start_ms;
    cfg.max_ms = max_ms;
    cfg.asr.open = asr_open;
    cfg.asr.close = asr_close;
    cfg.asr.feed = asr_feed;
    cfg.asr.poll = asr_poll;
    cfg.asr.readers = asr_readers;
    cfg.asr.speech_seen = asr_speech_seen;
    return lx06_listen_open(&cfg, now);
}

static struct lx06_listen_window *open_window(struct lx06_match_candidate *rows, unsigned ms, uint64_t now) {
    return open_window_timing(rows, ms, 0, 0, now);
}

static void test_matcher(void) {
    struct lx06_match_candidate rows[8], mixed[4], two[2];
    struct lx06_match_result mr;
    char norm[128];
    int i;
    struct timespec a, b;
    long long ns;
    for (i = 0; i < 8; i++) rows[i] = cand("qishui", i, lx06_qishui_rows[i]);
    assert(lx06_phrase_normalize("召唤 小智！", norm, sizeof(norm)) && !strcmp(norm, "召唤小智"));
    assert(lx06_phrase_normalize("不要召唤小智", norm, sizeof(norm)) && !strcmp(norm, "不要召唤小智"));
    two[0] = cand("xz", 0, "召唤小智");
    two[1] = cand("xz", 1, "召唤小智啊");
    assert(lx06_template_match("召唤小智", two, 2, &mr) == 0 && mr.kind == LX06_MATCH_EXACT && mr.index == 0);
    assert(lx06_template_match("召唤小智啊", two, 1, &mr) == 0 && mr.kind == LX06_MATCH_NONE);
    two[1] = cand("db", 0, "召唤小智");
    assert(lx06_template_match("召唤小智", two, 2, &mr) == 0 && mr.kind == LX06_MATCH_AMBIGUOUS && !mr.prompt[0]);
    expect_template("我要听周杰伦的晴天", 0, "artist", "周杰伦");
    expect_template("我要听周杰伦的晴天", 0, "song", "晴天");
    expect_template("我要听晴天", 5, "query", "晴天");
    expect_template("播放晴天", 7, "query", "晴天");
    expect_template("来一首晴天", 6, "query", "晴天");
    expect_template("我要听晴天歌", 4, "query", "晴天");
    expect_template("我要听周杰伦的歌", 3, "artist", "周杰伦");
    assert(lx06_template_match("我要听周杰伦的歌", rows, 8, &mr) == 0 && mr.var_count == 1);
    expect_template("播放周杰伦的晴天", 1, "song", "晴天");
    expect_template("我要听周杰伦的晴天歌", 0, "song", "晴天歌");
    expect_template("我要听周杰伦的晴天。", 0, "song", "晴天");
    mixed[0] = cand("qishui", 0, "我要听歌");
    mixed[1] = cand("qishui", 1, "我要听{query}");
    assert(lx06_template_match("我要听歌", mixed, 2, &mr) == 0 && mr.kind == LX06_MATCH_EXACT && mr.index == 0);
    two[0] = cand("a", 0, "播放{query}");
    two[1] = cand("b", 0, "播放{query}");
    assert(lx06_template_match("播放晴天", two, 2, &mr) == 0 && mr.kind == LX06_MATCH_REJECT);
    assert(!strcmp(mr.prompt, LX06_PROMPT_REJECT));
    assert(lx06_template_match("我要听", rows, 8, &mr) == 0 && mr.kind == LX06_MATCH_INCOMPLETE);
    assert(!strcmp(mr.prompt, LX06_PROMPT_REASK));
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (i = 0; i < 5000; i++) assert(lx06_template_match("我要听周杰伦的晴天", rows, 8, &mr) == 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    ns = (b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
    assert(ns / 5000 < (long long)LX06_MATCH_DEADLINE_US * 1000);
    /* No-template catalog: an exact phrase is unchanged by the presence of the
     * template function when the candidate list has no braces. */
    two[0] = cand("xz", 0, "召唤小智");
    assert(lx06_template_match("召唤小智", two, 1, &mr) == 0 && mr.kind == LX06_MATCH_EXACT);
    assert(lx06_template_match("我要听周杰伦的晴天", two, 1, &mr) == 0 && mr.kind == LX06_MATCH_NONE);
}

static void test_catalog(void) {
    char pipe[512] = "", local[512] = "", comma[512] = "";
    int i;
    for (i = 0; i < LX06_QISHUI_ROWS; i++) {
        if (i) { strcat(pipe, "|"); strcat(local, "|"); strcat(comma, ","); }
        strcat(pipe, lx06_qishui_rows[i]);
        strcat(local, lx06_qishui_rows[i]);
        strcat(comma, lx06_qishui_rows[i]);
    }
    assert(!strcmp(pipe, local));
    strcat(comma, ",放歌,我要听歌,来首歌");
    assert(strstr(comma, lx06_qishui_rows[0]) == comma);
    assert(strstr(pipe, "我要听{artist}的{song}|播放{artist}的{song}"));
}

static void test_window(void) {
    struct lx06_match_candidate rows[8];
    struct lx06_listen_window *w;
    struct lx06_listen_outcome out;
    int i;
    for (i = 0; i < 8; i++) rows[i] = cand("qishui", i, lx06_qishui_rows[i]);
    assert(lx06_window_ms_clamp(0) == 10000);
    assert(lx06_window_start_ms_clamp(0) == 5000);
    assert(lx06_window_start_ms_clamp(500) == 1000);
    assert(lx06_window_start_ms_clamp(2000) == 2000);
    assert(lx06_window_start_ms_clamp(20000) == 15000);
    assert(lx06_window_ms_clamp(2000) == 3000);
    assert(lx06_window_ms_clamp(8000) == 8000);
    assert(lx06_window_ms_clamp(20000) == 15000);
    assert(lx06_route_local(1, 1) == LX06_LOCAL_IGNORE);
    assert(lx06_route_local(0, 0) == LX06_LOCAL_PASSTHROUGH);
    assert(lx06_route_local(0, 1) == LX06_LOCAL_LISTEN);
    assert(!lx06_listen_launches(LX06_LISTEN_TIMEOUT));
    assert(!lx06_listen_launches(LX06_LISTEN_MISS));
    assert(lx06_listen_launches(LX06_LISTEN_HIT));
    g_readers = 0; g_fail_open = 0; g_final_at = 1500;
    snprintf(g_final, sizeof(g_final), "我要听晴天");
    w = open_window(rows, 0, 0);
    assert(w && g_readers == 1);
    assert(lx06_listen_poll(w, 1499, &out) == 0);
    assert(lx06_listen_poll(w, 1500, &out) == 1);
    assert(out.status == LX06_LISTEN_HIT && out.match.index == 5);
    assert(g_readers == 0 && out.readers_after_close == 0);
    assert(!strcmp(out.text, "我要听晴天"));
    lx06_listen_close(w);
    g_final[0] = 0; g_final_at = 0;
    w = open_window(rows, 0, 100);
    assert(lx06_listen_poll(w, 100 + 4999, &out) == 0 && g_readers == 1);
    assert(lx06_listen_poll(w, 100 + 5000, &out) == 1);
    assert(out.status == LX06_LISTEN_TIMEOUT && g_readers == 0);
    assert(!lx06_listen_launches(out.status));
    lx06_listen_close(w);
    /* PCM start fixes the deadline even when the observing poll is late. */
    w = open_window(rows, 0, 0);
    g_speech_seen = 1; g_speech_started = 2000;
    assert(lx06_listen_poll(w, 11999, &out) == 0 && g_readers == 1);
    assert(lx06_listen_poll(w, 12000, &out) == 1);
    assert(out.status == LX06_LISTEN_TIMEOUT && g_readers == 0);
    lx06_listen_close(w);
    /* Empty final after speech must not succeed or extend the cap. */
    w = open_window(rows, 0, 0);
    g_empty_final = 1; g_final_at = 6000;
    assert(lx06_listen_poll(w, 6000, &out) == 0);
    assert(lx06_listen_poll(w, 12000, &out) == 1 && out.status == LX06_LISTEN_TIMEOUT);
    lx06_listen_close(w);
    /* A real VAD final after the old open+8000 cap still ends early. */
    w = open_window(rows, 0, 0);
    snprintf(g_final, sizeof(g_final), "我要听晴天"); g_final_at = 9000;
    assert(lx06_listen_poll(w, 9000, &out) == 1 && out.status == LX06_LISTEN_HIT);
    lx06_listen_close(w);
    /* Explicit legacy max changes only the speech phase. */
    g_speech_seen = 0;
    w = open_window(rows, 7000, 0);
    assert(lx06_listen_poll(w, 4999, &out) == 0);
    assert(lx06_listen_poll(w, 5000, &out) == 1 && out.status == LX06_LISTEN_TIMEOUT);
    lx06_listen_close(w);
    w = open_window(rows, 7000, 0);
    g_speech_seen = 1; g_speech_started = 2000;
    assert(lx06_listen_poll(w, 8999, &out) == 0);
    assert(lx06_listen_poll(w, 9000, &out) == 1 && out.status == LX06_LISTEN_TIMEOUT);
    lx06_listen_close(w);
    /* Explicit max wins over the legacy alias; start is independent. */
    g_speech_seen = 0;
    w = open_window_timing(rows, 3000, 2000, 6000, 0);
    assert(lx06_listen_poll(w, 1999, &out) == 0);
    assert(lx06_listen_poll(w, 2000, &out) == 1 && out.status == LX06_LISTEN_TIMEOUT);
    lx06_listen_close(w);
    w = open_window_timing(rows, 3000, 2000, 6000, 0);
    g_speech_seen = 1; g_speech_started = 1000;
    assert(lx06_listen_poll(w, 6999, &out) == 0);
    assert(lx06_listen_poll(w, 7000, &out) == 1 && out.status == LX06_LISTEN_TIMEOUT);
    lx06_listen_close(w);
    g_speech_seen = 0;
    /* Empty final without speech leaves the 5s deadline intact. */
    w = open_window(rows, 0, 0);
    g_empty_final = 1; g_final_at = 200;
    assert(lx06_listen_poll(w, 200, &out) == 0);
    assert(lx06_listen_poll(w, 4999, &out) == 0);
    assert(lx06_listen_poll(w, 5000, &out) == 1 && out.status == LX06_LISTEN_TIMEOUT);
    lx06_listen_close(w);
    snprintf(g_final, sizeof(g_final), "完全听不懂");
    g_final_at = 6000;
    g_speech_seen = 1; g_speech_started = 2000;
    w = open_window(rows, 0, 0);
    assert(lx06_listen_poll(w, 6000, &out) == 1 && out.status == LX06_LISTEN_MISS && g_readers == 0);
    lx06_listen_close(w);
    g_final_at = 5000; snprintf(g_final, sizeof(g_final), "我要听晴天");
    w = open_window(rows, 0, 0);
    lx06_listen_cancel(w, 1000);
    assert(lx06_listen_poll(w, 1000, &out) == 1 && out.status == LX06_LISTEN_CANCELLED && g_readers == 0);
    lx06_listen_close(w);
    g_fail_open = 1; g_final[0] = 0;
    w = open_window(rows, 0, 0);
    assert(lx06_listen_poll(w, 0, &out) == 1 && out.status == LX06_LISTEN_ASR_FAIL && g_readers == 0);
    lx06_listen_close(w);
    g_fail_open = 0; g_speech_seen = 0;
    snprintf(g_final, sizeof(g_final), "我要听");
    g_final_at = 100;
    w = open_window(rows, 0, 0);
    assert(lx06_listen_poll(w, 100, &out) == 0 && g_readers == 1);
    assert(!strcmp(out.prompt, LX06_PROMPT_REASK));
    snprintf(g_final, sizeof(g_final), "我要听晴天");
    g_final_at = 200;
    assert(lx06_listen_poll(w, 200, &out) == 1 && out.status == LX06_LISTEN_HIT && g_readers == 0);
    lx06_listen_close(w);
}

static void test_bind_and_exit(const char *dir) {
    char bind[256], apps[256], script[320], id[65];
    const char *err = NULL;
    const char *bridge =
        "#!/bin/sh\n"
        "# LX06-AUDIO-MODE: phone-bridge\n"
        "# LX06-BRIDGE: on\n"
        "# LX06-OFFICIALWAKEUP: on\n"
        "# LX06-PHONE-APPS: doubao,qishui\n"
        "# LX06-DOUBAO-AUDIO: mic=open,speaker=open,ring=open\n"
        "# LX06-QISHUI-AUDIO: mic=close,speaker=open,ring=close\n"
        "# LX06-HANGUP-DOUBAO: 挂断|结束了\n"
        "# LX06-EXIT-QISHUI: 退出|别播了\n"
        "# LX06-LOCAL-CHANNEL:\n";
    const char *empty_exit =
        "#!/bin/sh\n# LX06-BRIDGE: on\n# LX06-PHONE-APPS: doubao,qishui\n"
        "# LX06-DOUBAO-AUDIO: mic=open\n# LX06-QISHUI-AUDIO: mic=close\n"
        "# LX06-HANGUP-DOUBAO:\n# LX06-EXIT-QISHUI:\n";
    FILE *f;
    snprintf(bind, sizeof(bind), "%s/bind", dir);
    snprintf(apps, sizeof(apps), "%s/apps", dir);
    assert(!mkdir(apps, 0755) || errno == EEXIST);
    snprintf(script, sizeof(script), "%s/doubao.sh", apps);
    f = fopen(script, "w"); assert(f); fputs(bridge, f); fclose(f);
    assert(lx06_preempt_allowed(0) == 0);
    assert(lx06_preempt_allowed(1) == 1);
    assert(lx06_route_official(0, LX06_BIND_ARMED, 0) == LX06_OFFICIAL_STOCK);
    assert(lx06_route_official(1, LX06_BIND_NONE, 1) == LX06_OFFICIAL_STOCK);
    assert(lx06_route_official(1, LX06_BIND_ARMED, 0) == LX06_OFFICIAL_PASSTHROUGH);
    assert(lx06_route_official(1, LX06_BIND_ARMED, 1) == LX06_OFFICIAL_LISTEN);
    assert(lx06_official_bind_decide_at(bind, apps, id, sizeof(id)) == LX06_BIND_NONE);
    assert(!lx06_official_bind_write_at(bind, "doubao.sh"));
    assert(lx06_official_bind_decide_at(bind, apps, id, sizeof(id)) == LX06_BIND_ARMED);
    assert(!strcmp(id, "doubao.sh"));
    assert(!lx06_local_channel_armed_at(apps, "doubao.sh"));
    unlink(script);
    assert(lx06_official_bind_decide_at(bind, apps, id, sizeof(id)) == LX06_BIND_CLEARED);
    assert(lx06_official_bind_read_at(bind, id, sizeof(id)) == 0);
    assert(lx06_exit_phrases_ok(bridge, strlen(bridge), 0, &err) == 0);
    assert(lx06_exit_phrases_ok(empty_exit, strlen(empty_exit), 1, &err) == 0);
    assert(lx06_exit_phrases_ok(empty_exit, strlen(empty_exit), 0, &err) == 1);
    assert(lx06_exit_phrases_ok("#!/bin/sh\necho hi\n", 18, 0, &err) == 0);
}

int main(void) {
    char dir[] = "/tmp/lx06-wt1-XXXXXX";
    assert(mkdtemp(dir));
    test_matcher();
    test_catalog();
    test_window();
    test_bind_and_exit(dir);
    printf("wake-channel selftest passed\n");
    return 0;
}

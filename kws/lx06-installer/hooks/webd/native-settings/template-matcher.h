/* One firmware template matcher (plan §4.1). Official routing and the local
 * channel both call this. Bridge scripts must not keep a second regex.
 * WT2 session control calls the same function with that script's candidates.
 * wake text is never written to a log by this module. */
#ifndef LX06_TEMPLATE_MATCHER_H
#define LX06_TEMPLATE_MATCHER_H
#include <stddef.h>
/* This complete native matcher coexists with the unchanged stock Web matcher. */
#define lx06_phrase_normalize lx06_native_phrase_normalize
#define lx06_template_match lx06_native_template_match

/* Same normalization as the historical official router: drop every space and
 * the punctuation 。！？!?.，,；：、 plus ASCII .!?;: anywhere in the sentence.
 * Output is at most cap-1 bytes. Returns 1 when the result is non-empty. */
int lx06_phrase_normalize(const char *in, char *out, size_t cap);

enum lx06_match_kind {
    LX06_MATCH_NONE = 0,
    LX06_MATCH_EXACT = 1,
    LX06_MATCH_TEMPLATE = 2,
    LX06_MATCH_REJECT = 3,      /* cross-script equal fixed length */
    LX06_MATCH_INCOMPLETE = 4,  /* a required variable would be empty */
    LX06_MATCH_AMBIGUOUS = 5    /* two scripts share one exact phrase */
};

struct lx06_match_candidate {
    const char *script; /* NULL and "" are the same script. */
    int line;           /* smaller line wins inside one script */
    const char *phrase; /* exact text, or a {variable} template */
};

struct lx06_match_var {
    char name[17];
    char value[257];
};

struct lx06_match_result {
    int kind;
    int index;       /* candidate index, or -1 */
    int fixed_chars; /* literals only; variables do not count */
    int var_count;
    struct lx06_match_var vars[4];
    char prompt[96];
};

/* Host and ARM tests assert a single match stays under this ceiling.
 * The work is a few dozen anchored comparisons; 2 ms is loose on purpose. */
#define LX06_MATCH_DEADLINE_US 2000

#define LX06_PROMPT_REJECT "有两个功能都能处理这句话，请换个说法"
#define LX06_PROMPT_TIMEOUT "没听到，请再说一次唤醒词"
#define LX06_PROMPT_ASR "没听清，请再说一次"
#define LX06_PROMPT_MISS "这句我还不会"
#define LX06_PROMPT_REASK "你要听什么歌？"
#define LX06_PROMPT_REASK_END "没听清，请再说一遍"

/* 0 = result filled (including NONE). -1 = bad arguments.
 * Exact phrases win before any template. Equal fixed length inside one
 * script follows line order. Equal fixed length across scripts is REJECT
 * and does not prefer install order. {query} is 1..64 characters;
 * {artist} and {song} are 1..32; any other {name} is 1..64. */
int lx06_template_match(const char *text, const struct lx06_match_candidate *cands,
                        int count, struct lx06_match_result *out);

/* Canonical qishui rows (§4.3). The generator self-check compares these
 * bytes after the three separators; it does not live in the matcher. */
#define LX06_QISHUI_ROWS 8
static const char *const lx06_qishui_rows[LX06_QISHUI_ROWS] = {
    "我要听{artist}的{song}",
    "播放{artist}的{song}",
    "听{artist}的{song}",
    "我要听{artist}的歌",
    "我要听{query}歌",
    "我要听{query}",
    "来一首{query}",
    "播放{query}"
};
#define LX06_QISHUI_CONN "放歌|我要听歌|来首歌"
#endif

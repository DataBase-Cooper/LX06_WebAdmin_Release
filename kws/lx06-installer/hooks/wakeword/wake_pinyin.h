#ifndef WAKE_PINYIN_H
#define WAKE_PINYIN_H
#include <stddef.h>
/* Toneless syllables separated by spaces. Returns the syllable count, or -1
 * when any hanzi has no table entry (callers must not discard a wake on that). */
int wake_pinyin_phrase(const char *utf8, char *out, size_t cap);
/* Recording-time text warning. >= WAKE_PINYIN_WARN means "too close". */
float wake_pinyin_warn_similarity(const char *left, const char *right);
/* Cloud ASR homophone match, including the measured 小吱/小只 and 古维斯 cases.
 * Retired from the wake gate: it compared ASR text with the typed label and
 * would accept a different word. Kept only so the old measurement stays testable. */
int wake_pinyin_asr_match(const char *heard, const char *enrolled);
/* Canonical toneless key, syllables separated by one space. Skips whitespace,
 * punctuation and any non-ideograph. Returns the syllable count, 0 when none,
 * -1 when an ideograph has no table entry (do not use out as a reference). */
int wake_pinyin_normalize(const char *utf8, char *out, size_t cap);
/* Exact key match. Both sides must already be canonical keys of at least two
 * syllables. No homophone substitution and no partial overlap. */
int wake_pinyin_key_equal(const char *left, const char *right);
#endif

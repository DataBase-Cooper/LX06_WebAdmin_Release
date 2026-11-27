#include "../src/text_match.h"

#include <assert.h>
#include <stdio.h>

static void test_exact_chinese_transcription_matches_played_text(void) {
    assert(xz_text_is_echo("你好，小智。", "你好，小智。") == 1);
}

static void test_punctuation_differences_do_not_hide_self_echo(void) {
    assert(xz_text_is_echo("你好小智", "你好，小智！") == 1);
}

static void test_nonmatching_transcript_is_not_suppressed_as_echo(void) {
    assert(xz_text_is_echo("今天天气怎么样", "你好，小智！") == 0);
}

static void test_short_common_words_are_not_classified_as_echo(void) {
    assert(xz_text_is_echo("好", "好的") == 0);
}

static void test_empty_or_null_text_is_not_an_echo(void) {
    assert(xz_text_is_echo("", "正在播报的回答") == 0);
    assert(xz_text_is_echo(NULL, "正在播报的回答") == 0);
    assert(xz_text_is_echo("正在播报的回答", NULL) == 0);
}

static void test_configured_phrase_matches_exact_transcription(void) {
    assert(xz_text_matches_phrase("小智再见", "小智再见") == 1);
}

static void test_phrase_match_ignores_punctuation(void) {
    assert(xz_text_matches_phrase("小智，再见。", "小智再见") == 1);
}

static void test_bare_zaijian_does_not_match_longer_phrase(void) {
    assert(xz_text_matches_phrase("再见", "小智再见") == 0);
}

static void test_asr_particle_suffix_still_matches_phrase(void) {
    /* Field ASR appends particles; the phrase must still be found inside. */
    assert(xz_text_matches_phrase("小智再见的", "小智再见") == 1);
    assert(xz_text_matches_phrase("小智，再见的。", "小智再见") == 1);
    assert(xz_text_matches_phrase("小智再见吗", "小智再见") == 1);
}

static void test_longer_transcription_without_phrase_does_not_match(void) {
    assert(xz_text_matches_phrase("小智在见", "小智再见") == 0);
    assert(xz_text_matches_phrase("再说一遍", "小智再见") == 0);
}

static void test_phrase_match_ignores_ascii_case(void) {
    assert(xz_text_matches_phrase("bye", "BYE") == 1);
}

static void test_interrupt_list_matches_any_comma_separated_phrase(void) {
    assert(xz_text_matches_list("等一下", "等一下,等等,停一下,你说的不对") == 1);
    assert(xz_text_matches_list("你说的不对啊", "等一下,等等,停一下,你说的不对") == 1);
    assert(xz_text_matches_list("电视新闻", "等一下,等等,停一下,你说的不对") == 0);
    assert(xz_text_matches_list("小智再见的", "小智再见,再见小智,拜拜了哦") == 1);
    assert(xz_text_matches_list("再见", "小智再见,再见小智") == 0);
    assert(xz_text_matches_list("等等", "等一下，等等") == 1);
    assert(xz_text_matches_list("", "等一下") == 0);
    assert(xz_text_matches_list("等一下", "") == 0);
}

static void test_phrase_match_rejects_empty_or_null(void) {
    assert(xz_text_matches_phrase("", "小智再见") == 0);
    assert(xz_text_matches_phrase(NULL, "小智再见") == 0);
    assert(xz_text_matches_phrase("小智再见", "") == 0);
    assert(xz_text_matches_phrase("小智再见", NULL) == 0);
}

int main(void) {
    test_exact_chinese_transcription_matches_played_text();
    test_punctuation_differences_do_not_hide_self_echo();
    test_nonmatching_transcript_is_not_suppressed_as_echo();
    test_short_common_words_are_not_classified_as_echo();
    test_empty_or_null_text_is_not_an_echo();
    test_configured_phrase_matches_exact_transcription();
    test_phrase_match_ignores_punctuation();
    test_bare_zaijian_does_not_match_longer_phrase();
    test_asr_particle_suffix_still_matches_phrase();
    test_longer_transcription_without_phrase_does_not_match();
    test_phrase_match_ignores_ascii_case();
    test_interrupt_list_matches_any_comma_separated_phrase();
    test_phrase_match_rejects_empty_or_null();
    puts("text match tests: PASS");
    return 0;
}

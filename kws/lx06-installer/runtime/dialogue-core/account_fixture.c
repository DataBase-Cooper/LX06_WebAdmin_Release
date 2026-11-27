/* Offline parser/serialization/segmentation boundary fixture. Including the
 * module exposes pure internal functions only to this test translation unit.
 * No script, cloud, ASR, microphone, SDK, playback or device is used. */
#include "account.c"
#include <assert.h>

int main(void) {
    unsigned checks = 0;
    dc_account *a = dc_account_create(); assert(a); ++checks;
    dc_account_event event;
    const char *good[] = {"{\"delta\":\"北京故宫。\"}",
        "{\"delta\":\"\\u4e2d\\u6587\\ud83d\\ude42\"}",
        "{\"done\":true}", "{\"done\":1}", "{\"error\":true}",
        "{\"error\":\"平台限制\"}", "{\"message\":\"请先登录账户。\"}"};
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; ++i) {
        memset(&event, 0, sizeof event);
        assert(!parse_line(good[i], strlen(good[i]), &event)); ++checks;
    }
    const char *bad[] = {"{\"delta\":\"x\",\"done\":true}",
        "{\"delta\":\"x\",\"delta\":\"y\"}", "{\"done\":false}",
        "{\"done\":1.0}", "{\"delta\":\"\\ud800\"}",
        "{\"delta\":\"\\u0000\"}", "{\"error\":\"bad\\nnotice\"}",
        "{\"unknown\":true}"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        memset(&event, 0, sizeof event);
        assert(parse_line(bad[i], strlen(bad[i]), &event)); ++checks;
    }
    assert(!make_request(a, "请用两句话介绍北京故宫，保留后半句。", "user-selected-model")); ++checks;
    assert(a->input_len < sizeof a->input && strstr(a->input, "user-selected-model")); ++checks;
    a->tag = (dc_tag){17, 23, 31};
    strcpy(a->answer, "北京故宫位于北京市中心，随后内容必须连续。🙂");
    a->answer_len = strlen(a->answer); a->protocol_done = 1; a->next_sequence = 1;
    assert(dc_account_phrase(a, 300, &event) == 1); ++checks;
    assert(event.type == DC_ACCOUNT_PHRASE && event.tag.session_epoch == 17 &&
           event.tag.reply_generation == 23 && event.tag.utterance_id == 31 &&
           event.length <= 72 && valid_utf8((unsigned char *)event.text, event.length)); ++checks;
    size_t consumed = event.length;
    while (!dc_account_drained(a)) {
        assert(dc_account_phrase(a, 300, &event) == 1); ++checks;
        assert(event.length <= 240 && valid_utf8((unsigned char *)event.text, event.length)); ++checks;
        consumed += event.length;
    }
    assert(consumed == a->answer_len); ++checks;
    strcpy(a->question, "完整问题。"); commit_history(a);
    assert(dc_account_history_count(a) == 1 && !strcmp(a->history[0].question, "完整问题。")); ++checks;
    assert(!dc_account_cancel(a, (dc_tag){18, 23, 31}, 400)); ++checks;
    assert(dc_account_cancel(a, a->tag, 400) == 1); ++checks;
    assert(!dc_account_cancel(a, a->tag, 400) && dc_account_drained(a)); ++checks;
    assert(dc_account_history_count(a) == 1); ++checks;
    size_t storage = dc_account_storage_bytes();
    assert(storage <= 80 * 1024); ++checks;
    assert(dc_account_destroy(a) == 0); ++checks;
    printf("{\"passed\":true,\"assertions\":%u,\"storage_bytes\":%zu,"
           "\"scope\":\"pure account parser/request/phrase ABI\","
           "\"actual_audio\":false,\"provider_requests\":0}\n", checks, storage);
    return 0;
}

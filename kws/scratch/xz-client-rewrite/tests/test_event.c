#include "../src/event.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_tts_start_is_parsed_without_retaining_spoken_text(void) {
    const char json[] = "{\"type\":\"tts\",\"state\":\"start\",\"text\":\"hello private text\"}";
    xz_server_event event;
    assert(xz_event_parse_text(json, strlen(json), &event) == 0);
    assert(event.type == XZ_EVENT_TTS);
    assert(strcmp(event.state, "start") == 0);
    assert(event.text_bytes > 0);
    assert(strcmp(event.text, "hello private text") == 0);
    assert(event.emotion[0] == '\0');
    assert(event.reason[0] == '\0');
}

static void test_stt_text_is_retained_for_echo_suppression(void) {
    const char json[] = "{\"type\":\"stt\",\"text\":\"今天的天气怎么样？\"}";
    xz_server_event event;
    assert(xz_event_parse_text(json, strlen(json), &event) == 0);
    assert(event.type == XZ_EVENT_STT);
    assert(strcmp(event.text, "今天的天气怎么样？") == 0);
    assert(event.text_bytes == strlen(event.text));
}

static void test_oversized_transcript_is_rejected_without_partial_text(void) {
    char json[700];
    size_t prefix = (size_t)snprintf(json, sizeof(json), "{\"type\":\"stt\",\"text\":\"");
    memset(json + prefix, 'x', 520);
    memcpy(json + prefix + 520, "\"}", 3);
    xz_server_event event;
    assert(xz_event_parse_text(json, prefix + 523, &event) != 0);
    assert(event.text[0] == '\0');
}

int main(void) {
    test_tts_start_is_parsed_without_retaining_spoken_text();
    test_stt_text_is_retained_for_echo_suppression();
    test_oversized_transcript_is_rejected_without_partial_text();
    puts("event tests: PASS");
    return 0;
}

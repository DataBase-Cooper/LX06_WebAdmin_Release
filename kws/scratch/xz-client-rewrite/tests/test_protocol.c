#include "../src/json.h"
#include "../src/protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int json_bool(const xz_json_doc *doc, int object, const char *key, int *value) {
    int token = xz_json_object_get(doc, object, key);
    if (token < 0) return -1;
    const xz_json_token *item = &doc->tokens[token];
    size_t length = item->end - item->start;
    if (length == 4 && memcmp(doc->source + item->start, "true", 4) == 0) {
        *value = 1;
        return 0;
    }
    if (length == 5 && memcmp(doc->source + item->start, "false", 5) == 0) {
        *value = 0;
        return 0;
    }
    return -1;
}

static void assert_string(xz_json_doc *doc, int object, const char *key, const char *expected) {
    char value[128];
    assert(xz_json_get_string(doc, object, key, value, sizeof(value)) == 0);
    assert(strcmp(value, expected) == 0);
}

static void test_feishu_v1_hello_uses_exact_audio_contract(void) {
    char json[512];
    assert(xz_protocol_build_hello(json, sizeof(json), 0, 0) == 0);
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    int number = 0;
    assert_string(&doc, doc.root, "type", "hello");
    assert(xz_json_get_int(&doc, doc.root, "version", &number) == 0 && number == 1);
    assert_string(&doc, doc.root, "transport", "websocket");
    int features = xz_json_object_get(&doc, doc.root, "features");
    assert(features < 0);
    int audio = xz_json_object_get(&doc, doc.root, "audio_params");
    assert(audio >= 0);
    assert_string(&doc, audio, "format", "opus");
    assert(xz_json_get_int(&doc, audio, "sample_rate", &number) == 0 && number == 16000);
    assert(xz_json_get_int(&doc, audio, "channels", &number) == 0 && number == 1);
    assert(xz_json_get_int(&doc, audio, "frame_duration", &number) == 0 && number == 60);
    xz_json_destroy(&doc);
}

static void test_hello_advertises_aec_only_when_enabled(void) {
    char json[512];
    assert(xz_protocol_build_hello(json, sizeof(json), 1, 0) == 0);
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    int features = xz_json_object_get(&doc, doc.root, "features");
    int flag = 0;
    assert(features >= 0);
    assert(json_bool(&doc, features, "aec", &flag) == 0 && flag == 1);
    xz_json_destroy(&doc);
}

static void test_missing_server_session_id_is_sent_as_empty_string(void) {
    char json[256];
    assert(xz_protocol_build_listen(json, sizeof(json), NULL, "start", "auto") == 0);
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    assert_string(&doc, doc.root, "session_id", "");
    assert_string(&doc, doc.root, "type", "listen");
    assert_string(&doc, doc.root, "state", "start");
    assert_string(&doc, doc.root, "mode", "auto");
    xz_json_destroy(&doc);
}

static void test_server_hello_uses_24khz_downlink_and_optional_fields(void) {
    const char json[] = "{\"type\":\"hello\",\"transport\":\"websocket\","
                        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,"
                        "\"channels\":1,\"frame_duration\":60}}";
    xz_hello hello = {0};
    char session_id[96] = "not-empty";
    assert(xz_protocol_parse_server_hello(json, strlen(json), &hello,
                                          session_id, sizeof(session_id)) == 0);
    assert(hello.version == 1);
    assert(hello.sample_rate == 24000);
    assert(hello.channels == 1 && hello.frame_duration_ms == 60);
    assert(strcmp(hello.transport, "websocket") == 0);
    assert(strcmp(hello.format, "opus") == 0);
    assert(hello.session_id == session_id);
    assert(session_id[0] == '\0');
}

static void test_listen_accepts_realtime_mode(void) {
    char json[256];
    assert(xz_protocol_build_listen(json, sizeof(json), "s1", "start", "realtime") == 0);
    assert(strstr(json, "\"mode\":\"realtime\"") != NULL);
}

static void test_abort_message_escapes_session_id_and_reason(void) {
    char json[256];
    assert(xz_protocol_build_abort(json, sizeof(json), "s\"\\1", "wake_word_detected") == 0);
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    assert_string(&doc, doc.root, "session_id", "s\"\\1");
    assert_string(&doc, doc.root, "type", "abort");
    assert_string(&doc, doc.root, "reason", "wake_word_detected");
    xz_json_destroy(&doc);
}

static void test_goodbye_message_carries_session_id(void) {
    char json[256];
    assert(xz_protocol_build_goodbye(json, sizeof(json), "local-test-session") == 0);
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    assert_string(&doc, doc.root, "session_id", "local-test-session");
    assert_string(&doc, doc.root, "type", "goodbye");
    xz_json_destroy(&doc);
}

static void test_goodbye_message_escapes_session_id_and_defaults_empty(void) {
    char json[256];
    assert(xz_protocol_build_goodbye(json, sizeof(json), "s\"\\1") == 0);
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    assert_string(&doc, doc.root, "session_id", "s\"\\1");
    assert_string(&doc, doc.root, "type", "goodbye");
    xz_json_destroy(&doc);
    assert(xz_protocol_build_goodbye(json, sizeof(json), NULL) == 0);
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    assert_string(&doc, doc.root, "session_id", "");
    xz_json_destroy(&doc);
}

int main(void) {
    test_feishu_v1_hello_uses_exact_audio_contract();
    test_hello_advertises_aec_only_when_enabled();
    test_missing_server_session_id_is_sent_as_empty_string();
    test_server_hello_uses_24khz_downlink_and_optional_fields();
    test_listen_accepts_realtime_mode();
    test_abort_message_escapes_session_id_and_reason();
    test_goodbye_message_carries_session_id();
    test_goodbye_message_escapes_session_id_and_defaults_empty();
    puts("protocol tests: PASS");
    return 0;
}

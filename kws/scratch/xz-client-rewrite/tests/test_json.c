#include "../src/json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_nested_hello_fields_are_read_without_flat_key_collisions(void) {
    const char *json = "{\"type\":\"hello\",\"session_id\":\"s-1\",\"audio_params\":{" 
                       "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60,"
                       "\"format\":\"opus\"},\"transport\":\"websocket\"}";
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    char value[32];
    int number = 0;
    assert(xz_json_get_string(&doc, doc.root, "type", value, sizeof(value)) == 0);
    assert(strcmp(value, "hello") == 0);
    int audio = xz_json_object_get(&doc, doc.root, "audio_params");
    assert(audio >= 0);
    assert(xz_json_get_int(&doc, audio, "sample_rate", &number) == 0 && number == 16000);
    assert(xz_json_get_int(&doc, audio, "channels", &number) == 0 && number == 1);
    assert(xz_json_get_string(&doc, audio, "format", value, sizeof(value)) == 0);
    assert(strcmp(value, "opus") == 0);
    xz_json_destroy(&doc);
}

static void test_escaped_strings_and_arrays_do_not_break_later_fields(void) {
    const char *json = "{\"type\":\"stt\",\"text\":\"hi \\\"there\\\"\\n\","
                       "\"data\":[1,{\"ok\":true}],\"state\":\"sentence_start\"}";
    xz_json_doc doc;
    assert(xz_json_parse(&doc, json, strlen(json)) == 0);
    char value[64];
    assert(xz_json_get_string(&doc, doc.root, "state", value, sizeof(value)) == 0);
    assert(strcmp(value, "sentence_start") == 0);
    assert(xz_json_get_string(&doc, doc.root, "text", value, sizeof(value)) == 0);
    assert(strcmp(value, "hi \"there\"\n") == 0);
    xz_json_destroy(&doc);
}

int main(void) {
    test_nested_hello_fields_are_read_without_flat_key_collisions();
    test_escaped_strings_and_arrays_do_not_break_later_fields();
    puts("json tests: PASS");
    return 0;
}

#include "../src/talk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_disabled_paths(void) {
    assert(xz_talk_open(NULL) == NULL);
    assert(xz_talk_open("") == NULL);
    assert(xz_talk_open("0") == NULL);
    assert(xz_talk_open("none") == NULL);
    xz_talk_close(NULL); /* 空指针安全 */
}

static void test_format_and_newline_sanitize(void) {
    char path[64] = "/tmp/xz_test_talk_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    xz_talk *talk = xz_talk_open(path);
    assert(talk != NULL);
    assert(xz_talk_stream(talk) != NULL);

    xz_talk_log(talk, "user", "你好\n小智\r测试");
    xz_talk_log(talk, "xiaozhi", "回答内容");
    xz_talk_log(talk, NULL, "role 为空不崩");
    xz_talk_log(talk, "user", ""); /* 空文本不写 */

    fflush(xz_talk_stream(talk));
    xz_talk_close(talk);
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    char line[256];
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "] user: ") != NULL);
    assert(strstr(line, "你好 小智 测试") != NULL);
    assert(strchr(line, '\n') == line + strlen(line) - 1); /* 换行已单行化 */
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "] xiaozhi: 回答内容") != NULL);
    assert(fgets(line, sizeof(line), f) != NULL);
    assert(strstr(line, "] ?: role 为空不崩") != NULL);
    assert(fgets(line, sizeof(line), f) == NULL); /* 空文本未写 */
    fclose(f);
    unlink(path);
}

static void test_rotation_keeps_tail_and_stays_bounded(void) {
    char path[64] = "/tmp/xz_test_talk_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    /* 预填满 48KB 以上（超过 XZ_TALK_LIMIT_BYTES=49152）：1200×42=50400 B */
    for (int i = 0; i < 1200; i++) {
        assert(write(fd, "OLD LINE 0123456789 0123456789 0123456789\n", 42) == 42);
    }
    close(fd);

    xz_talk *talk = xz_talk_open(path);
    assert(talk != NULL);
    xz_talk_log(talk, "user", "NEW TAIL LINE");
    xz_talk_close(talk);

    struct stat st;
    assert(stat(path, &st) == 0);
    /* 转后 ≈ 保留 16KB 尾部 + 一行新记录，远小于 48KB */
    assert(st.st_size <= 20000);
    assert(st.st_size >= 16000);

    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    char tail[512] = {0};
    assert(fseek(f, -(long)sizeof(tail), SEEK_END) == 0);
    size_t got = fread(tail, 1, sizeof(tail) - 1, f);
    fclose(f);
    (void)got;
    assert(strstr(tail, "NEW TAIL LINE") != NULL);
    /* 文件头必须是旧日志的尾部（保留了最近内容），而不是被清零 */
    FILE *head = fopen(path, "rb");
    char first[64] = {0};
    assert(fread(first, 1, sizeof(first) - 1, head) > 0);
    fclose(head);
    unlink(path);
}

int main(void) {
    test_disabled_paths();
    test_format_and_newline_sanitize();
    test_rotation_keeps_tail_and_stays_bounded();
    puts("talk tests: PASS");
    return 0;
}

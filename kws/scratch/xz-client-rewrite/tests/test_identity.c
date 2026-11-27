#define _POSIX_C_SOURCE 200809L
#include "../src/identity.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int is_uuid(const char *value) {
    if (strlen(value) != 36) return 0;
    for (size_t index = 0; index < 36; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return 0;
        } else if (!isxdigit((unsigned char)value[index])) {
            return 0;
        }
    }
    return value[14] == '4' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

static void test_missing_identity_file_generates_and_persists_uuid(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/xz-client-id-test-%ld", (long)getpid());
    unlink(path);

    char generated[37];
    assert(xz_client_id_load_or_create(path, generated, sizeof(generated)) == 0);
    assert(is_uuid(generated));

    FILE *file = fopen(path, "r");
    assert(file != NULL);
    char on_disk[64];
    assert(fgets(on_disk, sizeof(on_disk), file) != NULL);
    fclose(file);
    on_disk[strcspn(on_disk, "\r\n")] = '\0';
    assert(strcmp(on_disk, generated) == 0);

    char loaded[37];
    assert(xz_client_id_load_or_create(path, loaded, sizeof(loaded)) == 0);
    assert(strcmp(loaded, generated) == 0);
    unlink(path);
}

int main(void) {
    test_missing_identity_file_generates_and_persists_uuid();
    puts("identity tests: PASS");
    return 0;
}

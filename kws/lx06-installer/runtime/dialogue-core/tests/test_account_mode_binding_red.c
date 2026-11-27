/* Offline RED: production configure() does not bind mode to account bytes.
 * This is a controller seam probe, not an end-to-end launcher acceptance test.
 * Build: gcc -std=c11 -ffunction-sections -fdata-sections \
 *   tests/test_account_mode_binding_red.c account.c -Wl,--gc-sections -lm -o "$TMPDIR/mode-red"
 * Run only with a private synthetic environment; exit 1 is the expected RED.
 */
#define main dialogue_production_main
#include "../main.c"
#undef main
#include <stdio.h>

int main(void) {
    static const char keyword[] = "#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\nexit 0\n";
    dc_config config;
    int mode = dc_account_parse_declaration(keyword, sizeof keyword - 1);
    if (mode != DC_ACCOUNT_KEYWORD || setenv("MODEL_BARGE_ANY", "1", 1) || configure(&config, "MODEL")) {
        fprintf(stderr, "fixture setup failed\n");
        return 2;
    }
    if (config.barge_any != 0) {
        fprintf(stderr, "RED: keyword declaration but actual controller config barge_any=%d from settings\n", config.barge_any);
        return 1;
    }
    return 0;
}

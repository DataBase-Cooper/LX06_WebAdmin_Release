#ifndef LX06_SCRIPT_AUDIO_MODE_H
#define LX06_SCRIPT_AUDIO_MODE_H

#include <stddef.h>
#include <string.h>

/* Upload-script metadata, never executable shell input. Keep the same bounded
 * parser in the Web list and dialogue launcher so a phone bridge cannot be
 * mistaken for the cloud end-to-end client. Unknown/conflicting declarations
 * are invalid; absence remains unspecified for existing upload scripts. */
enum lx06_script_audio_mode {
    LX06_AUDIO_UNSPECIFIED = 0, LX06_AUDIO_CLOUD_E2E,
    LX06_AUDIO_ACCOUNT_TEXT, LX06_AUDIO_PHONE_BRIDGE, LX06_AUDIO_ACTION,
    LX06_AUDIO_INVALID
};
enum lx06_script_phone_app {
    LX06_PHONE_NONE = 0, LX06_PHONE_DOUBAO, LX06_PHONE_GROK,
    LX06_PHONE_CHATGPT, LX06_PHONE_INVALID
};
struct lx06_script_audio_metadata {
    enum lx06_script_audio_mode mode;
    enum lx06_script_phone_app phone_app;
};
static inline int lx06_meta_equals(const unsigned char *p, size_t n, const char *s) {
    return n == strlen(s) && !memcmp(p, s, n);
}
static inline struct lx06_script_audio_metadata
lx06_script_audio_parse(const unsigned char *p, size_t n) {
    struct lx06_script_audio_metadata result = {LX06_AUDIO_UNSPECIFIED, LX06_PHONE_NONE};
    const char *mode_key = "# LX06-AUDIO-MODE: ";
    const char *app_key = "# LX06-PHONE-APP: ";
    const char *apps_key = "# LX06-PHONE-APPS: ";
    if (!p) { result.mode = LX06_AUDIO_INVALID; return result; }
    if (n > 8192) n = 8192;
    for (size_t at = 0; at < n && p[at];) {
        size_t end = at;
        while (end < n && p[end] && p[end] != '\n') end++;
        /* A declaration cut by the header budget is never accepted. */
        size_t len = end - at;
        while (len && (p[at+len-1] == '\r' || p[at+len-1] == ' ' || p[at+len-1] == '\t')) len--;
        if (len >= strlen(mode_key) && !memcmp(p+at, mode_key, strlen(mode_key))) {
            const unsigned char *v = p+at+strlen(mode_key); size_t size = len-strlen(mode_key);
            enum lx06_script_audio_mode value = LX06_AUDIO_INVALID;
            if (lx06_meta_equals(v,size,"cloud-e2e")) value = LX06_AUDIO_CLOUD_E2E;
            else if (lx06_meta_equals(v,size,"account-text")) value = LX06_AUDIO_ACCOUNT_TEXT;
            else if (lx06_meta_equals(v,size,"phone-bridge")) value = LX06_AUDIO_PHONE_BRIDGE;
            else if (lx06_meta_equals(v,size,"action")) value = LX06_AUDIO_ACTION;
            if (end == n || (result.mode != LX06_AUDIO_UNSPECIFIED && result.mode != value)) value = LX06_AUDIO_INVALID;
            result.mode = value;
        } else if ((len >= strlen(app_key) && !memcmp(p+at, app_key, strlen(app_key))) ||
                   (len >= strlen(apps_key) && !memcmp(p+at, apps_key, strlen(apps_key)))) {
            /* The native bridge declaration uses the plural spelling. This
             * UI/router helper accepts one app only; native matching remains
             * authoritative and multi-app/conflicting values stay invalid. */
            size_t key = len >= strlen(apps_key) && !memcmp(p+at, apps_key, strlen(apps_key)) ? strlen(apps_key) : strlen(app_key);
            const unsigned char *v = p+at+key; size_t size = len-key;
            enum lx06_script_phone_app value = LX06_PHONE_INVALID;
            if (lx06_meta_equals(v,size,"doubao")) value = LX06_PHONE_DOUBAO;
            else if (lx06_meta_equals(v,size,"grok")) value = LX06_PHONE_GROK;
            else if (lx06_meta_equals(v,size,"chatgpt")) value = LX06_PHONE_CHATGPT;
            if (end == n || (result.phone_app != LX06_PHONE_NONE && result.phone_app != value)) value = LX06_PHONE_INVALID;
            result.phone_app = value;
        }
        if (end >= n || !p[end]) break;
        at = end+1;
    }
    if (result.phone_app == LX06_PHONE_INVALID ||
        (result.mode == LX06_AUDIO_PHONE_BRIDGE && result.phone_app == LX06_PHONE_NONE) ||
        (result.phone_app != LX06_PHONE_NONE && result.mode != LX06_AUDIO_PHONE_BRIDGE))
        result.mode = LX06_AUDIO_INVALID;
    return result;
}
static inline const char *lx06_script_audio_name(enum lx06_script_audio_mode mode) {
    switch (mode) {
    case LX06_AUDIO_UNSPECIFIED: return "";
    case LX06_AUDIO_CLOUD_E2E: return "cloud-e2e";
    case LX06_AUDIO_ACCOUNT_TEXT: return "account-text";
    case LX06_AUDIO_PHONE_BRIDGE: return "phone-bridge";
    case LX06_AUDIO_ACTION: return "action";
    default: return "invalid";
    }
}
#endif

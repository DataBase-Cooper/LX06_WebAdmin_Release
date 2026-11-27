#include "text_match.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#define XZ_TEXT_CODEPOINT_CAP 512u

static int is_ignored(uint32_t cp) {
    if (cp <= 0x7fu) {
        return isspace((unsigned char)cp) ||
               (cp < 0x20u) ||
               (cp >= 0x21u && cp <= 0x2fu) ||
               (cp >= 0x3au && cp <= 0x40u) ||
               (cp >= 0x5bu && cp <= 0x60u) ||
               (cp >= 0x7bu && cp <= 0x7eu);
    }
    switch (cp) {
        case 0x2018: case 0x2019: case 0x201c: case 0x201d:
        case 0x3000: case 0x3001: case 0x3002:
        case 0x300a: case 0x300b: case 0x300c: case 0x300d:
        case 0x300e: case 0x300f: case 0x3010: case 0x3011:
        case 0xff01: case 0xff08: case 0xff09: case 0xff0c:
        case 0xff1a: case 0xff1b: case 0xff1f:
            return 1;
        default:
            return 0;
    }
}

static uint32_t decode_utf8(const unsigned char **cursor) {
    const unsigned char *p = *cursor;
    uint32_t cp;
    size_t count;
    if (p[0] < 0x80) { cp = p[0]; count = 1; }
    else if ((p[0] & 0xe0) == 0xc0) { cp = p[0] & 0x1f; count = 2; }
    else if ((p[0] & 0xf0) == 0xe0) { cp = p[0] & 0x0f; count = 3; }
    else if ((p[0] & 0xf8) == 0xf0) { cp = p[0] & 0x07; count = 4; }
    else { *cursor = p + 1; return 0xfffd; }
    for (size_t i = 1; i < count; ++i) {
        if ((p[i] & 0xc0) != 0x80 || p[i] == '\0') {
            *cursor = p + 1;
            return 0xfffd;
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    *cursor = p + count;
    return cp;
}

static size_t normalize(const char *text, uint32_t *out) {
    size_t length = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && length < XZ_TEXT_CODEPOINT_CAP) {
        uint32_t cp = decode_utf8(&p);
        if (cp < 128u) cp = (uint32_t)tolower((unsigned char)cp);
        if (!is_ignored(cp)) out[length++] = cp;
    }
    return length;
}

static int contains(const uint32_t *haystack, size_t haystack_len,
                    const uint32_t *needle, size_t needle_len) {
    if (needle_len > haystack_len) return 0;
    for (size_t i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len * sizeof(*needle)) == 0) return 1;
    }
    return 0;
}

static size_t substring_distance(const uint32_t *a, size_t a_len,
                                 const uint32_t *b, size_t b_len) {
    size_t previous[XZ_TEXT_CODEPOINT_CAP + 1];
    size_t current[XZ_TEXT_CODEPOINT_CAP + 1];
    /* A recognition final may contain only part of the playback sentence. */
    for (size_t j = 0; j <= b_len; ++j) previous[j] = 0;
    for (size_t i = 1; i <= a_len; ++i) {
        current[0] = i;
        for (size_t j = 1; j <= b_len; ++j) {
            size_t substitution = previous[j - 1] + (a[i - 1] != b[j - 1]);
            size_t deletion = previous[j] + 1;
            size_t insertion = current[j - 1] + 1;
            size_t best = substitution < deletion ? substitution : deletion;
            current[j] = best < insertion ? best : insertion;
        }
        memcpy(previous, current, (b_len + 1) * sizeof(*previous));
    }
    size_t distance=a_len;
    for(size_t j=1;j<=b_len;j++)if(previous[j]<distance)distance=previous[j];
    return distance;
}

/* At high volume an ASR final can keep the same spoken clause but omit
 * several words. Require both a shared phrase and substantial ordered text. */
static int ordered_echo(const uint32_t *heard, size_t heard_len,
                        const uint32_t *spoken, size_t spoken_len) {
    size_t matches[XZ_TEXT_CODEPOINT_CAP+1]={0}, runs[XZ_TEXT_CODEPOINT_CAP+1]={0};
    size_t longest=0;
    for(size_t i=0;i<heard_len;i++) {
        size_t previous_match=0,previous_run=0;
        for(size_t j=1;j<=spoken_len;j++) {
            size_t old_match=matches[j],old_run=runs[j];
            if(heard[i]==spoken[j-1]) {
                matches[j]=previous_match+1;
                runs[j]=previous_run+1;
                if(runs[j]>longest)longest=runs[j];
            } else {
                if(matches[j-1]>matches[j])matches[j]=matches[j-1];
                runs[j]=0;
            }
            previous_match=old_match;
            previous_run=old_run;
        }
    }
    return longest>=4 && matches[spoken_len]*3>=heard_len*2;
}

/* Normalized substring match: the phrase matches when it appears anywhere in
 * the observed text after punctuation/whitespace removal and lowercasing.
 * Real ASR appends particles ("小智再见的"), so an exact-length match would
 * miss the phrase in the field.  A 4-codepoint phrase inside a sentence is
 * specific enough that false positives are negligible; a bare "再见" still
 * never matches the phrase "小智再见". */
static int matches_phrase(const char *observed, const char *phrase, int exact) {
    if (!observed || !phrase || !*observed || !*phrase) return 0;
    uint32_t left[XZ_TEXT_CODEPOINT_CAP], right[XZ_TEXT_CODEPOINT_CAP];
    size_t left_len = normalize(observed, left);
    size_t right_len = normalize(phrase, right);
    if (left_len == 0 || right_len < 2) return 0;
    if (exact) return left_len == right_len &&
                      memcmp(left, right, left_len * sizeof(*left)) == 0;
    return contains(left, left_len, right, right_len);
}

int xz_text_matches_phrase(const char *observed, const char *phrase) {
    return matches_phrase(observed, phrase, 0);
}

static int list_sep(const unsigned char *s, size_t n, size_t *used) {
    if (n >= 1 && (s[0] == ',' || s[0] == '\n' || s[0] == '\r' || s[0] == ';')) {
        *used = 1;
        return 1;
    }
    if (n >= 3 && s[0] == 0xef && s[1] == 0xbc && s[2] == 0x8c) {
        *used = 3;
        return 1;
    }
    if (n >= 3 && s[0] == 0xe3 && s[1] == 0x80 && s[2] == 0x81) {
        *used = 3;
        return 1;
    }
    return 0;
}

static int matches_list(const char *observed, const char *list, int exact) {
    const unsigned char *p;
    size_t n, i;
    if (!observed || !list || !*observed || !*list) return 0;
    p = (const unsigned char *)list;
    n = strlen(list);
    i = 0;
    while (i < n) {
        size_t used = 0, start, end, len;
        char phrase[128];
        while (i < n && (p[i] == ' ' || p[i] == '\t' ||
                         list_sep(p + i, n - i, &used))) {
            i += used ? used : 1;
            used = 0;
        }
        if (i >= n) break;
        start = i;
        used = 0;
        while (i < n && !list_sep(p + i, n - i, &used)) i++;
        end = i;
        while (end > start && (p[end - 1] == ' ' || p[end - 1] == '\t')) end--;
        if (end <= start) continue;
        len = end - start;
        if (len >= sizeof(phrase)) len = sizeof(phrase) - 1;
        memcpy(phrase, p + start, len);
        phrase[len] = 0;
        if (matches_phrase(observed, phrase, exact)) return 1;
    }
    return 0;
}

int xz_text_matches_list(const char *observed, const char *list) {
    return matches_list(observed, list, 0);
}

int xz_text_matches_list_exact(const char *observed, const char *list) {
    return matches_list(observed, list, 1);
}

int xz_text_is_echo(const char *observed, const char *spoken) {
    if (!observed || !spoken || !*observed || !*spoken) return 0;
    uint32_t left[XZ_TEXT_CODEPOINT_CAP], right[XZ_TEXT_CODEPOINT_CAP];
    size_t left_len = normalize(observed, left);
    size_t right_len = normalize(spoken, right);
    if (left_len < 2 || right_len < 2) return 0;
    if (contains(right,right_len,left,left_len)) return 1;
    return (left_len>=4 && substring_distance(left,left_len,right,right_len)*4<=left_len) ||
           (left_len>=7 && ordered_echo(left,left_len,right,right_len));
}

int xz_text_has_speech(const char *text, unsigned minimum) {
    uint32_t normalized[XZ_TEXT_CODEPOINT_CAP];
    return text && normalize(text,normalized)>=minimum;
}

static int phrase_in_text(const char *phrase, const char *text) {
    uint32_t needle[XZ_TEXT_CODEPOINT_CAP], hay[XZ_TEXT_CODEPOINT_CAP];
    size_t nlen, hlen;
    if (!phrase || !text || !*phrase || !*text) return 0;
    nlen = normalize(phrase, needle);
    hlen = normalize(text, hay);
    if (nlen < 2 || hlen < 2) return 0;
    return contains(hay, hlen, needle, nlen);
}

static int list_self_hit(const char *observed, const char *spoken, const char *list) {
    const unsigned char *p;
    size_t n, i;
    if (!observed || !spoken || !list || !*list) return 0;
    p = (const unsigned char *)list;
    n = strlen(list);
    i = 0;
    while (i < n) {
        size_t used = 0, start, end, len;
        char phrase[128];
        while (i < n && (p[i] == ' ' || p[i] == '\t' ||
                         list_sep(p + i, n - i, &used))) {
            i += used ? used : 1;
            used = 0;
        }
        if (i >= n) break;
        start = i;
        used = 0;
        while (i < n && !list_sep(p + i, n - i, &used)) i++;
        end = i;
        while (end > start && (p[end - 1] == ' ' || p[end - 1] == '\t')) end--;
        if (end <= start) continue;
        len = end - start;
        if (len >= sizeof(phrase)) len = sizeof(phrase) - 1;
        memcpy(phrase, p + start, len);
        phrase[len] = 0;
        if (xz_text_matches_phrase(observed, phrase) &&
            phrase_in_text(phrase, spoken)) return 1;
    }
    return 0;
}

int xz_self_echo(const char *observed, const char *spoken,
                 const char *list1, const char *list2, const char *list3) {
    if (!observed || !spoken || !*observed || !*spoken) return 0;
    return list_self_hit(observed, spoken, list1)
        || list_self_hit(observed, spoken, list2)
        || list_self_hit(observed, spoken, list3);
}

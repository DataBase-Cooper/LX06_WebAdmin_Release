#ifndef XZ_TEXT_MATCH_H
#define XZ_TEXT_MATCH_H

int xz_text_is_echo(const char *observed, const char *spoken);
int xz_text_has_speech(const char *text, unsigned minimum);
int xz_text_matches_phrase(const char *observed, const char *phrase);
int xz_text_is_phrase(const char *observed, const char *phrase);
/* list is comma-separated. ASCII comma, fullwidth comma, or ideographic comma. */
int xz_text_matches_list(const char *observed, const char *list);
/* 1 if observed hits a phrase in any list AND that phrase is inside spoken.
 * Generic: Doubao e2e calls the same with its outbound text. Not volume. */
int xz_self_echo(const char *observed, const char *spoken,
                 const char *list1, const char *list2, const char *list3);

#endif

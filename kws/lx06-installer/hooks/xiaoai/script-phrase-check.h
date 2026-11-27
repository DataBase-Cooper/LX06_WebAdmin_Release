/* Save-time checks that belong to WT1: exit phrases while official wake is
 * off (§2.7), and cross-script phrase conflicts (§2.5). Session-control
 * caps checks stay with WT3. */
#ifndef LX06_SCRIPT_PHRASE_CHECK_H
#define LX06_SCRIPT_PHRASE_CHECK_H
#include "template-matcher.h"

/* 0 = accept. 1 = reject, message is a fixed Chinese sentence.
 * official_on skips the exit-phrase rule. Non-bridge scripts are accepted. */
int lx06_exit_phrases_ok(const char *header, size_t n, int official_on, const char **error);

/* 1 when another installed script shares an exact phrase or an equal-length
 * template that matches the same probe sentence. 0 = no conflict. */
int lx06_phrase_conflicts(const char *header, size_t n, const char *name, const char *apps_dir);
#endif

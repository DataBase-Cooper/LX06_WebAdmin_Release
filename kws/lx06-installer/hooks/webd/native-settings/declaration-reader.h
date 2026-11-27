/* Private host-only reader/projection. Not a product ABI or new parser. */
#ifndef LX06_DECLARATION_READER_DRAFT_H
#define LX06_DECLARATION_READER_DRAFT_H
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision11 declaration reader is fixture-only"
#endif
#include "ctl-config.h"
#include "ctl-list-approval.h"
#include "template-matcher.h"

enum decl_scope { DECL_OFFICIAL=0, DECL_LOCAL, DECL_PLAY, DECL_CTL, DECL_SCOPES };
#define DECL_PHRASE_BYTES 32768
struct decl_item { uint32_t offset; int line; };
struct decl_table {
    const struct cfg *source;
    char app[33];
    char phrases[DECL_PHRASE_BYTES+1];
    size_t used, count[DECL_SCOPES];
    struct decl_item items[DECL_SCOPES][CFG_CANDIDATES];
};
/* Caller supplies an already-open regular header-only fixture fd. Consume
 * exactly its current size with pread; preserve its offset/ownership. Reject
 * extra/missing bytes, read errors and observed identity/metadata changes.
 * Call the original cfg_parse once. All failures preserve *out. This does not
 * establish a production lease or defend every noncooperative writer race. */
int decl_read_fd(int fd, struct cfg *out);
/* Immutable cfg is the sole declaration source. Intern phrase bytes across
 * projections; retain every candidate, alias order and original CTL row id.
 * No matching, caps inference, persistence or implicit CTL in this step.
 * Caller provides a canonical app already enumerated by cfg and retains c
 * unchanged for the table lifetime. The table borrows c for CTL actions.
 * Atomic output. */
int decl_compile(const struct cfg *c, const char *app, struct decl_table *out);
/* Channel-only scripts have no synthetic App or control/session defaults. */
int decl_compile_channels(const struct cfg *c, struct decl_table *out);
/* Entry channels are script-wide, including on multi-App scripts. No first
 * App choice, CTL projection or synthetic operation defaults. */
int decl_compile_entry_channels(const struct cfg *c, struct decl_table *out);
/* Borrowed phrase bytes. script belongs to the caller. No silent truncation.
 * CTL line is suitable for the original cfg_ctl_row(c, app, line), never an
 * index into a separately regenerated map. Other scopes use candidate order. */
int decl_candidate(const struct decl_table *t, enum decl_scope scope, size_t index,
                   const char *script, struct lx06_match_candidate *out);
/* Select the original raw CTL row from the SAME borrowed cfg, by candidate
 * index. No re-expansion or caller-supplied replacement map. NULL if invalid. */
const char *decl_ctl_row(const struct decl_table *t, size_t candidate);
/* Every scope uses the same original matcher once with the complete list.
 * This private text fixture does not dispatch, start a reader or run scripts. */
int decl_match(const struct decl_table *t, enum decl_scope scope, const char *script,
               const char *text, struct lx06_match_result *out);
/* Typed CTL composition only. The original reader/compiler and retained cfg
 * are unchanged owners; every list row needs the caller's same-source proof. */
int decl_read_fd_rows(int,struct cfg *,cfg_list_approve,void *);
int decl_compile_rows(const struct cfg *,const char *,struct decl_table *,cfg_list_approve,void *);
#endif

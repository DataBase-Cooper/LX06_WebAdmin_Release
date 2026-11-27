/* LX06_NATIVE_TARGET_SETTINGS: extension of the one original matcher. */
#ifndef LX06_TEMPLATE_RUNTIME_DRAFT_H
#define LX06_TEMPLATE_RUNTIME_DRAFT_H
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision12 runtime matching is fixture-only"
#endif
#include "template-expression.h"
struct lx06_match_candidate;
struct lx06_match_result;
#define EXPR_MATCH_STEPS 1000000u
enum { EXPR_LIST_PENDING=-3 };
/* Explicit same-source typed approval. Bounds belong to the actual captured
 * span; list items are resolved later by the retained roster consumer. */
struct expr_list_bounds { unsigned mask; unsigned short min[EXPR_SLOTS],max[EXPR_SLOTS]; };
typedef int (*expr_list_approve)(void *,const struct lx06_match_candidate *,
    const struct expr_program *,struct expr_list_bounds *);
int lx06_expr_template_match_lists(const char *,const struct lx06_match_candidate *,int,
    struct lx06_match_result *,int *,unsigned,expr_list_approve,void *);
/* Same candidate/result objects and arbitration as lx06_template_match.
 * branch is zero-based B, -1 for exact/miss/reject. budget bounds visits to
 * the original match_from across ALL conditions/branches/candidates.
 * Any invalid declaration, unsupported list, allocation failure or exhaustion
 * rejects the whole call; out/branch retain their exact original bytes.
 * Programs are compiled internally; no external mutable AST is trusted.
 * No mapping, completion, caps validation, ASR or execution authorization. */
int lx06_expr_template_match(const char *, const struct lx06_match_candidate *,
                             int, struct lx06_match_result *, int *, unsigned);
#endif

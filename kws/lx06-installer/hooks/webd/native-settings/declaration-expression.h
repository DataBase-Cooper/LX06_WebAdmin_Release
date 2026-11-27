/* Private projections/preparation; NOT a product schema or execution API. */
#ifndef LX06_DECLARATION_EXPRESSION_DRAFT_H
#define LX06_DECLARATION_EXPRESSION_DRAFT_H
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision12 effective declarations are fixture-only"
#endif
#include "declaration-reader.h"
#include "template-runtime.h"
/* Same scope/table/cfg; a zero-based selected B alongside the original result.
 * All failures preserve outputs. No externally provided captures are trusted. */
int decl_expr_match(const struct decl_table *, enum decl_scope, const char *,
                     const char *, struct lx06_match_result *, int *);
struct decl_expr_param { char key[81], value[257]; };
struct decl_expr_control {
    struct lx06_match_result match;
    int branch;
    char op[81];
    size_t count;
    struct decl_expr_param params[CFG_ARGS];
};
/* Match once, then map that hit's ORIGINAL CTL row from the SAME effective
 * cfg. A miss/conflict/incomplete returns only its match; op/params are empty.
 * Raw string values only: caps typing, permissions, flow, completion and sink
 * remain their owners' responsibility. No authorization to execute. */
int decl_expr_prepare_control(const struct decl_table *, const char *, const char *,
                               struct decl_expr_control *);
#endif

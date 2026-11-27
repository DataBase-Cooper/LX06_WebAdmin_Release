/* LX06_NATIVE_TARGET_SETTINGS: compiler in the existing matcher translation unit.
 * Private host representation, no product ABI, Wire schema or execution. */
#ifndef LX06_TEMPLATE_EXPRESSION_DRAFT_H
#define LX06_TEMPLATE_EXPRESSION_DRAFT_H
#if !defined(LX06_NATIVE_TARGET_SETTINGS) && !defined(LX06_NATIVE_TARGET_SETTINGS)
#error "revision12 expression compiler is fixture-only"
#endif
#include <stddef.h>
#include <stdint.h>
#define EXPR_BYTES 8192
#define EXPR_NODES 128
#define EXPR_SLOTS 4
#define EXPR_BRANCHES 64
#define EXPR_CONDITIONS 8
#define EXPR_ATOMS 64
#define EXPR_DEPTH 16
#define EXPR_LIST_BYTES 65536
#define EXPR_ITEMS 1024
enum { EXPR_OK=0, EXPR_INVALID=-1, EXPR_CAPACITY=-2 };
enum { EXPR_LITERAL=1, EXPR_SLOT, EXPR_SEQUENCE, EXPR_OR, EXPR_AND };
struct expr_node {
    uint16_t kind, left, right, branches, has_and, offset, length, slot;
};
struct expr_program {
    uint32_t magic;
    uint16_t root, count, used, slots, branches, syntax;
    char names[EXPR_SLOTS][17];
    unsigned char lists[EXPR_SLOTS];
    struct expr_node nodes[EXPR_NODES];
    char literals[EXPR_BYTES+1];
};
struct expr_condition { uint16_t count, atoms[EXPR_ATOMS]; };
struct expr_branch {
    uint16_t count, slot_mask, list_mask;
    struct expr_condition conditions[EXPR_CONDITIONS];
};
/* n excludes NUL. Invalid UTF-8, embedded NUL and every exceeded budget reject.
 * On failure out is byte-for-byte unchanged. No input values in diagnostics. */
int lx06_expr_compile(const char *source, size_t n, struct expr_program *out);
/* Branch zero = B1. OR adds, sequence/AND form left-major Cartesian products.
 * This is declaration inspection, never a text match or an execution permit. */
int lx06_expr_branch(const struct expr_program *, unsigned, struct expr_branch *);
size_t lx06_expr_program_size(void);
size_t lx06_expr_branch_size(void);
struct expr_span { uint32_t offset, length; };
/* mode: '|' local list, ',' official list, '=' CTL top-level =>. Lexical split
 * only; each returned pattern must still pass lx06_expr_compile. Atomic output.
 * Reuses the same JSON literal decoder; no unconditional delimiter splitting. */
int lx06_expr_split(const char *, size_t, int mode, struct expr_span *, size_t, size_t *);
#endif

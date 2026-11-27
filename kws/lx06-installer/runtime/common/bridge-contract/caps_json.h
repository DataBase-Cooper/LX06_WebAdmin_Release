/* B shared caps JSON decoder. Not a script/header/settings parser.
 * Bounded Wire payloads only; decoded strings preserve their explicit length.
 */
#ifndef LX06_BR_CAPS_JSON_H
#define LX06_BR_CAPS_JSON_H
#include <stddef.h>
#include <stdint.h>
#include "versions.h"
#define BR_JSON_TOKENS 4096u
#define BR_JSON_DEPTH 64u
/* Private in-memory ABI, PROVISIONAL(P-20261112-0050-B). Rebuild ALL C
 * consumers together; never persist/transmit this representation. */
#define BR_JSON_LAYOUT_VERSION 2u
enum br_json_kind { BR_J_OBJECT=1, BR_J_ARRAY, BR_J_STRING, BR_J_KEY,
                    BR_J_INT, BR_J_NUMBER, BR_J_TRUE, BR_J_FALSE, BR_J_NULL };
/* Low 14 bits of each span word hold the full 0..8192 value; their high
 * two bits hold the low/high halves of kind. No bitfields or unaligned loads.
 * Private to caps_json.c: consumers use the public accessors below. */
typedef struct { uint16_t offset_kind, length_kind, next; } br_json_token;
typedef struct {
    br_json_token tokens[BR_JSON_TOKENS+1]; /* 0 is the missing sentinel */
    unsigned char raw[LX06_BRIDGE_PAYLOAD_MAX+1], strings[LX06_BRIDGE_PAYLOAD_MAX+1];
    uint16_t count, length, used;
} br_json;
typedef struct { const unsigned char *data; size_t length; } br_text;
int br_json_parse(br_json *,const void *,size_t); /* 1 success; -1 invalid */
int br_json_kind(const br_json *,unsigned);
unsigned br_json_next(const br_json *,unsigned);
unsigned br_json_child(const br_json *,unsigned);
unsigned br_json_field(const br_json *,unsigned,const char *);
br_text br_json_text(const br_json *,unsigned);
int br_json_is(const br_json *,unsigned,const char *);
int br_json_equal(const br_json *,unsigned,const br_json *,unsigned);
int br_json_key_ci(const br_json *,unsigned,const char *);
/* Exact decimal comparison, including integers larger than machine words. */
int br_json_int_cmp(const br_json *,unsigned,const br_json *,unsigned);
int br_json_u64(const br_json *,unsigned,uint64_t *);
size_t br_json_characters(const br_json *,unsigned);
#endif

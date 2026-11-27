/* T17 display-only response validation. PROVISIONAL(P-20261111-1148-B).
 * No snapshot persistence or runtime capability authority. Paired with
 * describe.schema.json; notes:string remains PROVISIONAL(P-0722).
 */
#ifndef LX06_BR_DESCRIBE_H
#define LX06_BR_DESCRIBE_H
#include "caps_json.h"
/* Caller allocates a separate opaque document, serializes calls and supplies
 * one complete READY payload. 1=valid; -1=invalid, with document wiped.
 * Payload memory must not overlap document. Each app's actual encoded object
 * span (including whitespace/escapes) <=4096B.
 * doc_sha256 is FORMAT checked only: this does not verify an APK/body digest.
 * All views expire on the next validate/wipe. Never install as runtime caps.
 */
int br_describe_validate(br_json *,unsigned frame,const void *,size_t);
#endif

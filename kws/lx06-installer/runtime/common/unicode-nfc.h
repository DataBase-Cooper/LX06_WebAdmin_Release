/* Unicode 15.1.0 NFC validation for original eo_nfc name callbacks. */
#ifndef LX06_UNICODE_NFC_H
#define LX06_UNICODE_NFC_H
#define LX06_NFC_UNICODE_VERSION "15.1.0"
/* Exactly 1 for a NUL-terminated valid UTF-8 NFC string of 1..32 Unicode
 * scalars, 0 otherwise. At most 129 bytes are inspected. No normalization,
 * allocation, locale dependence, Python or shared-library dependency.
 * Path separators/control/name syntax remain the original caller's policy.
 * context is ignored; this signature is directly compatible with eo_nfc. */
int lx06_unicode_nfc_name(void *context,const char *name);
#endif

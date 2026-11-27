# Native NFC name callback

`lx06_unicode_nfc_name` implements the original `eo_nfc` contract: return
exactly 1 for valid UTF-8 NFC strings containing 1 through 32 Unicode scalars,
otherwise 0. It validates and never rewrites a saved name. The original EO
caller still rejects separators, controls, duplicate names and schema errors.
Compatibility characters such as U+FB01 remain NFC-valid; this is not NFKC.

The Unicode version is 15.1.0, matching the verified existing host Python
reference. Product code has no Python, locale, allocator or dynamic-library
dependency. The provider inspects at most 129 bytes of the NUL-terminated
input, keeps 32 original scalars and at most 128 decomposed scalars, and has
no static mutable state. Input pointers must meet the ordinary C-string
contract; this is not an arbitrary address validator.

The generated data is pinned by SHA-256 in
`tools/generate_unicode_nfc.py`. Source files come from
`https://www.unicode.org/Public/15.1.0/ucd/` and are retained with the evidence.
The generator is offline and requires the exact UnicodeData,
DerivedNormalizationProps, CompositionExclusions, NormalizationTest and
ReadMe bytes. The accompanying `unicode-data-LICENSE.txt` carries the Unicode
data license. Generated tables contain 941 canonical mappings, a composition
index excluding full composition exclusions, 922 combining classes and 73
NFC_QC=No ranges. Immediate rejection of NFC_QC=No scalars allows omission of
decompositions that no accepted scalar can reach; the generator closes the
remaining dependency set and proves maximum expansion 4 and recursion depth
3. Hangul decomposition and composition use the normative algorithm.

After strict UTF-8 decoding, the validator canonically decomposes and stably
orders combining marks. Composition observes blocking and full exclusions,
including Hangul L/V/T cases. It compares the result with the original scalar
sequence rather than approving every syntactically valid name.

The official NormalizationTest file supplies 19074 independent rows, all five
columns checked for this validator's NFC predicate. Native and ASan/UBSan
builds also exercise invalid UTF-8, 32/33-character and 128-byte boundaries,
combining-order inversion, equal-class blocking, singleton and composition
exclusions, Hangul, compatibility characters, controls and noncharacters.
These are validation evidence, not a feature-completion count.

The real original EO parser calls this provider and hands the accepted name
to `br_object_flow_copy`; a rejected replacement preserves the prior usable
object. The real complete-directory loader calls the same provider in the
compact source parser and in canonical run rendering. A decomposed-name
replacement discards the unpublished stage and cannot render through the old
stale source. No permissive NFC callback is used in these consumers. These
tests do not authorize scheduled FLOW, install a production owner or replace
the existing process/UNKNOWN/K6 gates.

The immutable table payload is 17446 bytes. Native x86-64 O2 code contributes
1664 bytes and unwind data 304 bytes: 19414 bytes before linker alignment.
OEM ARMv7 hard-float O2 code contributes 1196 bytes: 18642 code/table bytes
before linker alignment. Both provider objects have zero undefined symbols,
zero data/BSS and no allocator calls. The compiler-frame conservative call
chain is `name + 4 * decompose + emit + ccc`: 984 bytes native and 844 bytes
ARM (the caller's own stack is additional). These include bounded recursive
frames, not merely the largest frame. The ARM object was compiled using the
existing OEM glibc 2.25 headers; an object build is not on-device execution.

Provider sections, stack, final hash implementation and the original caller
chain must all be included in the scheduler resource assessment. This
package does not by itself close 128 KiB, combined-flow 256 KiB, complete-chain
RSS, global idle, ARM runtime or speaker acoustic acceptance. Its consumer
evidence binds the previous frozen compact A implementation; the separate
final SHA change needs only the affected consumer combination rechecked,
without repeating unaffected provider conformance checks.

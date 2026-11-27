# Normal phone profile and historical probe

The normal `build.py` now includes the source-owned phone profile. `--phone-fd`
accepts the original bounded request from an owned root `SOCK_SEQPACKET`; the
normal source owner reads the actual deployed script and private settings,
checks their complete retained identity, performs the native wake match, and
borrows the same CA/token/SPKI configuration. An ordinary call needs no FLOW
file or directory. FLOW and schedule inputs keep their separate original
three-input contracts; an entry-only source cannot provide their proof token.

The current normal operation is `doubao.call.start`. TLS/CA/SPKI/Bearer must
succeed, then live caps must validate the operation before its single business
OPEN. GRANT is a started acknowledgement, and only the actual correlated
READY confirms the declared audio vector. For an explicit MIC declaration, a
correlated started GRANT permits setup capture on the same retained source;
READY and downlink playback still require the complete vector confirmation.
The real phone native record hook consumes that bounded MIC PCM. A kept
speaker path uses the original ALSA player and pending slot; stop and exact
K6/readback/reap release
its original source. A missing result-output directory is created privately;
unsafe existing objects are refused without permission changes or replacement.

| Contract | Value | Normal consumer |
| --- | --- | --- |
| Wire header | 1, 48 bytes, payload at most 8192 bytes | `bridge/wire.c` and shared `bridge-contract/versions.h` |
| Specification revision | BRIDGE-WIRE v2.2 | Shared contract; not a header version |
| Bridge capability | `bridge_version: 2.0` | `bridge/discovery.c`, `bridge/operation.c` |
| Container/hook IPC | `hook/1.0` | Phone registration; incompatibility cannot mean READY |

Ordinary business keeps the existing generation 0. It does not advertise the
provisional `session.command_id` feature or repurpose a remote generation as a
local player tag. Original READY accepts `kind=result` or `kind=state`, not a
new `kind=ready`. TEXT/GRANT/UI state alone cannot establish media readiness.

Normal executable consumers are in `tests/parallel/b/test_t16_normal_keep.py`:
first use with only real script/settings, actual TLS and K6 readback, unsafe
result-path refusal, and kept exact PCM followed by original STOP/reap.
Strict OEM glibc 2.25/ABI/K8 evidence and emulated ARM loading remain separate
from device loading and acoustic acceptance. The original official/MFCC sender,
resident script and session-run now invoke this normal entry. A local composition
with the actual phone Session/readLoop/registry/native hooks has consumed exact
MIC and downlink PCM through ordinary READY and the original B player/K6/reap.
It retains full_call_fence=false and leaves physical phone cleanup pending.

The same retained effective control declaration supplies BARGEIN words before
the original dc_session_open. A blank or inactive CTL declaration preserves the
saved PHONE configuration, including its interruption mode and other fields;
explicit pipe-separated words adapt only to the original matcher separator and
pass the original configuration validator before opening a session.

Full ASR/CTL, same-WSS multiple
business commands, persistent UNKNOWN recovery, actual FLOW publication and
whole-device acceptance remain incomplete. No firmware/package completion is
claimed by these software consumers.

## Historical private probe

The following describes the retained legacy probe, not the normal profile.
Its independently inherited configuration and metadata cannot authorize the
normal source owner or substitute for the checks above.


This directory's `phone.c`, `phone.h`, `phone_worker.c` and `build_phone.sh` are
an independently built transport candidate. The core has a private metadata-only
`--phone-fd` wiring seam, but phone media, router callers, firmware, installer
and scripts are **not enabled**. The existing Xiaozhi/Doubao/account audio
adapters are unchanged. No actual phone call, device audio or acoustic behavior
has been accepted by these synthetic tests.

The current Android service is a manually attached **probe**. Its successful
command ACK means the hook attached; it does not mean a call started. Its READY
has `probe_only=true`, `call_ready=false`, and no genuine reply lifecycle. This
worker never emits core READY or PCM for that probe. The full contract is
[`../../phone-audio/WIRE.md`](../../phone-audio/WIRE.md).

## Dependency and trust boundary

Stock rc23 contains libcurl 7.55.1, without `curl_ws_*`; host curl 8 WebSocket
support cannot be assumed on LX06. This worker uses the stock public OpenSSL
1.0 ABI and the existing RFC6455 frame codec. ARM builds import at most glibc
2.22. TLS is strictly 1.2, with all three checks required: configured CA chain,
numeric IPv4 SAN, and SHA-256 SPKI pin. There is no `ws://`, DNS endpoint,
public-Internet endpoint or insecure fallback. The stock ABI compatibility is
target-only; host builds use their normal OpenSSL implementation.

The executable takes no arguments. FD 3 must be an inherited connected Unix
`SOCK_SEQPACKET` owned by a root peer; the worker must itself run as root.
`PHONE_PAIRING_CONFIG` names an absolute root-owned regular file, single link,
mode 0600. Every parent must be root-owned, non-symlink and not writable by
other users, with `/tmp`'s root-owned sticky directory the sole exception. The
CA file has the same ownership/path checks and cannot be writable by others.
Prepare both inside a private root-owned RAM directory; do not chmod official
DATA paths to make an unsafe path pass. Configuration is small bounded JSON:

```json
{
  "endpoint": "wss://127.0.0.1:9443/lx06/phone-audio/v1",
  "token": "replace_with_a_private_pairing_token",
  "ca_file": "/tmp/private-phone-pairing/phone-ca.pem",
  "spki_sha256": "replace_with_the_exact_64_hex_character_public_key_pin"
}
```

This is a shape example, not a usable pairing file. Tokens must contain 32–128
URL-safe characters and travel only in the TLS Authorization header. Tokens,
raw wake text and server response bodies never go to stdout/stderr or reports.
Core dumps are disabled before reading credentials. No automatic retry occurs,
because a future physical action may have completed without its ACK arriving.

## Command and readiness planes

The first core OPEN contains the exact bounded **command JSON v2** envelope from WIRE.md,
including the actual wake source/text, script identity and action registry key.
It is forwarded byte-for-byte after strict UTF-8/JSON/schema validation.
Duplicate decoded keys, malformed Unicode, unknown fields, mismatched epoch,
unbounded nesting, arbitrary app names and shell actions are rejected. The only
registered target is `doubao`; `probe.attach` and `call.start` are transportable,
with empty params. The current phone probe explicitly refuses `call.start`.
There is no WeChat, takeout, arbitrary package or shell-command implementation.
Use the common script-name subset: first ASCII alphanumeric or underscore,
remaining ASCII alphanumeric/underscore/hyphen/dot, `.sh` suffix, at most 64
characters, no adjacent dots. Both current Java and C enforce this subset.

Two TEXT metadata records have generation=sequence=0:

- `kind=command_ack`: strict typed ACK with matching epoch and command_id.
- `kind=phone_status`: sanitized capability diagnostic with one boolean,
  `production_capable`; no arbitrary server text.

**Main routes these to a metadata-only consumer. They never enter the spoken
transcript, model prompt, control matcher or TTS.** `mode_gate.c` owns that seam;
the worker still builds separately. Current main also refuses a complete READY
before LISTENING because the protocol below has no verified turn fence. See
[`../../../docs/unified-phone-action-wiring-20261101.md`](../../../docs/unified-phone-action-wiring-20261101.md)
for actual private-FD requests, the shared connection cue, offline evidence and
the remaining router/media work. A future generic command status channel may
replace this transport-compatible TEXT wrapper.

Command JSON v2 adds mandatory decimal-string `deadline_unix_ms` and ACK boolean
`replayed`; the DC 48-byte header/WSS URL remain v1. First admission requires wall
time in [2024-01-01, 2100-01-01) and a future budget of at most 30 seconds, then
uses only monotonic time until a terminal result. The deadline covers TLS setup
and action completion, not the lifetime of an attached audio session. Pending
expiry freezes forwarding, returns fixed failure metadata and closes the remote
socket. The old command cannot finish later and become READY.

The C FD retains one command digest and last typed ACK. Identical duplicate OPEN
only replays metadata locally, never another WSS action. A conflicting request
gets `command_conflict` without replacing the original. Root has a separate
64-entry/five-minute process-local ledger, documented in WIRE.md. Replayed ACKs
are always metadata, including completed results past the old deadline; remote
`replayed=true` never satisfies READY. Neither side automatically reconnects or
retries an uncertain action. Restart-safe physical-action deduplication is not
implemented and no physical action is enabled.

Core READY requires both a matching non-replayed completed ACK and all of:
`probe_only=false`, `call_ready=true`, `virtual_mic=true`, `rtc_capture=true`,
`call_lifecycle=true`, `reply_lifecycle=true`. Callback counters, claimed hook,
elapsed time and probe ACK cannot satisfy these conditions. Capability downgrade
after readiness fails closed. A future server can be tested with synthetic
production capabilities, but this does not establish actual phone support.

## Audio identity and cancellation

The future audio seam requires real REPLY_BEGIN/END, with a reply sequence that
is constant across its PCM. REPLY_BEGIN has generation 0; the core must GRANT
that sequence a nonzero generation. The phone must wait for that GRANT and send
PCM bearing that **same** identity. The worker never rewrites a packet's epoch,
generation or sequence. Audio arriving before the grant, with the wrong
generation, stereo format or unsupported playback rate is refused. It does not
silently resample. Current probe PCM uses continuous chunk sequences and has
no reply boundaries, so it cannot satisfy this seam.

CANCEL freezes forwarding immediately and removes old queued PCM and unsent
MIC frames before network work. A TLS write already in progress retains its
exact buffer until its frame finishes; CANCEL then follows, with a two-second
write deadline and fail-closed behavior. Actual speaker stop remains the core's
independent stop path and must not wait for this remote cancellation. Repeated
CANCEL is idempotent. Old PCM retains its original identity and is discarded.

There is **no verifiable remote turn-fence event in this v1 protocol**.
Consequently every RESUME returns `turn_fence_unavailable`, and unsolicited
RESUMED is refused even if a peer advertises `turn_fence=true`. A capability bit,
new generation, newer OPEN, quiet PCM, track callback or fixed delay cannot
invent a fence. Implement and independently verify a real call/reply stop/start
fence before enabling continued phone turns. This restriction is intentional;
the current probe is not a complete phone voice adapter.

## Bounded reactor

One process/reactor owns numeric nonblocking TCP, TLS and WebSocket I/O. It
drains core commands before reading network audio, processes bounded batches,
keeps separate TLS read/write readiness and never performs blocking DNS, SDK,
audio-device work or network work on an audio callback. There are eight bounded
network frames and eight local packets, each at most 8240 payload+header bytes;
the receive buffer and JSON tree also have fixed caps. Overflow fails closed.
Connecting/upgrading is limited to five seconds; stalled writes to two seconds.
GRANT keepalive runs every two seconds. Fragmentation, masked server frames,
noncanonical lengths, duplicate upgrade headers and invalid wire flags/format
are rejected. There is no reconnect or generation relabelling workaround.

## Reproduce offline tests

Run with WSL root; the test re-execs itself in a private network namespace and
enables only loopback. It generates a temporary local CA/certificate and pairing
material, uses a Python mock TLS/WSS server, then removes the temporary files.
It never contacts a real phone, platform, account, speaker or Internet service.

```sh
python3 runtime/dialogue-core/adapters/tests/test_phone.py --mode host --output build/phone-host-new
python3 runtime/dialogue-core/adapters/tests/test_phone.py --mode sanitize --output build/phone-sanitize-new
python3 runtime/dialogue-core/adapters/tests/test_phone.py --mode arm --output build/phone-arm-new
```

Outputs must be new directories. Clock-fault cases use a separately built test
binary with an explicit link-time clock wrapper; the normal worker has no clock
override environment or test hook. Reports identify that test-only binary.
ARM uses QEMU and the actual rc23 stock loader,
libc and OpenSSL, not host TLS libraries. Reports distinguish synthetic
transport behavior from real audio/call acceptance, and bind exact source and
binary hashes. Keep earlier failed reports; never overwrite immutable attempts.

Historical command-JSON v1 offline evidence (46 cases in each report). These
reports remain frozen; they do not describe the current v2 sources:

| Mode | Report | Report SHA-256 |
| --- | --- | --- |
| Host | `build/dialogue-core-phone-attempt5/report.json` | `54ca6b14b622634953e1badf1305b63b17da262d786a257a8da34919327d98e5` |
| ASan + UBSan | `build/dialogue-core-phone-attempt6/report.json` | `bf92c3335b68c13d0d34810f05df0f129e6a08a31454131ac7cafcf967190661` |
| Stock ARM | `build/dialogue-core-phone-attempt7/report.json` | `fadcd051f1990f976254ca5494466fe90096d015c1126e2b69a6b987eca19257` |

All three completed with `source_unchanged=true`. The ARM executable in attempt7
is 67104 bytes, SHA-256
`398a24503ac0c08ab867c92b6426b81d979799ce7979169d6c47d5f298e6e54b`.
These results cover strict framing/JSON/TLS, readiness gates, terminal ACKs,
stale-generation cancellation, genuine synthetic reply identity, immediate
local freeze, no-fence resume refusal and bounded backpressure. They do not
establish real call lifecycle, physical stop latency or acoustic acceptance.

Current command-JSON v2 offline evidence has 63 cases in each of
`build/dialogue-core-phone-attempt13/report.json` (host), `attempt14` (ASan+UBSan)
and `attempt15` (stock ARM). Each reports `source_unchanged=true`. The current
ARM worker is 67112 bytes, SHA256
`3e3c64f311b07ce6cc30e782f95cf625c56b6fd841614d243631792fbc31cf42`.
The added cases cover bounded deadlines, clock faults, duplicate/conflicting
OPEN and replay metadata that cannot create READY. See
`docs/phone-command-v2-20261101.md` for cross-language evidence and limitations.

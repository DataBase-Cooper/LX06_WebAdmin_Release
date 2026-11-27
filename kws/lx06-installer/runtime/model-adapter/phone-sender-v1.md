# Original phone sender, v1

This source increment connects the existing official/local script launchers to
normal `lx06-dialogued --phone-fd`. Its sanitized upload is built with:

```
python3 runtime/model-adapter/build_phone.py /private/output/doubao_phone.sh
```

The builder embeds the function prefix from the original `account-voice.sh` into
`scripts/examples/user-scripts/phone-bridge-v1/doubao_phone.sh.tpl`, as the existing
account builders do. There is no new ROM service, configuration library, account
HTTP pool, provider login or alternate request parser. Existing script settings
and native saved-source matching remain authoritative.

The new template explicitly uses the existing `script-settings run-script`
`shell-fd-v1` contract. Its first command closes FD9; after binding, it uses
`LX06_SCRIPT_PATH` and `LX06_SCRIPT_ID`, because `$0` is the closed descriptor path.
An initial ordinary shell launch selects a fresh named source via `run-script`;
a refusal has no unbound `run` fallback. The protocol retains the original source
inode, with the limitations documented in `run-script-v1.md`.

The only new native sender command is:

```
/usr/lib/lx06/xiaoai/session-run run NAME.sh --phone-session official_asr TEXT
/usr/lib/lx06/xiaoai/session-run run NAME.sh --phone-session local_wake TEXT
```

It requires root, an exact argument vector, a valid script basename and a
nonempty wake of at most 96 UTF-8 bytes. It builds only the original six-field
v1 `doubao` / `call.start` request, using `dc_mode_escape_text` and
`dc_mode_parse_request` from the original `mode_gate.c`. Control characters,
invalid UTF-8 and unsupported sources fail before session acquisition.

Build the existing helper with product capability `LX06_SESSION_PHONE_RUNTIME`
and the original `mode_gate.c` as a second translation unit. The unchanged
single-TU build retains the old exec path and has no new link dependency.
The actual `scripts/build_processed_mic.sh` helper recipe is a separately owned,
reviewed input. No old release package or builder is rewritten.

The supervisor creates one CLOEXEC SOCK_SEQPACKET pair, moves its own endpoints
to free descriptors at or above 10, sends after the original lease record and
resource-issuer start succeed, then releases the original child gate. The child
closes the sender endpoint and inherits only its receiver endpoint for this
channel when execing `${LX06_DIALOGUED:-/usr/bin/lx06-dialogued}`. Existing caller,
lease, light, FD9 and proof descriptors retain their own original semantics.
Default `execvp` and group/deadline/reap handling are unchanged.

Only explicit valid Doubao phone metadata enables the new router argv:
`--phone-wakeup official_asr TEXT` or `--phone-wakeup local_wake TEXT`.
The original official router and actual Webd MFCC/listen/launcher paths supply
those sources. Native phone declarations use `# 小爱通道唤醒词: `; old account
scripts retain their existing equals declaration. The existing audio metadata
helper accepts the native single-app `LX06-PHONE-APPS: doubao` spelling as well
as its historical singular spelling; conflicts and unsupported values refuse.
The normal receiver independently reloads and rematches the saved declaration.

Phone resident mailboxes have the explicit four-field ABI
`wake<TAB>score<TAB>monotonic-seconds<TAB>source<LF>`. Missing, unknown, extra or
control-injected sources cannot launch a phone session. The existing account
three-field mailbox remains unchanged. Webd quotes the actual wake for the
original keeper argument splitter, without shell evaluation; its phone listen
consumer retains the full selected phrase after `text=`. Literal quotes and
backslashes in declarations must use the original expression quoted-literal
grammar; an unescaped invalid declaration remains rejected.

Current template audio is **mic=close, speaker=open, ring=close**. This delivery
proves the speaker-output path only. It does not claim microphone uplink,
full duplex, an accepted causal turn fence, music/CTL, Android device operation,
ROM installation, flashing, acoustics or project closure.

`tests/parallel/b/test_t16_phone_sender.py` executes the complete original
router/Webd translation units, generated upload, actual `run-script`, actual
session supervisor, unmodified normal host receiver, authenticated local TLS,
original ALSA file output and K6 readback. Keeper execution, private /data path
mapping and ASR acquisition are explicitly synthetic host boundaries. No caller
injects a prepared request socket into the normal core. Separate focused cases
exercise actual resident re-wake, source refusal, retained FD9, original resource
proof closure and default exec/argv/status. The transport-only receiver probe
uses the original mode parser and lease; it does not claim native profile/audio
acceptance. Raw failed builds and the initial invalid declaration evidence are
preserved outside source snapshots.

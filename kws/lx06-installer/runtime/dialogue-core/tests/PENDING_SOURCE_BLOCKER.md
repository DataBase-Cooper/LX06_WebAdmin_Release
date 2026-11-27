# Pending ASR source identity counterexample (offline)

From `runtime/dialogue-core` on Linux/WSL, compile and run (use a scratch
binary path; the test intentionally exits 1 until the defect is fixed):

```sh
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
  -ffunction-sections -fdata-sections -fsanitize=address,undefined \
  -fno-sanitize-recover=all -no-pie -Ivendor/libfvad/include \
  tests/test_pending_source_red.c asr/question-endpoint-vad.c \
  vendor/libfvad/src/fvad.c vendor/libfvad/src/signal_processing/*.c \
  vendor/libfvad/src/vad/*.c io/player.c media.c session.c control.c tts.c \
  account.c bootstrap.c audio_route.c protocol.c ../doubao/src/text_match.c \
  ../doubao/src/json.c -Wl,--gc-sections -pthread -lm -lopus -o "$TMPDIR/pending-source-red"
"$TMPDIR/pending-source-red"
```

The real adapter queues two old-generation PCM frames during final wait and the
200 ms post-final delay. After an independently accepted cancellation and a new
reply, it drains both Opus packets into a new recognizer turn. A synthetic SDK
partial for that turn reaches real `recognition` and `dc_session` with reply 2,
cancelling that reply. Three sanitizer runs produced the same RED: reply 1
pending, result reply 2, two cancellations, reply 2 no longer live.

The socket peer supplies the recognition result; the arbitrary synthetic PCM is
not proof that cloud ASR would recognize that phrase. The stop callback is a
counter, not a real player worker or acoustic measurement. There is no device,
network, OTA, or production recognizer in this fixture.

The pending byte queue contains no per-span reply or near-end voice provenance.
`bind_source` binds the currently active decoder (including the ended old turn),
then `start` resets that binding before replay. A new turn can also mix queued
old and genuinely new speech. A blanket flush can erase the user's next sentence,
and simply tagging an entire mixed queue may wrongly reject that sentence or
misattribute old audio. No production fix is made: defining how source identity
and utterance boundaries travel with captured spans, including mixed/ambiguous
frames and replay, and how to fail closed while preserving subsequent speech,
requires a verified contract and regression cases. This RED remains open.

## Release gate — current source, offline rerun

The command above was rerun against the current source under WSL with
ASan/UBSan: exit 1, `pending-old-reply=1, recognized-reply=2, serial=2,
cancels=2, current-live=0`. There were no sanitizer diagnostics. The actual
`recognition()` calls the actual `dc_session_text_interrupt()` endpoint; its
identity checks do **not** fail closed in this reproduction because the adapter
has already relabeled old queued PCM as reply 2. The second cancellation and
loss of reply 2 are observable at the real session endpoint. This is not a full
launched production process: `dc_session_open()` is invoked directly, the SDK
peer supplies synthetic text, and stop/cancel callbacks are counters, not the
production player or adapter. Thus it proves neither startup/ownership guard
behavior nor physical speaker drop, and does not establish real-world acoustic
recognition frequency. An intentional startup refusal, if separately established,
would block reaching the defect, not make this session endpoint safe.

**BLOCK release of this dialogue path** until production carries trustworthy
capture-to-result source/utterance attribution and has an explicit bounded,
observable policy for ambiguous mixed spans and overflow without silently
throwing away a subsequent sentence. Require this exact adapter → recognition →
session regression to pass, with fresh-current-speech and complete-next-sentence
positive controls, stale/mixed/unknown and partial/final/replay/rollover cases,
plus the production startup/ownership and real actor/player path exercised
separately. Validate target/device and human acoustic behavior separately; this
offline RED cannot clear either gate. Do not fix it by flushing pending bytes or
by assigning the whole mixed queue the current generation.

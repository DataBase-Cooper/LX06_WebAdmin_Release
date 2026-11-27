# Deferred ASR overflow: offline API experiment

Run on Linux with gcc and libopus, with `TMPDIR` set to a scratch directory:

```
python3 runtime/common/tests/test_pending_overflow_prototype.py
```

The script compiles unmodified actual translation units and scratch-only transformed copies; it never edits production. For both common and dialogue ASR, it checks CONTROL, QUESTION and QUESTION bank-allocation failure, filling the real deferred queue through feed and then feeding a distinct 960-byte frame. The original must fail the contract test; the candidate must return `-2` with `errno=ENOBUFS`, preserving all queued bytes, head, size, drop count and caller's current bytes. Dialogue invokes the real stamped feed wrapper. The fixture holds the caller frame until it inspects the error; it does not implement a capture queue or replay.

Candidate semantics are deliberately narrow: deferred append rejects the whole input before eviction and propagates its return through feed and the stamped wrapper. The frame remains owned by the caller, which must retain it until accepted or issue a genuinely user-visible failure; a return code alone does neither. This is not an atomicity guarantee for other feed errors or pending drain: the existing recursive draining path resets the pending count before sending and collapses failures to `-1`. Nor does it provide source identity for mixed deferred audio, verify an audio service, or exercise a real user notification.

Production adoption is blocked. The real `runtime/dialogue-core/main.c` capture path reads a frame, processes it into a stack-local PCM buffer, and on any ASR error merely sets `failed` and returns before route capture; neither that buffer nor a retry queue survives, and no verified user-visible overflow report is emitted. `runtime/doubao/src/main.c` and `runtime/model-adapter/model-asr.c` also break on feed errors; `model-voice.c` logs and exits, not a verified user notification. Ownership, bounded backpressure and an end-to-end user-facing failure path need independent design and testing. Do not ship this scratch transformation as a production fix.

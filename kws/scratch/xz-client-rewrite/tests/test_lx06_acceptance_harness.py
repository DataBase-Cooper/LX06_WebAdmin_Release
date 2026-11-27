"""Unit tests for wake retry behavior in the physical acceptance harness."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HARNESS_PATH = ROOT / "tests" / "run_lx06_twenty_turn_acceptance.py"
spec = importlib.util.spec_from_file_location("lx06_acceptance_harness", HARNESS_PATH)
harness = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(harness)


def snapshot(sessions: int, tts_start: int) -> str:
    return "\n".join(
        ["watcher session_start"] * sessions + ["event=TTS_START"] * tts_start
    )


def main() -> None:
    lifecycle = harness.counts(
        "event=STT_RECOGNIZED text_bytes=12\n"
        "event=LISTEN_RESUME mode=auto\n"
    )
    assert lifecycle["stt_recognized"] == 1
    assert lifecycle["listen_resume"] == 1

    spoken = []
    baseline = snapshot(3, 9)
    after_second_attempt = snapshot(4, 10)
    wait_calls = []

    def speak(text: str):
        spoken.append(text)
        return {"text": text, "output": "speak OK"}

    def timeline():
        return baseline

    def wait_until(predicate, seconds: float, label: str) -> str:
        wait_calls.append((seconds, label))
        candidate = baseline if len(wait_calls) == 1 else after_second_attempt
        if not predicate(candidate):
            raise TimeoutError(label)
        return candidate

    source, attempts, active = harness.start_session_with_wake_retry(
        speak_fn=speak,
        timeline_fn=timeline,
        wait_fn=wait_until,
        attempts=3,
    )

    assert source["output"] == "speak OK"
    assert len(spoken) == 2
    assert spoken == [harness.WAKE, harness.WAKE]
    assert [item["status"] for item in attempts] == ["NO_SESSION", "STARTED"]
    assert active == after_second_attempt

    baseline_counts = harness.counts("")
    assert harness.round_ready(
        "event=SESSION_SUMMARY mic_frames=1 status=0\n", baseline_counts, final_round=True
    )
    assert not harness.round_ready(
        "event=SESSION_SUMMARY\n", baseline_counts, final_round=False
    )

    assert harness.build_prompts(20) == harness.ROUNDS
    long_prompts = harness.build_prompts(23)
    assert len(long_prompts) == 23
    assert long_prompts[-1] == harness.GOODBYE_PROMPT
    assert long_prompts[19:22] == harness.ROUNDS[:3]
    for rounds in range(1, harness.MAX_ROUNDS + 1):
        prompts = harness.build_prompts(rounds)
        assert len(prompts) == rounds
        assert prompts[-1] == harness.GOODBYE_PROMPT
        assert harness.GOODBYE_PROMPT not in prompts[:-1]
    for rounds in (0, harness.MAX_ROUNDS + 1):
        try:
            harness.build_prompts(rounds)
        except ValueError:
            pass
        else:
            raise AssertionError(f"rounds={rounds} must be rejected")
    print("lx06 acceptance harness tests: PASS")


if __name__ == "__main__":
    main()

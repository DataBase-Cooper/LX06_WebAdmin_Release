#!/usr/bin/env python3
"""Physical 20-turn acceptance: LX04 .100 speaks, LX06 .166 replies."""

from __future__ import annotations

import json
import re
import subprocess
import sys
import time
import argparse
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KWS_ROOT = ROOT.parents[1]
sys.path.insert(0, str(KWS_ROOT))

import paramiko  # noqa: E402
import put166  # noqa: E402

ROUNDS = [
    "你好",
    "请说一句你好",
    "你叫什么名字",
    "一加一等于几",
    "中国的首都是哪里",
    "请说一个颜色",
    "请说一个水果",
    "请说一个动物",
    "请从一数到三",
    "你能听见我吗",
    "请回答好的",
    "请说今天天气不错",
    "请说一句早上好",
    "请说一句晚安",
    "请告诉我你会聊天",
    "请说一个数字",
    "请继续",
    "请再说一句",
    "请用一句话介绍自己",
    "小智再见",
]
# The client ends the session itself on this exact phrase (protocol goodbye);
# it must never be spoken mid-session, so the cycling pool excludes it.
GOODBYE_PROMPT = "小智再见"
# The OEM recognizer first needs its native wake prefix before it can hear the
# follow-up trigger that the watcher matches in its final ASR record.
WAKE = "小爱同学，召唤小智"
TIMELINE = "/data/lx06/xiaozhi/timeline.log"
INSTRUCTIONS = "/tmp/mico_aivs_lab/instruction.log"
MAX_ROUNDS = 60


def build_prompts(rounds: int) -> list[str]:
    """Cycle the non-goodbye prompt set; only the final round says 小智再见."""
    if not 1 <= rounds <= MAX_ROUNDS:
        raise ValueError(f"rounds must be between 1 and {MAX_ROUNDS}")
    pool = [p for p in ROUNDS if p != GOODBYE_PROMPT]
    return [
        pool[(index - 1) % len(pool)] if index < rounds else GOODBYE_PROMPT
        for index in range(1, rounds + 1)
    ]


def device(command: str) -> str:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(
        put166.HOST,
        username=put166.USER,
        password=put166.PASS,
        port=89,
        timeout=10,
        allow_agent=False,
        look_for_keys=False,
        transport_factory=put166._T,
    )
    try:
        status, output, error = put166._exec(client, command)
    finally:
        client.close()
    if status:
        raise RuntimeError(f"device command failed rc={status}: {error.strip()}")
    return output


def timeline() -> str:
    return device(f"cat {TIMELINE}")


def counts(text: str) -> dict[str, int]:
    return {
        "sessions": text.count("watcher session_start") + text.count("event=CLIENT_START"),
        "tts_start": text.count("event=TTS_START"),
        "tts_stop": text.count("event=TTS_STOP"),
        "playback_exit_ok": text.count("event=PLAYBACK_EXIT exit=0"),
        "stt_recognized": text.count("event=STT_RECOGNIZED"),
        "listen_resume": text.count("event=LISTEN_RESUME mode=auto"),
        "playback_drain_timeout": text.count("event=PLAYBACK_DRAIN_TIMEOUT"),
        "summary": text.count("event=SESSION_SUMMARY"),
    }


def round_ready(text: str, baseline_counts: dict[str, int], final_round: bool) -> bool:
    """Accept normal re-arm, or a clean server-closed final goodbye turn."""
    current = counts(text)
    if current["listen_resume"] > baseline_counts["listen_resume"]:
        return True
    if not final_round or current["summary"] <= baseline_counts["summary"]:
        return False
    return bool(re.search(r"event=SESSION_SUMMARY .* status=0", text))


def xz_client_running() -> bool:
    return "xz_client" in device("ps -w | grep '[x]z_client' || true")


def wait_until(predicate, seconds: float, label: str) -> str:
    deadline = time.monotonic() + seconds
    last = ""
    while time.monotonic() < deadline:
        last = timeline()
        if predicate(last):
            return last
        time.sleep(1)
    raise TimeoutError(f"timed out waiting for {label}")


def speak(text: str) -> dict[str, object]:
    command = [sys.executable, str(KWS_ROOT / "ssh100.py"), "speak", text]
    result = subprocess.run(command, text=True, capture_output=True, timeout=90)
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0 or "speak OK" not in output:
        raise RuntimeError(f".100 playback failed: {output}")
    return {"text": text, "output": output}


def input_peak(delta: str) -> int:
    values = [
        int(match.group(1))
        for match in re.finditer(r"event=MIC_FRAME.*?rms=(\d+).*?tts=0.*?muted=0", delta)
    ]
    return max(values, default=0)


def start_session_with_wake_retry(speak_fn, timeline_fn, wait_fn, attempts: int = 3):
    """Start one physical session, retrying only missed OEM ASR wakeups."""
    if attempts < 1:
        raise ValueError("attempts must be positive")
    records = []
    for attempt in range(1, attempts + 1):
        baseline_counts = counts(timeline_fn())
        source = speak_fn(WAKE)
        record = {"attempt": attempt, "source": source["output"]}
        try:
            active = wait_fn(
                lambda text: counts(text)["sessions"] > baseline_counts["sessions"]
                and counts(text)["tts_start"] > baseline_counts["tts_start"],
                30,
                f"XiaoZhi session and greeting (wake attempt {attempt})",
            )
        except TimeoutError:
            record["status"] = "NO_SESSION"
            records.append(record)
            continue
        record["status"] = "STARTED"
        records.append(record)
        return source, records, active
    raise TimeoutError(f"XiaoZhi did not start after {attempts} physical wake attempts")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rounds", type=int, default=len(ROUNDS),
                        help=f"number of sequential source/reply rounds (1-{MAX_ROUNDS})")
    args = parser.parse_args()
    try:
        prompts = build_prompts(args.rounds)
    except ValueError as exc:
        parser.error(str(exc))
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    evidence = ROOT / "evidence" / f"lx06-20turn-{stamp}"
    evidence.mkdir(parents=True)
    result: dict[str, object] = {
        "status": "RUNNING",
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "wake": WAKE,
        "rounds_requested": args.rounds,
        "rounds": [],
    }

    try:
        before = timeline()
        before_counts = counts(before)
        wake_playback, wake_attempts, active = start_session_with_wake_retry(
            speak, timeline, wait_until
        )
        result["wake_playback"] = wake_playback
        result["wake_attempts"] = wake_attempts
        greeting_counts = counts(active)
        active = wait_until(
            lambda text: counts(text)["tts_stop"] >= greeting_counts["tts_start"],
            30,
            "greeting playback completion",
        )
        wait_until(
            lambda text: counts(text)["playback_exit_ok"] > before_counts["playback_exit_ok"],
            30,
            "greeting playback device release",
        )
        wait_until(
            lambda text: counts(text)["listen_resume"] > before_counts["listen_resume"],
            10,
            "greeting microphone resume",
        )

        for index, prompt in enumerate(prompts, start=1):
            terminal_round = index == args.rounds
            if not xz_client_running():
                raise RuntimeError(f"round {index}: xz_client exited before input")
            baseline = timeline()
            baseline_counts = counts(baseline)
            source = speak(prompt)
            wait_until(
                lambda text: counts(text)["stt_recognized"] > baseline_counts["stt_recognized"],
                25,
                f"round {index} .100 speech recognition",
            )
            response = wait_until(
                lambda text: counts(text)["tts_start"] > baseline_counts["tts_start"],
                25,
                f"round {index} TTS response",
            )
            response_counts = counts(response)
            settled = wait_until(
                lambda text: counts(text)["tts_stop"] >= response_counts["tts_start"],
                30,
                f"round {index} TTS completion",
            )
            playback_settled = wait_until(
                lambda text: counts(text)["playback_exit_ok"] > baseline_counts["playback_exit_ok"],
                30,
                f"round {index} playback device release",
            )
            ready = wait_until(
                lambda text: round_ready(text, baseline_counts, terminal_round),
                30 if terminal_round else 10,
                f"round {index} microphone resume or clean session close",
            )
            delta = ready[len(baseline) :]
            peak = input_peak(delta)
            if peak < 500:
                raise RuntimeError(f"round {index}: no audible .100 input in .166 mic trace (peak={peak})")
            if "event=PLAYBACK_DRAIN_TIMEOUT" in delta:
                raise RuntimeError(f"round {index}: XiaoZhi playback did not drain before next input")
            if terminal_round:
                if "event=GOODBYE_WORD_DETECTED" not in delta:
                    raise RuntimeError(f"round {index}: 小智再见 did not trigger the local goodbye detection")
                # The cloud may also close the socket at the same instant; the
                # session must end cleanly either by our protocol goodbye or
                # by the server's own close after the final reply.
                if "event=GOODBYE_SENT" not in delta and "event=WS_CLOSED" not in delta:
                    raise RuntimeError(f"round {index}: session did not end after the goodbye phrase")
            elif "event=GOODBYE_SENT" in delta or "event=GOODBYE_WORD_DETECTED" in delta:
                raise RuntimeError(f"round {index}: goodbye triggered mid-session")
            if not terminal_round and not xz_client_running():
                raise RuntimeError(f"round {index}: xz_client exited after reply")
            result["rounds"].append(
                {
                    "index": index,
                    "prompt": prompt,
                    "source": source["output"],
                    "input_peak_rms": peak,
                    "stt_recognized_count": counts(ready)["stt_recognized"],
                    "tts_start_count": counts(ready)["tts_start"],
                    "tts_stop_count": counts(ready)["tts_stop"],
                    "playback_exit_ok_count": counts(ready)["playback_exit_ok"],
                    "listen_resume_count": counts(ready)["listen_resume"],
                    "terminal_session_close": terminal_round and
                    counts(ready)["summary"] > baseline_counts["summary"],
                    "goodbye_detected": "event=GOODBYE_WORD_DETECTED" in delta,
                    "goodbye_sent": "event=GOODBYE_SENT" in delta,
                    "status": "PASS",
                }
            )

        result["status"] = "PASS"
    except Exception as exc:  # Preserve partial rounds and raw evidence.
        result["status"] = "FAIL"
        result["error"] = str(exc)
    finally:
        result["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        (evidence / "run.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        (evidence / "timeline.log").write_text(timeline(), encoding="utf-8")
        (evidence / "instruction.log").write_text(device(f"cat {INSTRUCTIONS}"), encoding="utf-8")
        (evidence / "device-sha256.txt").write_text(
            device(
                "sha256sum /data/lx06/xiaozhi/xz_client_armhf "
                "/data/lx06/apps/xz_watch.sh /data/lx06/xiaozhi/xz.env"
            ),
            encoding="utf-8",
        )

    print(json.dumps(result, ensure_ascii=False, indent=2))
    print(f"evidence={evidence}")
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

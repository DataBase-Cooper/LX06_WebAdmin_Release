#!/usr/bin/env python3
"""Host-only private sample ledger and A/B driver for the existing C matrix.

No training, calibration, recording, networking or device commands. Reports and
input audio/models belong outside the checkout. See the A T02 evidence note.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import wave

PURPOSES = ("train", "calibration", "acceptance", "diagnostic")
ROOT = Path(__file__).resolve().parents[4]


def sha(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256")
    return digest.hexdigest()


def private_path(path):
    path = path.resolve()
    if path == ROOT or ROOT in path.parents:
        raise ValueError("private input/output must be outside the checkout")
    return path


def artifact(folder, item):
    path = private_path(folder / item["path"])
    if not path.is_file() or sha(path) != item["sha256"]:
        raise ValueError("missing or changed private input")
    return path


def load_ledger(path, purpose):
    path = private_path(path)
    ledger = json.loads(path.read_text())
    if ledger.get("version") != 1 or not ledger.get("models") or not ledger.get("samples"):
        raise ValueError("expected ledger version 1 with models and samples")
    models, labels = [], []
    for item in ledger["models"]:
        label, gate = item["label"], item["threshold"]
        if (not isinstance(label, str) or not label or len(label.encode()) >= 48 or
                any(c.isspace() or c in "=:/\\\"" or ord(c) < 32 for c in label) or
                label in labels):
            raise ValueError("invalid or duplicate model label")
        if isinstance(gate, bool) or not isinstance(gate, (int, float)) or not math.isfinite(gate) or gate <= 0:
            raise ValueError("invalid model threshold")
        model = artifact(path.parent, item)
        if ":" in str(model):
            raise ValueError("model path cannot contain ':' in the detector CLI")
        labels.append(label)
        models.append({**item, "resolved": model})
    if len(models) > 6:
        raise ValueError("existing continuous detector supports at most 6 models")
    selected, ids, content_roles, fingerprints = [], set(), {}, set()
    for item in ledger["samples"]:
        role, origin = item["purpose"], item["origin"]
        if role not in PURPOSES or origin not in ("human", "synthetic", "forensic"):
            raise ValueError("invalid sample purpose/origin")
        if item["id"] in ids or not isinstance(item["id"], str) or not item["id"]:
            raise ValueError("invalid or duplicate sample id")
        ids.add(item["id"])
        if item["label"] not in labels:
            raise ValueError("sample has no loaded model")
        audio = artifact(path.parent, item)
        digest = item["sha256"]
        if digest in content_roles and content_roles[digest] != role:
            raise ValueError("same audio bytes used for different purposes")
        content_roles[digest] = role
        if role == "acceptance" and origin != "human":
            raise ValueError("acceptance requires independent human audio")
        with wave.open(str(audio), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, 16000, "NONE"):
                raise ValueError("samples must be 16k mono s16 PCM WAV")
            if not 0 < wav.getnframes() <= 80000:
                raise ValueError("matrix clips must be nonempty and <=5s; use detect stream for long background")
        if role == purpose:
            if digest in fingerprints:
                raise ValueError("duplicate selected audio would inflate sample counts")
            fingerprints.add(digest)
            selected.append({**item, "resolved": audio})
    for label in labels:
        group = [s for s in selected if s["label"] == label]
        if not group:
            raise ValueError("every loaded word needs selected samples")
        if purpose == "acceptance":
            normal = sum(s.get("condition") == "normal" for s in group)
            other = sum(s.get("condition") in ("far", "soft") for s in group)
            if len(group) < 30 or normal < 20 or other < 10:
                raise ValueError("acceptance requires >=30/word: >=20 normal and >=10 far/soft")
    return models, selected


def run_matrix(evaluator, models, samples, report, detector=None):
    args = [str(evaluator), "matrix", "--policy", "norm", "--phases", "4", "--report-json", str(report)]
    if detector:
        args += ["--detect-executable", str(detector)]
    ordered = []
    for index, model in enumerate(models):
        group = [s for s in samples if s["label"] == model["label"]]
        if index:
            args.append("--")
        args.append(f'{model["label"]}={model["resolved"]}:{model["threshold"]:.9g}')
        args.extend(str(s["resolved"]) for s in group)
        ordered.append(group)
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env={**os.environ, "LX06_WAKE_CLOUD_VERIFY": "off", "LX06_WAKE_FORENSICS": "0"}, timeout=3600)
    trials = [json.loads(line) for line in report.read_text().splitlines()]
    expected = {(r, s, phase) for r, group in enumerate(ordered) for s in range(len(group)) for phase in range(4)}
    seen = set()
    rows = [{"label": m["label"], "trials": 0, "dispatch_trials": [0] * len(models),
             "wake_events": [0] * len(models), "rejected": 0, "no_trigger": 0} for m in models]
    for trial in trials:
        key = (trial["row"], trial["sample"], trial["phase"])
        counts = trial["wake_counts"]
        if key not in expected or key in seen or len(counts) != len(models) or any(type(n) is not int or n < 0 for n in counts):
            raise ValueError("invalid matrix trial report")
        if type(trial["rejection_events"]) is not int or trial["rejection_events"] < 0:
            raise ValueError("invalid rejection count")
        if trial["rejected"] != (not any(counts) and trial["rejection_events"] > 0) or trial["no_trigger"] != (not any(counts) and not trial["rejection_events"]):
            raise ValueError("invalid rejection/no-trigger classification")
        seen.add(key)
        row = rows[key[0]]
        row["trials"] += 1
        for col, count in enumerate(counts):
            row["dispatch_trials"][col] += count > 0
            row["wake_events"][col] += count
        row["rejected"] += trial["rejected"]
        row["no_trigger"] += trial["no_trigger"]
    if seen != expected:
        raise ValueError("incomplete matrix trial report")
    for i, row in enumerate(rows):
        if detector:
            # Old stdout may not expose rejection decisions. Lack of an event
            # cannot distinguish old rejection from no candidate being found.
            row["no_dispatch_unclassified"] = row.pop("no_trigger")
            row["rejection_observation"] = "legacy stdout only; unreported decisions unknown"
        row["recall"] = row["dispatch_trials"][i] / row["trials"]
        row["crosstalk_events"] = sum(n for col, n in enumerate(row["wake_events"]) if col != i)
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--purpose", choices=PURPOSES, required=True)
    parser.add_argument("--evaluator", type=Path, required=True, help="candidate C binary with matrix/report support")
    parser.add_argument("--baseline", type=Path, help="exact frozen old detect binary; never raw030 as a substitute")
    parser.add_argument("--baseline-manifest", type=Path, help="frozen old models/gates and the identical sample set")
    parser.add_argument("--output", type=Path, required=True, help="new private directory, outside checkout")
    args = parser.parse_args(argv)
    manifest_sha = sha(args.manifest)
    models, samples = load_ledger(args.manifest, args.purpose)
    if bool(args.baseline) != bool(args.baseline_manifest):
        raise ValueError("baseline binary and frozen manifest must be supplied together")
    old_models, old_samples = models, samples
    old_manifest_sha = sha(args.baseline_manifest) if args.baseline_manifest else None
    if args.baseline:
        old_models, old_samples = load_ledger(args.baseline_manifest, args.purpose)
        identity = lambda group: sorted((s["id"], s["label"], s["sha256"], s.get("condition")) for s in group)
        if [m["label"] for m in models] != [m["label"] for m in old_models] or identity(samples) != identity(old_samples):
            raise ValueError("A/B must use identical labeled samples and model order")
    evaluator = args.evaluator.resolve(strict=True)
    evaluator_sha = sha(evaluator)
    baseline_sha = sha(args.baseline.resolve(strict=True)) if args.baseline else None
    output = private_path(args.output)
    output.mkdir(mode=0o700)  # exclusive: never overwrite historical evidence
    summary = {"version": 1, "purpose": args.purpose, "cloud_verify": "off", "phases": 4,
               "manifest_sha256": manifest_sha, "evaluator_sha256": evaluator_sha,
               "baseline_sha256": baseline_sha, "status": "incomplete", "acceptance_passed": False,
               "latency_p95_ms": None, "latency_status": "pending end-of-speech/WAKE timing evidence",
               "device_status": "pending device", "sample_sha256": [s["sha256"] for s in samples],
               "models": [{k: m[k] for k in ("label", "sha256", "threshold")} for m in models]}
    result_path = output / "summary.json"
    def save():
        result_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    save()
    try:
        summary["candidate"] = run_matrix(evaluator, models, samples, output / "candidate.jsonl")
        if args.baseline:
            summary["baseline_manifest_sha256"] = old_manifest_sha
            summary["baseline_models"] = [{k: m[k] for k in ("label", "sha256", "threshold")} for m in old_models]
            summary["baseline"] = run_matrix(evaluator, old_models, old_samples, output / "baseline.jsonl", args.baseline.resolve())
            summary["recall_not_lower"] = all(a["recall"] >= b["recall"] for a, b in zip(summary["candidate"], summary["baseline"]))
        # Detect input mutation during either run; no calibration/training is done.
        load_ledger(args.manifest, args.purpose)
        if sha(args.manifest) != manifest_sha or (args.baseline_manifest and sha(args.baseline_manifest) != old_manifest_sha):
            raise ValueError("sample ledger changed during run")
        if sha(evaluator) != evaluator_sha or (args.baseline and sha(args.baseline.resolve()) != baseline_sha):
            raise ValueError("detector bytes changed during run")
        if args.baseline:
            load_ledger(args.baseline_manifest, args.purpose)
        summary["status"] = "offline-matrix-complete"
    except Exception:
        summary["status"] = "failed-incomplete"
        raise
    finally:
        save()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError, wave.Error) as error:
        raise SystemExit(f"matrix failed: {type(error).__name__}: {error}") from None

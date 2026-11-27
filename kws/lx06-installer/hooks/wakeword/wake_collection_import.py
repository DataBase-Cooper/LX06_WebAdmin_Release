#!/usr/bin/env python3
"""Join private web88 exports to a private matrix ledger. Never trains/calibrates.

--models is a private JSON object with version=1 and the frozen `models` array
used by wake_matrix.py. Export archives, models and output stay outside Git.
Partial (<30/word) collections may be imported; the acceptance runner still
enforces its independent sample/condition requirements before evaluation.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import tarfile
import wave

from wake_matrix import PURPOSES, artifact, private_path


def import_collection(model_file, archives, purpose, output):
    model_file = private_path(model_file)
    model_doc = json.loads(model_file.read_text(encoding='utf-8'))
    if model_doc.get('version') != 1 or not isinstance(model_doc.get('models'), list):
        raise ValueError('expected version 1 frozen models array')
    models, labels = [], set()
    for item in model_doc['models']:
        label = item['label']
        if (not isinstance(label, str) or not label or len(label.encode()) >= 24 or
                any(c.isspace() or c in '/\\=:."|' or ord(c) < 32 for c in label) or label in labels):
            raise ValueError('invalid or duplicate model label')
        labels.add(label)
        gate = item.get('threshold')
        if type(gate) not in (int, float) or not math.isfinite(gate) or gate <= 0:
            raise ValueError('invalid frozen model threshold')
        models.append({**item, 'path': str(artifact(model_file.parent, item))})
    if not 1 <= len(models) <= 6 or purpose not in ('acceptance', 'calibration', 'diagnostic'):
        raise ValueError('expected 1..6 models and an independent collection purpose')
    # A frozen full ledger may also identify training/calibration audio. Keep
    # those references so joining exports cannot hide known purpose overlap.
    prior, prior_ids, known_hashes = [], set(), set()
    for sample in model_doc.get('samples', []):
        if (not isinstance(sample.get('id'), str) or not sample['id'] or sample['id'] in prior_ids or
                sample.get('label') not in labels or sample.get('purpose') not in PURPOSES or
                sample.get('origin') not in ('human', 'synthetic', 'forensic')):
            raise ValueError('invalid existing sample inventory')
        prior_ids.add(sample['id'])
        prior.append({**sample, 'path': str(artifact(model_file.parent, sample))})
        known_hashes.add(sample['sha256'])
    archives = [private_path(p) for p in archives]
    output = private_path(output)
    output.mkdir(mode=0o700)  # exclusive; never replace a previous collection
    marker = output / 'INCOMPLETE'
    marker.write_text('Import incomplete; do not evaluate this directory.\n')
    entries, hashes = {}, set()
    for archive in archives:
        # Streaming only: do not extractall or trust tar paths, links or lengths.
        with tarfile.open(archive, mode='r|*') as stream:
            for member in stream:
                parts = PurePosixPath(member.name).parts
                if (not member.isfile() or len(parts) != 3 or member.name != '/'.join(parts) or
                        parts[0] not in labels or not re.fullmatch(r's-[A-Za-z0-9]{6}', parts[1]) or
                        parts[2] not in ('sample.json', 'audio.wav')):
                    raise ValueError('unexpected archive member')
                key = '/'.join(parts[:2])
                if key not in entries:
                    if sum(k.startswith(parts[0] + '/') for k in entries) >= 100:
                        raise ValueError('more than 100 clips/word/purpose')
                    entries[key] = {}
                entry = entries[key]
                limit = 767 if parts[2] == 'sample.json' else 180000
                if parts[2] in entry or not 0 < member.size <= limit:
                    raise ValueError('duplicate or oversized archive member')
                data = stream.extractfile(member).read(limit + 1)
                if len(data) != member.size:
                    raise ValueError('truncated archive member')
                if parts[2] == 'sample.json':
                    entry[parts[2]] = json.loads(data)
                else:
                    folder = output / parts[0] / parts[1]
                    folder.mkdir(parents=True, mode=0o700)
                    path = folder / 'audio.wav'
                    with path.open('xb') as dest:
                        dest.write(data)
                    path.chmod(0o600)
                    entry[parts[2]] = path
    samples = []
    for key, entry in sorted(entries.items()):
        meta, path = entry.get('sample.json'), entry.get('audio.wav')
        label, slot = key.split('/')
        if not isinstance(meta, dict) or path is None:
            raise ValueError('incomplete sample pair')
        if (meta.get('label') != label or meta.get('id') != slot or meta.get('path') != key + '/audio.wav' or
                meta.get('purpose') != purpose or meta.get('origin') != 'human' or
                meta.get('condition') not in ('normal', 'far', 'soft')):
            raise ValueError('sample identity/purpose mismatch')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != meta.get('sha256') or digest in hashes or digest in known_hashes:
            raise ValueError('changed or duplicate audio bytes')
        hashes.add(digest)
        with wave.open(str(path), 'rb') as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) != (1, 2, 16000, 'NONE'):
                raise ValueError('expected mono 16k s16 PCM WAV')
            if not 0 < wav.getnframes() <= 80000:
                raise ValueError('expected nonempty <=5s clip')
            if len(wav.readframes(wav.getnframes())) != wav.getnframes() * 2:
                raise ValueError('truncated WAV frames')
        # IDs are global in the matrix ledger, whereas web slots are word-local.
        if key in prior_ids:
            raise ValueError('sample ID already exists in the frozen inventory')
        samples.append({**meta, 'id': key})
    if not samples:
        raise ValueError('empty collection')
    ledger = {'version': 1, 'models': models, 'samples': prior + samples,
              'purpose_inventory': 'provided-inputs-only; human independence requires operator evidence',
              'acceptance_passed': False, 'device_status': 'pending device'}
    ledger_path = output / 'ledger.json'
    with ledger_path.open('x', encoding='utf-8') as dest:
        dest.write(json.dumps(ledger, ensure_ascii=False, indent=2) + '\n')
    ledger_path.chmod(0o600)
    marker.unlink()
    return ledger_path


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--models', type=Path, required=True)
    p.add_argument('--archive', action='append', type=Path, required=True)
    p.add_argument('--purpose', choices=('acceptance', 'calibration', 'diagnostic'), required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args(argv)
    print(import_collection(args.models, args.archive, args.purpose, args.output))


if __name__ == '__main__':
    main()

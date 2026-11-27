"""Keep the staged microphone tap and native-audio startup manifest in sync."""

import argparse
import hashlib
from pathlib import Path
import re
import stat

TAP = "usr/lib/lx06/xiaoai/processed-mic-tap.so"
MANIFEST = "usr/lib/lx06/native-audio.sha256"
HASH_LINE = re.compile(r"([0-9a-f]{64})  /([a-zA-Z0-9_./-]+)")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def refresh(root, expected_tap):
    manifest = root / MANIFEST
    tap = root / TAP
    if tap.is_symlink() or not tap.is_file() or tap.stat().st_nlink != 1:
        raise ValueError("staged microphone tap must be a regular file with one link")
    if digest(tap) != expected_tap:
        raise ValueError("staged microphone tap does not match the approved build")
    if manifest.is_symlink() or not manifest.is_file() or manifest.stat().st_nlink != 1:
        raise ValueError("invalid staged native-audio manifest")

    updated = []
    seen = set()
    for line in manifest.read_text(encoding="ascii").splitlines():
        match = HASH_LINE.fullmatch(line)
        if match is None:
            raise ValueError("invalid native-audio manifest record")
        expected, path = match.groups()
        if path in seen or any(part in {"", ".", ".."} for part in path.split("/")):
            raise ValueError(f"invalid or duplicated native-audio path: {path}")
        seen.add(path)
        if path == TAP:
            updated.append(f"{expected_tap}  /{TAP}")
        else:
            if digest(root / path) != expected:
                raise ValueError(f"unexpected native-audio change: {path}")
            updated.append(line)

    if TAP not in seen:
        raise ValueError("staged native-audio manifest is missing the tap")
    tap.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP | stat.S_IROTH)
    manifest.write_text("\n".join(updated) + "\n", encoding="ascii", newline="\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("staging_root", type=Path)
    parser.add_argument("approved_tap_sha256")
    args = parser.parse_args()
    if args.staging_root.resolve() == Path("/"):
        parser.error("provide a staging root")
    refresh(args.staging_root, args.approved_tap_sha256)


if __name__ == "__main__":
    main()

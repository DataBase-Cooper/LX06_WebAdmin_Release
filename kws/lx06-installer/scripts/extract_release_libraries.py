"""Prepare a local ARM link directory from an enhanced LX06 SquashFS image."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess


LIBRARIES = {
    "lib/libc-2.25.so": ("libc.so.6", "sysarm-libc-2.25.so"),
    "lib/libm-2.25.so": ("libm.so.6", "sysarm-libm-2.25.so"),
    "lib/libdl-2.25.so": ("libdl.so.2", "sysarm-libdl.so.2"),
    "lib/libpthread-2.25.so": ("libpthread.so.0", "sysarm-libpthread.so.0"),
    "lib/libutil-2.25.so": ("libutil.so.1",),
    "lib/libgcc_s.so.1": ("libgcc_s.so.1", "sysarm-libgcc_s.so.1"),
    "lib/ld-2.25.so": ("ld-linux-armhf.so.3", "sysarm-ld-2.25.so"),
    "usr/lib/libssl.so.1.0.0": ("libssl.so.1.0.0", "sysarm-libssl.so.1.0.0"),
    "usr/lib/libcrypto.so.1.0.0": ("libcrypto.so.1.0.0", "sysarm-libcrypto.so.1.0.0"),
    "usr/lib/libopus.so.0.6.1": ("libopus.so.0.6.1", "libopus.so.0"),
    "usr/lib/libasound.so.2.0.0": ("libasound.so.2", "sysarm-libasound.so.2.0.0"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sha256", help="First verify this SquashFS SHA-256")
    args = parser.parse_args()
    if not args.image.is_file() or args.output.exists():
        parser.error("Supply an existing image and a new output directory.")
    if args.sha256:
        with args.image.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != args.sha256.lower():
                parser.error("SquashFS SHA-256 differs.")
    with args.image.open("rb") as stream:
        if stream.read(4) != b"hsqs":
            parser.error("Expected SquashFS root image.")
    tool = shutil.which("unsquashfs")
    if tool is None:
        parser.error("Install squashfs-tools.")
    inputs = {}
    for source in LIBRARIES:
        raw = subprocess.check_output([tool, "-cat", str(args.image.resolve()), source])
        if raw[:5] != b"\x7fELF\x01" or int.from_bytes(raw[18:20], "little") != 40:
            raise ValueError(f"Expected ARM library at {source}")
        inputs[source] = raw
    args.output.mkdir(parents=True)
    for source, names in LIBRARIES.items():
        for name in names:
            (args.output / name).write_bytes(inputs[source])
    print(args.output.resolve())


if __name__ == "__main__":
    main()

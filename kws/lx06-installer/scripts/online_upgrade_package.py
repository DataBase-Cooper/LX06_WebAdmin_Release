#!/usr/bin/env python3
"""Build/verify LX06OTA1 packages using only the Python standard library.

Wire layout: 4096 ASCII header bytes, raw boot.img, raw root.squashfs.
The header starts LX06OTA1\n, then key=value lines, then an empty line and
NUL padding. SHA-256 detects corruption; it is NOT signature authentication.
This tool never flashes, runs payloads, or changes shared DATA.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import os
import stat
import tempfile
from contextlib import contextmanager

FIELDS = frozenset(("model", "version", "kernel_size", "kernel_sha256",
                    "rootfs_size", "rootfs_sha256", "payload_sha256", "data_schema"))

HEADER_SIZE = 4096
CHUNK_SIZE = 64 * 1024


def transfer(source, size, payload_hash, target=None):
    digest = hashlib.sha256()
    while size:
        block = source.read(min(size, CHUNK_SIZE))
        if not block:
            raise ValueError("truncated payload")
        size -= len(block)
        digest.update(block)
        payload_hash.update(block)
        if target is not None:
            target.write(block)
    return digest.hexdigest()


LIMITS = {"kernel": 6 * 1024 * 1024, "rootfs": 40 * 1024 * 1024}


def valid_version(version):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,62}", version):
        raise ValueError("invalid version (1..63 ASCII identifier characters)")


def image_prefix(source, name, size):
    if not 96 <= size <= LIMITS[name]:
        raise ValueError("invalid " + name + " size")
    offset = source.tell()
    prefix = source.read(96)
    source.seek(offset)
    if name == "kernel":
        page = int.from_bytes(prefix[36:40], "little")
        parts = [int.from_bytes(prefix[i:i+4], "little") for i in (8, 16, 24)]
        if prefix[:8] != b"ANDROID!" or page != 2048 or not parts[0]:
            raise ValueError("invalid LX06 Android boot header")
        if page + sum((n + page - 1) // page * page for n in parts) > size:
            raise ValueError("Android boot sections exceed image size")
    elif (prefix[:4] != b"hsqs" or prefix[20:22] != b"\x04\x00" or
          prefix[28:32] != b"\x04\x00\x00\x00" or
          not 96 <= int.from_bytes(prefix[40:48], "little") <= size):
        raise ValueError("invalid target SquashFS v4/XZ superblock")


@contextmanager
def regular_input(path):
    path = Path(path)
    if any(parent.is_symlink() for parent in (path, *path.parents)):
        raise ValueError("symbolic link inputs are not allowed")
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode):
        raise ValueError("input is not a regular file")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0))
    with os.fdopen(fd, "rb") as source:
        after = os.fstat(source.fileno())
        if not stat.S_ISREG(after.st_mode) or (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
            raise ValueError("input changed during open")
        yield source


def build(kernel, rootfs, version, output):
    valid_version(version)
    output = Path(output)
    if os.path.lexists(output):
        raise FileExistsError("output exists; releases are immutable")
    if any(p.is_symlink() for p in output.parents):
        raise ValueError("symbolic link output directories are not allowed")
    payload = hashlib.sha256()
    fields = {"model": "LX06", "version": version}
    fd, temporary = tempfile.mkstemp(prefix=".lx06-ota-", dir=output.parent)
    try:
        with os.fdopen(fd, "w+b") as target:
            target.write(bytes(HEADER_SIZE))
            for name, path in (("kernel", kernel), ("rootfs", rootfs)):
                with regular_input(path) as source:
                    before = os.fstat(source.fileno())
                    size = before.st_size
                    image_prefix(source, name, size)
                    fields[name + "_size"] = str(size)
                    fields[name + "_sha256"] = transfer(source, size, payload, target)
                    after = os.fstat(source.fileno())
                    if source.read(1) or (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                        raise ValueError("input changed during build")
            fields["payload_sha256"] = payload.hexdigest()
            fields["data_schema"] = "1"
            header = ("LX06OTA1\n" + "".join(k + "=" + v + "\n" for k, v in fields.items()) + "\n").encode("ascii")
            parse_header(header.ljust(HEADER_SIZE, b"\0"))
            target.seek(0)
            target.write(header.ljust(HEADER_SIZE, b"\0"))
            target.flush()
            os.fsync(target.fileno())
        verify(temporary)
        # Atomic no-clobber publication on NTFS and POSIX; never replace a release.
        os.link(temporary, output)
    finally:
        os.unlink(temporary)
    return fields


def parse_header(header):
    if len(header) != HEADER_SIZE:
        raise ValueError("truncated header")
    end = header.find(b"\n\n")
    if end < 0 or any(header[end + 2:]):
        raise ValueError("header requires an empty line followed only by NUL padding")
    text = header[:end].decode("ascii")
    if any(ord(c) < 32 and c != "\n" for c in text) or "\x7f" in text:
        raise ValueError("control character in header")
    lines = text.split("\n")
    if lines[0] != "LX06OTA1":
        raise ValueError("invalid LX06OTA1 magic")
    fields = {}
    for line in lines[1:]:
        if line.count("=") != 1:
            raise ValueError("invalid key=value line")
        key, value = line.split("=")
        if key not in FIELDS or key in fields:
            raise ValueError("unknown or duplicate header field")
        fields[key] = value
    if fields.keys() != FIELDS:
        raise ValueError("missing header field")
    if fields["model"] != "LX06" or fields["data_schema"] != "1":
        raise ValueError("unsupported model or data schema")
    valid_version(fields["version"])
    for name in ("kernel", "rootfs"):
        if not re.fullmatch(r"[1-9][0-9]{0,7}", fields[name + "_size"]):
            raise ValueError("invalid decimal " + name + " size")
        if not 96 <= int(fields[name + "_size"]) <= LIMITS[name]:
            raise ValueError("invalid " + name + " size")
    for key in ("kernel_sha256", "rootfs_sha256", "payload_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", fields[key]):
            raise ValueError("invalid lowercase SHA-256")
    return fields


def verify(path):
    with regular_input(path) as source:
        fields = parse_header(source.read(HEADER_SIZE))
        payload = hashlib.sha256()
        for name in ("kernel", "rootfs"):
            image_prefix(source, name, int(fields[name + "_size"]))
            actual = transfer(source, int(fields[name + "_size"]), payload)
            if actual != fields[name + "_sha256"]:
                raise ValueError(name + " SHA-256 mismatch")
        if source.read(1):
            raise ValueError("trailing bytes after payload")
        if payload.hexdigest() != fields["payload_sha256"]:
            raise ValueError("payload SHA-256 mismatch")
    return fields


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build", help="build a package")
    for name in ("kernel", "rootfs", "version", "output"):
        builder.add_argument("--" + name, required=True)
    checker = commands.add_parser("verify", help="verify a package (not authenticate its origin)")
    checker.add_argument("path")
    args = parser.parse_args(argv)
    try:
        if args.command == "build":
            fields = build(args.kernel, args.rootfs, args.version, args.output)
        else:
            fields = verify(args.path)
    except (OSError, ValueError) as exc:
        parser.exit(1, "error: " + str(exc) + "\n")
    print(json.dumps(fields, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

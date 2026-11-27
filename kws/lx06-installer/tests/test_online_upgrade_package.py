"""Offline CLI acceptance tests; all images and outputs live in Hermes scratch."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "online_upgrade_package.py"
SCRATCH = Path(os.environ.get("TMPDIR", Path.home() / "AppData/Local/hermes/cache/scratch"))


class PackageTests(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="lx06-ota-test-", dir=SCRATCH)
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.kernel = self.base / "boot.img"
        self.rootfs = self.base / "root.squashfs"
        self.output = self.base / "update.lx06ota"
        kernel = bytearray(b"ANDROID!" + bytes(range(256)) * 512)
        struct.pack_into("<I", kernel, 8, 4096)
        struct.pack_into("<I", kernel, 16, 2048)
        struct.pack_into("<I", kernel, 24, 0)
        struct.pack_into("<I", kernel, 36, 2048)
        self.kernel.write_bytes(kernel)
        root = bytearray(128)
        root[:4] = b"hsqs"
        struct.pack_into("<H", root, 20, 4)
        struct.pack_into("<HH", root, 28, 4, 0)
        struct.pack_into("<Q", root, 40, 96)
        self.rootfs.write_bytes(root)

    def cli(self, *args):
        return subprocess.run([sys.executable, "-B", str(SCRIPT), *map(str, args)],
                              cwd=self.base, capture_output=True, text=True, timeout=30)

    def build(self, **changes):
        options = dict(kernel=self.kernel, rootfs=self.rootfs, version="V20261101.1-rc24", output=self.output)
        options.update(changes)
        args = ["build"]
        for key, value in options.items():
            args.extend(["--" + key, value])
        return self.cli(*args)

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_rejected(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("error:", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_payload_corruption_truncation_and_trailer(self):
        self.assert_ok(self.build())
        valid = self.output.read_bytes()
        damaged = bytearray(valid)
        damaged[4096 + 9] ^= 1
        damaged_root = bytearray(valid)
        damaged_root[-1] ^= 1
        for label, data in (("kernel damage", damaged), ("root damage", damaged_root),
                            ("truncated", valid[:-1]), ("trailer", valid + b"X")):
            with self.subTest(label=label):
                self.output.write_bytes(data)
                self.assert_rejected(self.cli("verify", self.output))

    def test_header_is_strict(self):
        self.assert_ok(self.build())
        valid = self.output.read_bytes()
        header = valid[:4096].split(b"\n\n", 1)[0]
        mutations = {
            "magic": header.replace(b"LX06OTA1", b"LX06OTA2"),
            "model": header.replace(b"model=LX06", b"model=LX04"),
            "schema": header.replace(b"data_schema=1", b"data_schema=2"),
            "unknown": header + b"\nscript=evil.sh",
            "duplicate": header + b"\nmodel=LX06",
            "missing": header.replace(b"model=LX06\n", b""),
            "no equals": header + b"\ninvalid",
            "extra equals": header.replace(b"model=LX06", b"model=LX06=x"),
            "CRLF": header.replace(b"\n", b"\r\n"),
            "nonascii": header.replace(b"model=LX06", b"model=\xff"),
            "uppercase hash": header.replace(hashlib.sha256(self.kernel.read_bytes()).hexdigest().encode(), hashlib.sha256(self.kernel.read_bytes()).hexdigest().upper().encode()),
            "leading zero size": header.replace(b"rootfs_size=128", b"rootfs_size=0128"),
            "signed size": header.replace(b"rootfs_size=128", b"rootfs_size=+128"),
            "negative size": header.replace(b"rootfs_size=128", b"rootfs_size=-128"),
            "empty size": header.replace(b"rootfs_size=128", b"rootfs_size="),
            "huge number": header.replace(b"rootfs_size=128", b"rootfs_size=" + b"9" * 3000),
        }
        for label, text in mutations.items():
            with self.subTest(label=label):
                self.output.write_bytes((text + b"\n\n").ljust(4096, b"\0") + valid[4096:])
                self.assert_rejected(self.cli("verify", self.output))
        for label, data in (("padding", valid[:4095] + b"X" + valid[4096:]),
                            ("no blank line", header.ljust(4096, b"\0") + valid[4096:]),
                            ("short header", valid[:100])):
            with self.subTest(label=label):
                self.output.write_bytes(data)
                self.assert_rejected(self.cli("verify", self.output))

    def test_reject_invalid_images_and_version_without_output(self):
        for version in ("", "../rc25", "rc25\nmodel=LX04", "版本", "a" * 64):
            with self.subTest(version=version):
                self.assert_rejected(self.build(version=version))
                self.assertFalse(self.output.exists())
        for path, data in ((self.kernel, b"not a boot image"), (self.rootfs, b"hsqs")):
            original = path.read_bytes()
            path.write_bytes(data)
            self.assert_rejected(self.build())
            self.assertFalse(self.output.exists())
            path.write_bytes(original)
        original = self.rootfs.read_bytes()
        for size in (0, 95, 129):
            root = bytearray(original)
            struct.pack_into("<Q", root, 40, size)
            self.rootfs.write_bytes(root)
            self.assert_rejected(self.build())
            self.assertFalse(self.output.exists())
        self.rootfs.write_bytes(original)
        with self.kernel.open("r+b") as f:
            f.truncate(6 * 1024 * 1024 + 1)
        self.assert_rejected(self.build())
        self.assertFalse(self.output.exists())

    def test_boot_layout_must_fit_and_squashfs_must_match_target(self):
        original = self.kernel.read_bytes()
        for offset, value in ((36, 0), (36, 1234), (8, 0), (8, 0xffffffff), (16, 0xffffffff), (24, 0xffffffff)):
            kernel = bytearray(original)
            struct.pack_into("<I", kernel, offset, value)
            self.kernel.write_bytes(kernel)
            self.assert_rejected(self.build())
            self.assertFalse(self.output.exists())
        self.kernel.write_bytes(original)
        original = self.rootfs.read_bytes()
        for offset, value in ((20, 99), (28, 3), (30, 1)):
            root = bytearray(original)
            struct.pack_into("<H", root, offset, value)
            self.rootfs.write_bytes(root)
            self.assert_rejected(self.build())
            self.assertFalse(self.output.exists())

    def test_no_overwrite_and_failed_build_leaves_no_output(self):
        self.output.write_bytes(b"keep immutable release")
        self.assert_rejected(self.build())
        self.assertEqual(self.output.read_bytes(), b"keep immutable release")
        self.output.unlink()
        self.assert_rejected(self.build(rootfs=self.base / "missing"))
        self.assertFalse(self.output.exists())
        self.assertEqual(sorted(p.name for p in self.base.iterdir()), ["boot.img", "root.squashfs"])

    def test_verifier_rejects_self_consistent_invalid_metadata_and_image(self):
        self.assert_ok(self.build())
        valid = self.output.read_bytes()
        for before, after in ((b"version=V20261101.1-rc24", b"version=../bad"),
                              (b"rootfs_size=128", b"rootfs_size=41943041")):
            header = valid[:4096].split(b"\n\n")[0].replace(before, after)
            self.output.write_bytes((header + b"\n\n").ljust(4096, b"\0") + valid[4096:])
            self.assert_rejected(self.cli("verify", self.output))
        kernel = b"NOTBOOT!" + self.kernel.read_bytes()[8:]
        rootfs = self.rootfs.read_bytes()
        fields = dict(line.split(b"=", 1) for line in valid[:4096].split(b"\n\n")[0].split(b"\n")[1:])
        fields[b"kernel_sha256"] = hashlib.sha256(kernel).hexdigest().encode()
        fields[b"payload_sha256"] = hashlib.sha256(kernel + rootfs).hexdigest().encode()
        header = b"LX06OTA1\n" + b"".join(k + b"=" + v + b"\n" for k, v in fields.items()) + b"\n"
        self.output.write_bytes(header.ljust(4096, b"\0") + kernel + rootfs)
        self.assert_rejected(self.cli("verify", self.output))

    def test_verifier_rejects_rehashed_structurally_invalid_images(self):
        self.assert_ok(self.build())
        valid = self.output.read_bytes()
        original = dict(line.split(b"=", 1) for line in valid[:4096].split(b"\n\n")[0].split(b"\n")[1:])
        for kind, offset, value, fmt in (("kernel", 36, 0, "<I"), ("kernel", 8, 0xffffffff, "<I"),
                                        ("rootfs", 20, 99, "<H"), ("rootfs", 28, 3, "<H")):
            kernel, root = bytearray(self.kernel.read_bytes()), bytearray(self.rootfs.read_bytes())
            struct.pack_into(fmt, kernel if kind == "kernel" else root, offset, value)
            fields = original.copy()
            fields[b"kernel_sha256"] = hashlib.sha256(kernel).hexdigest().encode()
            fields[b"rootfs_sha256"] = hashlib.sha256(root).hexdigest().encode()
            fields[b"payload_sha256"] = hashlib.sha256(kernel + root).hexdigest().encode()
            header = b"LX06OTA1\n" + b"".join(k+b"="+v+b"\n" for k,v in fields.items()) + b"\n"
            self.output.write_bytes(header.ljust(4096, b"\0") + kernel + root)
            self.assert_rejected(self.cli("verify", self.output))

    def test_reject_symlinks(self):
        link = self.base / "link"
        try:
            link.symlink_to(self.kernel)
        except OSError:
            self.skipTest("symlink privilege unavailable")
        self.assert_rejected(self.build(kernel=link))
        self.assertFalse(self.output.exists())
        self.assert_ok(self.build())
        link.unlink()
        link.symlink_to(self.output)
        self.assert_rejected(self.cli("verify", link))
        self.assert_rejected(self.build(output=link))

    def test_build_verify_roundtrip(self):
        self.assert_ok(self.build())
        package = self.output.read_bytes()
        kernel, rootfs = self.kernel.read_bytes(), self.rootfs.read_bytes()
        digest = lambda value: hashlib.sha256(value).hexdigest()
        expected = ("LX06OTA1\nmodel=LX06\nversion=V20261101.1-rc24\n"
                    f"kernel_size={len(kernel)}\nkernel_sha256={digest(kernel)}\n"
                    f"rootfs_size={len(rootfs)}\nrootfs_sha256={digest(rootfs)}\n"
                    f"payload_sha256={digest(kernel + rootfs)}\ndata_schema=1\n\n").encode("ascii")
        self.assertEqual(package[:4096], expected.ljust(4096, b"\0"))
        self.assertEqual(package[4096:], kernel + rootfs)
        self.assert_ok(self.cli("verify", self.output))
        self.assertFalse((self.base / "SHA256SUMS.txt").exists())


if __name__ == "__main__":
    unittest.main()

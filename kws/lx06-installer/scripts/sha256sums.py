"""Write per-package integrity manifests (LX04 layout):
   <pkg>/SHA256SUMS.txt  (3 columns: hash  size  name, relative to pkg dir)
   <pkg>/version.txt     (single line firmware version)
   <pkg>/SOURCE.txt      (key=value provenance)
Invoked by build_release.py: sha256sums.py <release_dir> (reads official-ssh/firmware.json)
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

BASE_BIN = "mico_all_911151b95_1.94.13.bin"
BASE_BYTES = 43478328
BASE_MD5 = "b202de2bbe149ce884aa500911151b95"
CDN = "http://api.miwifi.com/rs/grayupgrade/v2/lx06 (stable channel)"

# pkg dir -> (firmware file, version line, source label, patch note)
PKGS = {
    "official": ("root.squashfs", "unpatched",
                 "LX06 official 1.94.13 (unpatched root.squashfs)",
                 "extracted from official stable base; no patch applied"),
    "official-ssh": ("root.squashfs", "", "", ""),
    "official-hooked": ("root.squashfs", "1.94.13 + boot hook",
                        "LX06 1.94.13 hooked base (stock + boot() tail hook)",
                        "hook calls /usr/lib/lx06/ssh-apply.sh + init-launch.sh at boot; "
                        "pre-flash SSH bake appends lx06-ssh.conf onto this image"),
}


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    release = Path(sys.argv[1])
    metadata = json.loads((release / "official-ssh/firmware.json").read_text(encoding="utf-8"))
    actual = sha256_of(release / "official-ssh/root.squashfs")
    if actual != metadata["sha256"]:
        raise SystemExit("official-ssh image SHA256 does not match firmware.json; refusing to relabel it")
    if metadata["feature"] != "mfcc-lifter32" or metadata["model_version"] != 8:
        raise SystemExit("unexpected wake feature/model version")
    version = metadata["version"]
    if not version or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-" for c in version):
        raise SystemExit("invalid firmware version")
    for pkg, (fw, _ver, label, note) in PKGS.items():
        pdir = release / pkg
        fwp = pdir / fw
        if not fwp.exists():
            print("WARN missing", fwp, file=sys.stderr)
            continue

        version_line = "user-1.94.13-release"
        package_metadata_path=pdir/'firmware.json'
        package_metadata=json.loads(package_metadata_path.read_text(encoding='utf-8')) if package_metadata_path.exists() else {}
        if package_metadata.get('did_width_fix'):
            if sha256_of(fwp)!=package_metadata['sha256']:
                raise SystemExit('package image mismatch: '+pkg)
            version_line=package_metadata['version']
            label=f"LX06 official 1.94.13 base + DID int64 fix ({version_line}; {pkg})"
            note='DID correction only at verified miio_helper response_dinfo call sites; original device identity and keys; no DATA payload'
        if pkg == "official-ssh":
            version_line = version
            label = f"LX06 official 1.94.13 + SSH89/Web88 + MFCC v8 ({version})"
            note = ("candidate; acceptance_complete=" + str(metadata["acceptance_complete"]).lower() +
                    "; MFCC mfcc-lifter32 model_version=8; programs/assets in read-only firmware; "
                    "record state, login sessions and routine SSH/Web/wake logs in /tmp; "
                    "DATA retains passwords/host keys, user enrollment/models/calibration and recovery state; "
                    "no bundled user models, recordings or startup probes; calibration bound to exact model SHA256; "
                    "shared native dialog-light API; companion XiaoZhi/Doubao scripts optional via Web88")
            if metadata.get('did_width_fix'):
                note+='; DID int64 fix; speaker-authenticated Xiaomi control ASR; dual interruption modes'

        # ---- SHA256SUMS.txt ----
        line = f"{sha256_of(fwp)}  {fwp.stat().st_size}  {fw}\r\n"
        (pdir / "SHA256SUMS.txt").write_text(line, encoding="utf-8", newline="")

        # ---- version.txt ----
        (pdir / "version.txt").write_text(version_line + "\r\n",
                                          encoding="utf-8", newline="")

        # ---- SOURCE.txt ----
        src = (
            f"{label}\r\n"
            "\r\n"
            f"archive_name={BASE_BIN}\r\n"
            f"archive_sha256=see SHA256SUMS.txt (firmware payload hash)\r\n"
            f"archive_md5={BASE_MD5}\r\n"
            f"archive_bytes={BASE_BYTES}\r\n"
            f"source={CDN}\r\n"
            f"patch={note}\r\n"
            "\r\n"
        )
        (pdir / "SOURCE.txt").write_text(src, encoding="utf-8", newline="")
        print("MANIFEST_OK", pkg)


if __name__ == "__main__":
    main()

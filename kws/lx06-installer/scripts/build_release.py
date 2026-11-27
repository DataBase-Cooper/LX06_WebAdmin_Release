"""LX06 green-release builder: icon -> vet -> go build -> stage -> sha256 -> zip.
Invoked by build_windows.bat. Keeps all cmd-fragile logic in Python.
"""
from __future__ import annotations

import argparse
import os
import re
import json
import shutil
import subprocess
import sys
import zipfile
from datetime import datetime
from pathlib import Path, PurePosixPath
from release_payloads import sha256, verify_scripts

ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], **kw) -> None:
    print("+", " ".join(cmd))
    r = subprocess.run(cmd, cwd=ROOT, **kw)
    if r.returncode != 0:
        raise SystemExit(f"step failed (rc={r.returncode}): {' '.join(cmd)}")


def find_go() -> str:
    return shutil.which("go") or "go"


def find_rsrc() -> str:
    cand = os.path.expandvars(r"%USERPROFILE%\go\bin\rsrc.exe")
    if os.path.exists(cand):
        return cand
    if shutil.which("rsrc"):
        return "rsrc"
    raise SystemExit("missing rsrc (github.com/akavel/rsrc) and no prebuilt syso")


def verified_bundle(root: Path, manifest_name: str, expected_sha: str, required: set[str]) -> dict:
    """Consume an explicitly pinned immutable image/tool package, read-only."""
    root = root.resolve()
    manifest_path = root / manifest_name
    if (not re.fullmatch('[0-9a-f]{64}', expected_sha or '') or
            manifest_path.is_symlink() or not manifest_path.is_file() or sha256(manifest_path) != expected_sha):
        raise SystemExit('audited input manifest SHA256 mismatch: ' + manifest_name)
    manifest = json.loads(manifest_path.read_text(encoding='utf8'))
    files = manifest.get('files')
    if not isinstance(files, dict) or not required <= files.keys():
        raise SystemExit('audited input manifest lacks complete payload: ' + manifest_name)
    for name, item in files.items():
        if not isinstance(name, str):
            raise SystemExit('invalid input manifest path')
        relative = PurePosixPath(name)
        if (not relative.parts or relative.is_absolute() or
                any(part in ('.', '..') for part in relative.parts) or
                any(c in name for c in ('\\', ':')) or name != relative.as_posix()):
            raise SystemExit('invalid input manifest path')
        file = root.joinpath(*relative.parts)
        if (not file.resolve().is_relative_to(root) or file.is_symlink() or not file.is_file() or
                not isinstance(item, dict) or type(item.get('bytes')) is not int or
                item['bytes'] != file.stat().st_size or sha256(file) != item.get('sha256')):
            raise SystemExit('audited input file mismatch: ' + name)
    # Only manifest-listed immutable bytes may enter a release through copytree.
    copied = ('official', 'official-ssh', 'official-hooked') if manifest_name == 'image-index.json' else ('.',)
    actual = set()
    for prefix in copied:
        for file in (root/prefix).rglob('*'):
            if file.is_symlink():
                raise SystemExit('symlink in audited input bundle')
            if file.is_file() and file != manifest_path:
                actual.add(file.relative_to(root).as_posix())
    expected = {name for name in files if copied == ('.',) or name.split('/', 1)[0] in copied}
    if actual != expected:
        raise SystemExit('unlisted or missing file in audited input bundle')
    return manifest


def verify_staged_payloads(release: Path, manifest: dict, *, prefixes=(), names=()) -> None:
    """Check copied original bytes before generating any new release hashes."""
    expected = {name: item for name, item in manifest['files'].items()
                if name in names or any(name.startswith(prefix + '/') for prefix in prefixes)}
    actual = set()
    for prefix in prefixes:
        directory = release/prefix
        if directory.is_symlink() or not directory.is_dir():
            raise SystemExit('staged audited payload missing: ' + prefix)
        for file in directory.rglob('*'):
            if file.is_symlink():
                raise SystemExit('symlink in staged audited payload')
            if file.is_file():
                actual.add(file.relative_to(release).as_posix())
    for name in names:
        file = release/name
        if file.is_symlink():
            raise SystemExit('symlink in staged audited payload')
        if file.is_file():
            actual.add(name)
    if actual != set(expected):
        raise SystemExit('unlisted or missing staged audited payload')
    for name, item in expected.items():
        file = release/name
        if file.stat().st_size != item['bytes'] or sha256(file) != item['sha256']:
            raise SystemExit('staged audited input mismatch: ' + name)


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, help='Audited image bundle with image-index.json; original project layout is the default')
    parser.add_argument('--images-manifest', choices=('image-index.json', '文件校验.json'), default='image-index.json',
                        help='Existing image index or complete installer file manifest, selected explicitly')
    parser.add_argument('--images-sha256', help='Exact SHA256 of the nominated image manifest')
    parser.add_argument('--baseline-package', type=Path, help='Immutable complete installer supplying USB tools and driver payloads')
    parser.add_argument('--baseline-sha256', help='Exact SHA256 of the baseline 文件校验.json')
    parser.add_argument('--version', help='Installer version; must match the audited enhanced-image metadata')
    parser.add_argument('--preflight-only', action='store_true', help='Check nominated inputs without compiling or creating release outputs')
    args = parser.parse_args(argv)
    for left, right in ((args.images, args.images_sha256), (args.baseline_package, args.baseline_sha256)):
        if bool(left) != bool(right):
            parser.error('Each nominated input directory requires its exact manifest SHA256')
    return args


def main(argv=None) -> None:
    args = arguments(argv)
    images = args.images.resolve() if args.images else ROOT
    payloads = args.baseline_package.resolve() if args.baseline_package else ROOT
    image_manifest = None
    baseline_manifest = None
    if args.images:
        image_manifest = verified_bundle(images, args.images_manifest, args.images_sha256,
            {flavor + '/root.squashfs' for flavor in ('official', 'official-ssh', 'official-hooked')} |
            {'official-ssh/firmware.json'})
    if args.baseline_package:
        baseline_manifest = verified_bundle(payloads, '文件校验.json', args.baseline_sha256,
            {'LX06Installer.exe', 'elevhelper.exe', 'flash-tool/bin/update.exe',
             'flash-tool/drivers/dpinst64.exe', 'flash-tool/drivers/dpinst32.exe',
             'flash-tool/drivers/amd64/libusb0.sys', 'flash-tool/drivers/x86/libusb0.sys',
             'tools/mksquashfs.exe', 'official/root.squashfs', 'official-ssh/root.squashfs'})
    metadata_file = images / 'official-ssh/firmware.json'
    metadata = json.loads(metadata_file.read_text(encoding='utf-8')) if metadata_file.exists() else {}
    seq = os.environ.get('VERSION_SEQ', '1')
    build_date = os.environ.get('BUILD_DATE') or datetime.now().strftime('%Y-%m-%d')
    version_date = build_date.replace('-', '')
    version = args.version or os.environ.get('RELEASE_VERSION') or metadata.get('version') or f'V{version_date}.{seq}'
    if not version or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-' for c in version):
        raise SystemExit('invalid release version')
    if metadata.get('version') and version != metadata['version']:
        raise SystemExit('installer version must match the audited firmware version')
    if args.preflight_only and not metadata_file.is_file():
        raise SystemExit('missing enhanced-image metadata')
    if image_manifest is not None:
        if image_manifest.get('version') != version or metadata.get('version') != version:
            raise SystemExit('image-index and enhanced metadata must match the selected version')
        if (metadata.get('feature') != 'mfcc-lifter32' or metadata.get('model_version') != 8 or
                sha256(images/'official-ssh/root.squashfs') != metadata.get('sha256')):
            raise SystemExit('audited enhanced-image identity mismatch')
    releases = ROOT / "build" / "releases"
    release = releases / f"LX06Installer-{version}"
    zip_path = releases / f"LX06Installer-{version}.zip"
    if release.exists() or zip_path.exists():
        raise SystemExit(f"release already exists; preserve it and select a new version: {version}")
    # Reject mismatched image metadata before building or replacing the dev EXE.
    if not args.images and not args.preflight_only:
        run([sys.executable, "scripts/sha256sums.py", str(ROOT)])
    companions = metadata.get("companion_scripts", {})
    companion_source = ROOT / "build" / f"用户脚本-{version}"
    if args.baseline_package and not companion_source.is_dir():
        companion_source = payloads / "用户脚本"
    notes = metadata.get("release_notes", "docs/rc8-人工验收.md")
    if metadata.get("release_notes") and not (ROOT / notes).is_file():
        raise SystemExit(f"missing release documentation: {notes}")
    if companions:
        if not metadata.get('script_variants'):
            verify_scripts(companion_source, companions)
        for label, identities in metadata.get('script_variants', {}).items():
            verify_scripts(companion_source / label, identities)
        for relative in ("docs/用户脚本使用说明.md", notes, "docs/dialog-light-api.md"):
            if not (ROOT / relative).is_file():
                raise SystemExit(f"missing release documentation: {relative}")

    # 0. CRLF 铁律门：要烘进设备的 shell 脚本必须纯 LF。CRLF 行尾的 `2>&1\r`
    #    会被 ash 解析成无效 fd → redir error → 脚本 rc=2 中断 → keeper
    #    descendant-abnormal-exit 误入安全模式（V20261019.1 .166 真机实证）。
    #    hooks/unified/** 是 dropbear 上游源码树（WSL 构建用），不在此列。
    bad = []
    for base_dir in ("hooks", "init-apps"):
        for f in sorted((ROOT / base_dir).rglob("*")):
            if not f.is_file() or f.suffix not in (".sh", ".init"):
                continue
            if base_dir == "hooks" and f.relative_to(ROOT).parts[1] == "unified":
                continue
            if b"\r" in f.read_bytes():
                bad.append(str(f.relative_to(ROOT)))
    if bad:
        sys.exit("CRLF 检测到（设备脚本必须纯 LF）: " + ", ".join(bad))

    if args.preflight_only:
        required = [payloads/'flash-tool/bin/update.exe', payloads/'tools/mksquashfs.exe']
        required += [images/flavor/'root.squashfs' for flavor in ('official', 'official-ssh', 'official-hooked')]
        if any(not file.is_file() or file.is_symlink() for file in required):
            raise SystemExit('missing release input payload')
        if (metadata.get('feature') != 'mfcc-lifter32' or metadata.get('model_version') != 8 or
                sha256(images/'official-ssh/root.squashfs') != metadata.get('sha256')):
            raise SystemExit('enhanced-image identity mismatch')
        print(json.dumps(dict(version=version, images=str(images), baseline_package=str(payloads),
            image_manifest=args.images_manifest if args.images else None,
            image_manifest_sha256=args.images_sha256, baseline_manifest_sha256=args.baseline_sha256,
            compiler_started=False, release_created=False), ensure_ascii=False))
        return

    # 1. icon
    run([sys.executable, "scripts/build_icon.py"])
    # 2. rsrc -> syso
    run([find_rsrc(), "-arch", "amd64", "-ico",
         "assets/icon/lx06-app.ico", "-o", "resource_windows_amd64.syso"])
    go = find_go()
    # 3. vet + build
    run([go, "vet", "./..."])
    build_exe = ROOT / "build" / "LX06Installer.exe"
    build_exe.parent.mkdir(parents=True, exist_ok=True)
    run([go, "build", "-trimpath",
         f"-ldflags=-s -w -H=windowsgui -X main.version={version} -X main.buildDate={build_date}",
         "-o", str(build_exe), "."])
    run([go, "build", "-trimpath", "-ldflags=-s -w -H=windowsgui",
         "-o", str(ROOT / "build" / "elevhelper.exe"), "./elevhelper"])

    # 4. stage green release (flat)
    releases.mkdir(parents=True, exist_ok=True)
    # Older packages are rollback artifacts, not disposable build scratch.
    release.mkdir(parents=True)
    shutil.copy2(build_exe, release / "LX06Installer.exe")
    shutil.copy2(ROOT / "build" / "elevhelper.exe", release / "elevhelper.exe")
    for name in ('README.txt', 'DATA说明.md'):
        source = ROOT/name if (ROOT/name).is_file() else payloads/name
        shutil.copy2(source, release/name)
    for d in ("flash-tool", "official", "official-ssh", "official-hooked"):
        # 历史构建备份（root.squashfs.fix*/.bak 等）不进发布包
        shutil.copytree((payloads if d == "flash-tool" else images) / d, release / d,
                        ignore=shutil.ignore_patterns("root.squashfs.*", "*.bak"))
    init_dst = release / "init-apps"
    if metadata.get("include_init_apps", True):
        init_source = ROOT/"init-apps" if (ROOT/"init-apps").is_dir() else payloads/"init-apps"
        shutil.copytree(init_source, init_dst)
    else:
        # The wake candidate is a clean system. Keep development probes in the
        # source tree, without silently installing them on the user's speaker.
        init_dst.mkdir()
        (init_dst / "README.txt").write_text(
            ("需要使用配套脚本时，请在 Web88 上传用户脚本目录中的文件。\n" if companions else
             "需要连接第三方服务时，请解压单独提供的脚本包，在 Web88 上传所需脚本。\n"),
            encoding="utf-8")
    if companions:
        companion_dst = release / "用户脚本"
        companion_dst.mkdir()
        if not metadata.get('script_variants'):
            for name in companions:
                shutil.copy2(companion_source / name, companion_dst / name)
        for label, identities in metadata.get('script_variants', {}).items():
            folder = companion_dst / label
            folder.mkdir()
            for name in identities:
                shutil.copy2(companion_source / label / name, folder / name)
            verify_scripts(folder, identities)
        for optional in ('door_open.sh', '玻璃门使用说明.md'):
            if (companion_source / optional).is_file():
                shutil.copy2(companion_source / optional, companion_dst / optional)
        shutil.copy2(ROOT / "docs/用户脚本使用说明.md", companion_dst / "使用说明.md")
        (release / "docs").mkdir()
        shutil.copy2(ROOT / "docs/dialog-light-api.md", release / "docs/dialog-light-api.md")
        if metadata.get("official_command_router"):
            shutil.copy2(ROOT / "docs/official-command-router.md", release / "docs/official-command-router.md")
        if not metadata.get('script_variants'):
            verify_scripts(companion_dst, companions)
    # Audited acceptance notes belong to the installer even when it ships no
    # personal companion scripts. Keep them before the manifest/ZIP writers.
    if companions or metadata.get("release_notes"):
        shutil.copy2(ROOT / notes, release / "人工验收.md")
    # dev 布局：build/<pkg> 是 exe 实际读取的目录，同步固件镜像与元数据
    # （version/SOURCE/SHA256），否则固件卡不显示版本号、preflight 哈希不匹配。
    for d in ("official", "official-ssh", "official-hooked"):
        dev_pkg = ROOT / "build" / d
        dev_pkg.mkdir(parents=True, exist_ok=True)
        for mf in ("root.squashfs", "version.txt", "SOURCE.txt", "SHA256SUMS.txt", "firmware.json"):
            src = images / d / mf
            if src.exists():
                shutil.copy2(src, dev_pkg / mf)
    # dev 布局：init-apps/ 也是 exe 直接读取的目录（烘焙 /lx06-init.d/ 的源），
    # 必须整目录同步（先删后拷），否则旧副本（如 CRLF 版脚本）混进刷机
    # （V20261019.2 教训：build/init-apps 残留 09:44 CRLF probe）。
    dev_init = ROOT / "build" / "init-apps"
    if dev_init.exists():
        if dev_init.resolve().parent != (ROOT / "build").resolve() or dev_init.is_symlink():
            raise SystemExit("refusing to replace init-apps outside build directory")
        shutil.rmtree(dev_init)
    shutil.copytree(init_dst, dev_init)
    # tools/: only the static mksquashfs (pre-flash SSH bake); unsquashfs is
    # a dev tool and hits symlink-permission walls in plain user sessions.
    tools_dst = release / "tools"
    tools_dst.mkdir(parents=True)
    shutil.copy2(payloads / "tools" / "mksquashfs.exe", tools_dst / "mksquashfs.exe")
    # dev 布局：build/LX06Installer.exe 直接从 build/ 跑，同样需要 tools/。
    dev_tools = ROOT / "build" / "tools"
    dev_tools.mkdir(parents=True, exist_ok=True)
    shutil.copy2(payloads / "tools" / "mksquashfs.exe", dev_tools / "mksquashfs.exe")

    # Compilation can take minutes. Validate the actual copied bytes against the
    # original pinned inputs before sha256sums/manifest can relabel replacements.
    # Newly built EXEs and current README/docs intentionally keep their new bytes.
    if image_manifest is not None:
        verify_staged_payloads(release, image_manifest,
            prefixes=('official', 'official-ssh', 'official-hooked'))
    if baseline_manifest is not None:
        verify_staged_payloads(release, baseline_manifest,
            prefixes=('flash-tool',), names=('tools/mksquashfs.exe',))

    # 5. SHA256SUMS.txt — regenerate from the actual files in EVERY location,
    #    not just the release dir. The source package dirs and the dev layout
    #    (build/<pkg>, which is what the dev exe reads) must each hash the
    #    image that is physically present there, otherwise a rebuilt image
    #    leaves a stale manifest and preflight's SHA256 check fails.
    manifest_roots = (release, ROOT / "build") if args.images else (ROOT, release, ROOT / "build")
    for base in manifest_roots:
        run([sys.executable, "scripts/sha256sums.py", str(base)])

    # The handoff belongs to the compiled package, before manifest and ZIP.
    run([sys.executable, "scripts/write_github_handoff.py", str(release)])

    # Cover optional scripts and documentation as well as firmware images.
    manifest = {p.relative_to(release).as_posix(): {"sha256": sha256(p), "bytes": p.stat().st_size}
                for p in sorted(release.rglob("*")) if p.is_file()}
    (release / "文件校验.json").write_text(json.dumps(
        {"version": version, "acceptance_complete": metadata.get("acceptance_complete", False),
         "files": manifest}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    # 6. zip (contents at zip root, LX04 style)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(release.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(release))

    print("BUILD_OK", release / "LX06Installer.exe")
    print("RELEASE_ZIP", zip_path)


if __name__ == "__main__":
    main()

"""Package a new local installer from an immutable audited release baseline.

Firmware work is intentionally limited to audited helper replacements in the
SSH image. The original image and all previous release directories stay intact.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inventory(root: Path) -> dict[str, str]:
    return {p.relative_to(root).as_posix(): sha(p) for p in root.rglob('*')
            if p.is_file() and not p.is_symlink()}


def run(*args: str) -> None:
    subprocess.run(args, check=True)


def archive(folder: Path) -> dict:
    manifest = {'version': json.loads((folder/'official-ssh/firmware.json').read_text('utf8'))['version'],
                'acceptance_complete': False,
                'files': {p.relative_to(folder).as_posix(): {'sha256': sha(p), 'bytes': p.stat().st_size}
                          for p in sorted(folder.rglob('*')) if p.is_file() and p.name != '文件校验.json'}}
    (folder/'文件校验.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n', 'utf8')
    target = folder.parent/(folder.name+'.zip')
    assert not target.exists(), f'preserve existing archive: {target}'
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in [*manifest['files'], '文件校验.json']:
            z.write(folder/name, name)
    with zipfile.ZipFile(target) as z:
        assert z.testzip() is None
        assert set(z.namelist()) == set(manifest['files']) | {'文件校验.json'}
        for name, item in manifest['files'].items():
            with z.open(name) as stream:
                assert hashlib.file_digest(stream, 'sha256').hexdigest() == item['sha256'], name
    return {'name': target.name, 'sha256': sha(target), 'bytes': target.stat().st_size,
            'members_verified': len(manifest['files'])+1, 'user_scripts_included': True}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', required=True)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--helpers', type=Path, required=True)
    parser.add_argument('--notes', type=Path, required=True)
    parser.add_argument('--user-scripts', type=Path, required=True)
    args = parser.parse_args()
    version = args.version
    assert version.startswith('V') and all(c.isalnum() or c in '.-' for c in version)
    release = ROOT/'build/releases'/('LX06Installer-'+version)
    assert not release.exists() and not release.with_suffix('.zip').exists(), 'never overwrite releases'
    shutil.copytree(args.base, release, ignore=shutil.ignore_patterns('文件校验.json'))
    shutil.copy2(args.installer, release/'LX06Installer.exe')
    (release/'人工验收.md').write_bytes(args.notes.read_bytes())
    (release/'docs/rc1-人工验收.md').write_bytes(args.notes.read_bytes())
    scripts = release/'用户脚本'
    scripts.mkdir(exist_ok=True)
    for name in ('chatgpt_account.sh', 'grok_account.sh', 'deepseek_account.sh'):
        shutil.copy2(args.user_scripts/name, scripts/name)
    (scripts/'使用说明.md').write_text(
        '脚本通过 Web88 按需上传。ChatGPT、Grok、DeepSeek 的登录凭据保存在音箱本机；模型列表由账户目录更新。\n'
        '私人连接、密钥和门控配置不得进入公开仓库。官方小爱、豆包和小智脚本按需使用。\n', 'utf8')

    ssh = release/'official-ssh'
    base_meta = json.loads((ssh/'firmware.json').read_text('utf8'))
    base_image_sha = sha(ssh/'root.squashfs')
    assert base_meta['sha256'] == base_image_sha
    with tempfile.TemporaryDirectory(prefix='lx06-rc1-image-') as td:
        temp = Path(td)
        tree = temp/'tree'
        verified = temp/'verified'
        before_tree = temp/'before'
        run('unsquashfs', '-q', '-d', str(before_tree), str(ssh/'root.squashfs'))
        before = inventory(before_tree)
        run('unsquashfs', '-q', '-d', str(tree), str(ssh/'root.squashfs'))
        shutil.copy2(args.helpers/'lx06-model-tool', tree/'usr/bin/lx06-model-tool')
        shutil.copy2(args.helpers/'lx06-model-voice', tree/'usr/bin/lx06-model-voice')
        (tree/'usr/bin/lx06-model-tool').chmod(0o755)
        (tree/'usr/bin/lx06-model-voice').chmod(0o755)
        image_new = ssh/'root.squashfs.new'
        run('mksquashfs', str(tree), str(image_new), '-noappend', '-comp', 'xz', '-b', '131072',
            '-tailends', '-no-xattrs', '-no-exports', '-all-root', '-processors', '2', '-no-progress')
        run('unsquashfs', '-q', '-d', str(verified), str(image_new))
        after = inventory(verified)
        changed = {name: digest for name, digest in after.items() if before.get(name) != digest}
        assert set(changed) == {'usr/bin/lx06-model-tool', 'usr/bin/lx06-model-voice'}, changed.keys()
        assert not set(before)-set(after)
        for name in ('usr/bin/lx06-model-tool', 'usr/bin/lx06-model-voice'):
            assert (verified/name).stat().st_mode & 0o777 == 0o755
        shutil.copyfile(image_new, ssh/'root.squashfs')

    meta = base_meta
    meta.update(version=version, sha256=sha(ssh/'root.squashfs'), acceptance_complete=False,
                release_notes='docs/rc1-人工验收.md', model_catalog_merge=1, provider_error_notices=1)
    meta['account_scripts'] = {name: {'sha256': sha(scripts/name), 'helpers': 'ROM'}
                               for name in ('chatgpt_account.sh', 'grok_account.sh', 'deepseek_account.sh')}
    (ssh/'firmware.json').write_text(json.dumps(meta, ensure_ascii=False, indent=2)+'\n', 'utf8')
    (ssh/'version.txt').write_text(version+'\n', 'utf8')
    (ssh/'SOURCE.txt').write_text('Vendor LX06 1.94.13; audited rc18 baseline; see image-audit.json.\n', 'utf8')
    (ssh/'SHA256SUMS.txt').write_text(f"{meta['sha256']}  { (ssh/'root.squashfs').stat().st_size }  root.squashfs\n", 'utf8')
    audit = {'version': version, 'flavor': 'official-ssh', 'base_version': 'V20261030.1-rc18',
             'base_sha256': base_image_sha, 'sha256': meta['sha256'], 'bytes': (ssh/'root.squashfs').stat().st_size,
             'changed_files': changed, 'added_files': [], 'other_original_files_identical': True,
             'flashed': False, 'acceptance_complete': False}
    (ssh/'image-audit.json').write_text(json.dumps(audit, indent=2)+'\n', 'utf8')

    for flavor in ('official', 'official-hooked'):
        folder = release/flavor
        m = json.loads((folder/'firmware.json').read_text('utf8'))
        m.update(version=version, acceptance_complete=False, release_notes='docs/rc1-人工验收.md')
        (folder/'firmware.json').write_text(json.dumps(m, ensure_ascii=False, indent=2)+'\n', 'utf8')
        (folder/'version.txt').write_text(version+'\n', 'utf8')
    (release/'README.txt').write_text(
        f'LX06 {version} 个人完整版\n\n解压所有文件后运行 LX06Installer.exe。保留全部驱动、工具及镜像目录。\n'
        '用户脚本由 Web88 按需上传，不随刷机自动安装。\n'
        '增强固件更新通用模型目录合并和服务器错误语音提示；官方固件镜像保持原样。\n'
        '这是人工验收候选版，未在本版重新刷机；请先保留 DATA 并手动验收。\n', 'utf8')
    (release/'docs/rc1-development-handoff.md').write_text(
        f'# {version} 开发交接\n\n'
        '- 基于不可变 rc18 完整包；旧包、旧标签和历史资产均保留。\n'
        '- 官方 root.squashfs 与 rc18 字节一致；增强镜像只更新 model-tool 和 model-voice。\n'
        '- 新增多模型目录形状合并、ID 去重，以及有界服务器错误通知；不把失败请求写入对话历史。\n'
        '- 用户脚本与账号数据留在本地包，不进入 GitHub ZIP。\n'
        '- 真人/噪音验收和本版本刷机未完成，acceptance_complete=false。\n', 'utf8')
    (release/'GITHUB提交说明.md').write_text(
        '# GitHub 提交与发布交接 - '+version+'\n\n'
        '公开包仅使用 export_github.py 导出的脱敏源码、完整安装器和固件 ZIP。个人完整版、用户脚本、DeepSeek/Grok/ChatGPT 登录信息及门控配置不得上传。\n\n'
        '源码更新包括模型列表多响应格式支持、DeepSeek 有界错误通知、对应 C helper 与测试，以及中英文说明和 release 清单。每版脱敏脚本源码提交到 kws/lx06-installer/scripts/examples/user-scripts/{version}/；API Key、小智私人地址/令牌和门控资料仅保留占位符。公开固件更新仅涉及 official-ssh 镜像内的 model-tool 与 model-voice；official 镜像保持 rc18 字节不变。\n\n'
        '提交前运行公开包审计、校验 source-manifest 与每个 ZIP 成员；提交源码后创建新版本 Release，上传完整安装器 ZIP、固件 ZIP、SHA256SUMS 和 Release notes。不得覆盖旧标签或资产。\n', 'utf8')
    subprocess.run(['python3', 'scripts/sha256sums.py', str(release)], cwd=ROOT, check=True)
    installer_result = archive(release)
    firmware = release.parent/('LX06Firmware-'+version)
    firmware.mkdir()
    for name in ('official', 'official-ssh'):
        shutil.copytree(release/name, firmware/name)
    shutil.copytree(scripts, firmware/'用户脚本')
    shutil.copytree(release/'docs', firmware/'docs')
    for name in ('人工验收.md', 'GITHUB提交说明.md'):
        shutil.copy2(release/name, firmware/name)
    firmware_result = archive(firmware)
    report = {'version': version, 'installer': installer_result, 'firmware': firmware_result,
              'flashed': False, 'acceptance_complete': False}
    (ROOT/'build'/('package-'+version+'-verification.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', 'utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()

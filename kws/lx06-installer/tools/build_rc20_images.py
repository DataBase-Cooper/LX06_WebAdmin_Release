"""rc20 Web extension parser + identity fix; WSL Python and squashfs-tools.

Only enhanced webd/settings/UI and two version/manifest files may change. ARM webd
must parse companion declarations under QEMU. All other nodes stay unchanged.
No USB operations, speaker connection, DATA payload or GitHub operations.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = 'V20261031.1-rc20'
BASE = ROOT/'build/releases/LX06Installer-V20261031.1-rc19'
OUT = ROOT/'build'/('firmware-'+VERSION)
BIN = ROOT/'build/rc20-components/lx06webd'
SETTINGS = BIN.parent/'script-settings'
REPLACEMENTS = {'usr/sbin/lx06webd': BIN, 'usr/lib/lx06/script-settings': SETTINGS,
                'usr/lib/lx06/web/index.html': ROOT/'hooks/webd/index.html'}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+'\n', encoding='utf8')


def run(*args):
    subprocess.run([str(a) for a in args], check=True, stdout=subprocess.DEVNULL)


def inventory(tree):
    result = {}
    for p in tree.rglob('*'):
        st = p.lstat()
        row = {'mode': st.st_mode, 'uid': st.st_uid, 'gid': st.st_gid}
        if stat.S_ISLNK(st.st_mode):
            row['link'] = str(p.readlink())
        elif stat.S_ISREG(st.st_mode):
            row['sha256'] = sha(p)
        elif stat.S_ISCHR(st.st_mode) or stat.S_ISBLK(st.st_mode):
            row['rdev'] = st.st_rdev
        result[p.relative_to(tree).as_posix()] = row
    return result


def main():
    assert not OUT.exists(), 'never overwrite an earlier image'
    BIN.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['bash', str(ROOT/'scripts/build_webd_standalone.sh'), str(BIN)],
                   cwd=ROOT, check=True, env={**os.environ, 'LX06_LIB': str(ROOT/'build/model-adapter-v1/abi'), 'LX06_FW_VERSION': VERSION})
    subprocess.run(['bash', str(ROOT/'scripts/build_script_settings.sh'), str(SETTINGS)],
                   cwd=ROOT, check=True, env={**os.environ, 'LX06_LIB': str(ROOT/'build/model-adapter-v1/abi')})
    inputs = json.loads((BASE/'文件校验.json').read_text(encoding='utf8'))['files']
    for flavor in ('official', 'official-ssh', 'official-hooked'):
        assert sha(BASE/flavor/'root.squashfs') == inputs[flavor+'/root.squashfs']['sha256']
    OUT.mkdir()
    for flavor in ('official', 'official-ssh', 'official-hooked'):
        source = BASE/flavor
        dest = OUT/flavor
        dest.mkdir()
        meta = json.loads((source/'firmware.json').read_text(encoding='utf8'))
        base = source/'root.squashfs'
        assert sha(base) == meta['sha256']
        image = dest/'root.squashfs'
        changed = {}
        old_internal_version = None
        if flavor != 'official-ssh':
            shutil.copyfile(base, image)
        else:
            with tempfile.TemporaryDirectory(prefix='lx06-rc20-') as td:
                tree = Path(td)/'root'
                run('unsquashfs', '-q', '-d', tree, base)
                before = inventory(tree)
                for name, path in REPLACEMENTS.items():
                    shutil.copyfile(path, tree/name)
                    (tree/name).chmod(0o644 if name.endswith('.html') else 0o755)
                marker = tree/'lx06-firmware-version'
                old_internal_version = marker.read_text().strip()
                marker.write_text(VERSION+'\n')
                wake_path = tree/'usr/lib/lx06/wake-stack.json'
                wake = json.loads(wake_path.read_text())
                wake['version'] = VERSION
                for name, path in REPLACEMENTS.items():
                    wake['files'][name] = sha(path)
                write_json(wake_path, wake)
                after = inventory(tree)
                assert set(before) == set(after), 'no nodes may be added or removed'
                names = {n for n in after if after[n] != before[n]}
                assert names == set(REPLACEMENTS)|{'lx06-firmware-version', 'usr/lib/lx06/wake-stack.json'}, names
                changed = {n: after[n]['sha256'] for n in sorted(names)}
                assert sha(tree/'usr/lib/lx06/wakeword/wakeword_template') == meta['mfcc_binary_sha256']
                run('mksquashfs', tree, image, '-noappend', '-comp', 'xz', '-b', '131072',
                    '-tailends', '-no-xattrs', '-no-exports', '-all-root', '-processors', '2', '-no-progress')
                verified = Path(td)/'verified'
                run('unsquashfs', '-q', '-d', verified, image)
                assert inventory(verified) == after, 'packed image differs from audited tree'
                assert (verified/'lx06-firmware-version').read_text().strip() == VERSION
                assert json.loads((verified/'usr/lib/lx06/wake-stack.json').read_text())['version'] == VERSION
                # Exercise the actual ARM binary shipped in this exact image,
                # not just a host-compiled copy of the source header.
                checks = []
                def check_script(p, label):
                    expected = b'# LX06-EXTENSION-BEGIN' in p.read_bytes()[:16384]
                    r = subprocess.run(['qemu-arm', '-L', str(verified), str(verified/'usr/sbin/lx06webd'),
                                        '--check-extension', str(p)], capture_output=True, text=True, timeout=20)
                    result = json.loads(r.stdout)
                    assert result['ok'] == expected and r.returncode == (0 if expected else 1), label
                    if expected:
                        # Read defaults from a header-only copy. This never
                        # executes the upload script, logs in or writes settings.
                        data = p.read_bytes()
                        end = data.index(b'# LX06-EXTENSION-END') + len(b'# LX06-EXTENSION-END')
                        sample = Path(td)/('settings-'+p.name)
                        sample.write_bytes(data[:end]+b'\nexit 0\n'); sample.chmod(0o600)
                        env = {k:v for k,v in os.environ.items() if not k.startswith('LX06_EXTENSION_')}
                        read = subprocess.run(['qemu-arm', '-L', str(verified), str(verified/'usr/lib/lx06/script-settings'),
                                               'show', str(sample)], capture_output=True, text=True, timeout=20, env=env)
                        assert read.returncode == 0 and 'values' in json.loads(read.stdout), label+' settings'
                        result['settings_read'] = True
                    checks.append({'script': label, **result})
                for p in sorted((BASE/'用户脚本').glob('*.sh')):
                    check_script(p, p.name)
                for p in sorted((ROOT/'scripts/examples/user-scripts'/VERSION).glob('*.sh')):
                    check_script(p, 'sanitized/'+p.name)
                old_zip = ROOT/'build/releases/LX06Installer-V20261030.1-rc18.zip'
                with zipfile.ZipFile(old_zip) as z:
                    for name in ('grok_account.sh', 'chatgpt_account.sh'):
                        p = Path(td)/('rc18-'+name)
                        p.write_bytes(z.read('用户脚本/'+name))
                        check_script(p, 'rc18/'+name)
                write_json(OUT/'extension-verification.json', {'version': VERSION, 'actual_packed_arm_binary': True, 'checks': checks})
        assert image.stat().st_size <= 41943040
        audit = {'version': VERSION, 'flavor': flavor, 'base_version': 'V20261031.1-rc19',
                 'base_sha256': sha(base), 'sha256': sha(image), 'bytes': image.stat().st_size,
                 'changed_files': changed, 'added_files': [], 'removed_files': [],
                 'other_original_files_identical': True, 'file_modes_nodes_links_verified': flavor == 'official-ssh',
                 'base_internal_web_version': old_internal_version,
                 'flashed': False, 'acceptance_complete': False}
        write_json(dest/'image-audit.json', audit)
        meta.update(version=VERSION, sha256=sha(image), release_notes='docs/rc20-data-wipe-version.md', acceptance_complete=False)
        if flavor == 'official-ssh':
            meta['internal_web_version'] = VERSION
        write_json(dest/'firmware.json', meta)
        (dest/'version.txt').write_text(VERSION+'\n')
        (dest/'SHA256SUMS.txt').write_text(f'{sha(image)}  {image.stat().st_size}  root.squashfs\n')
        (dest/'SOURCE.txt').write_text('rc20: verified rc19 payload; enhanced image updates extension webd/settings/UI and two version/manifest files; see image-audit.json.\n')
        print(json.dumps(audit), flush=True)


if __name__ == '__main__':
    main()

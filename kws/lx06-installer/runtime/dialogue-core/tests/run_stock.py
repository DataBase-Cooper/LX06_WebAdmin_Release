"""Boot a production ARM core only inside a temporary stock-image chroot.

The production executable has no host-fixture bypass. Protocol discovery and
missing-lease refusals run under QEMU in private mount/network namespaces. No
speaker, live microphone, SDK, account, DATA, firmware or Internet is touched.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], capture_output=True, text=True,
                          timeout=kwargs.pop('timeout', 20), **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-name', required=True)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--arm-output', type=Path)
    parser.add_argument('--image', type=Path, default=ROOT / 'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs')
    args = parser.parse_args()
    if os.geteuid():
        raise RuntimeError('temporary proc/null namespace fixtures require root')
    output = ROOT / 'build' / args.output_name; output.mkdir(exist_ok=False)
    report = dict(passed=False, actual_device=False, actual_audio=False, cloud_requests=0,
                  production_fixture_bypass=False, private_mount_network_namespace=True,
                  source_sha256=sha(__file__), cases=[])
    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    try:
        if args.binary:
            if args.arm_output:
                raise ValueError('--binary and --arm-output are mutually exclusive')
            binary = args.binary.resolve()
        else:
            arm_output = (args.arm_output or ROOT / 'build' / (args.output_name + '-core')).resolve()
            completed = run(['/usr/bin/python3', ROOT / 'runtime/dialogue-core/build.py',
                             '--arm', '--output', arm_output], timeout=120)
            if completed.returncode:
                raise RuntimeError(completed.stderr[-3000:])
            binary = arm_output / 'lx06-dialogued'
        manifest_path = binary.parent / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        assert manifest['passed'] and manifest['arm'] and not manifest['fixture'] and not manifest['sanitized']
        assert sha(binary) == manifest['sha256']
        assert b'DC_HOST_FIXTURE' not in binary.read_bytes(), 'production contains a test ownership permit'
        image = args.image.resolve(); assert image.is_file()
        symbols = run(['arm-linux-gnueabihf-readelf', '--dyn-syms', '-W', binary])
        assert symbols.returncode == 0
        (output / 'dynsyms.txt').write_text(symbols.stdout)
        versions = [(int(a), int(b)) for a, b in re.findall(r'@GLIBC_(\d+)\.(\d+)', symbols.stdout)]
        assert versions and all(v <= (2, 25) for v in versions), 'requires a newer glibc'
        header = run(['arm-linux-gnueabihf-readelf', '-h', binary]); assert header.returncode == 0
        assert 'ARM' in header.stdout and 'hard-float ABI' in header.stdout
        (output / 'elf-header.txt').write_text(header.stdout)
        report.update(binary=str(binary), binary_sha256=sha(binary), binary_bytes=binary.stat().st_size,
                      binary_manifest_sha256=sha(manifest_path), image_sha256=sha(image),
                      max_glibc='.'.join(map(str, max(versions))))
        save()
        with tempfile.TemporaryDirectory(prefix='dc-core-stock-') as temporary:
            stock = Path(temporary) / 'stock'
            unpacked = run(['unsquashfs', '-q', '-d', stock, image], timeout=60)
            if unpacked.returncode:
                raise RuntimeError(unpacked.stderr[-2000:])
            for folder in ('dev', 'proc', 'tmp'):
                (stock / folder).mkdir(exist_ok=True)
            # The backing filesystem can be nodev: bind the real harmless null
            # only in a private mount namespace, never mknod/open a real speaker.
            (stock / 'dev/null').touch(exist_ok=True)
            shutil.copyfile(shutil.which('qemu-arm-static'), stock / 'qemu-arm-static')
            (stock / 'qemu-arm-static').chmod(0o755)
            shutil.copyfile(binary, stock / 'tmp/lx06-dialogued'); (stock / 'tmp/lx06-dialogued').chmod(0o755)
            script = ('dc_stock="$1"; shift; mount --bind /dev/null "$dc_stock/dev/null" && '
                      'mount -t proc proc "$dc_stock/proc" && '
                      'exec chroot "$dc_stock" /qemu-arm-static /tmp/lx06-dialogued "$@"')
            cases = [(['--protocol'], 0), (['--account', '/tmp/synthetic.sh', 'selected'], 77),
                     (['--audio', 'xiaozhi'], 77), (['--audio', 'doubao'], 77),
                     (['--audio', 'unknown'], 2), (['--bad-mode'], 2), ([], 2)]
            for arguments, expected in cases:
                completed = run(['unshare', '--mount', '--net', 'sh', '-c', script,
                                 'dc-core-stock', stock, *arguments],
                                env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin'}, timeout=10)
                assert completed.returncode == expected, (arguments, completed.returncode, completed.stderr[-1000:])
                if arguments == ['--protocol']:
                    assert completed.stdout.strip() == 'LX06DIALOGUE/1' and not completed.stderr
                elif expected == 77:
                    assert not completed.stdout
                    assert re.fullmatch(r'dialogue-core: event=ownership_refused value=0 at=\d+\n', completed.stderr)
                else:
                    assert not completed.stdout and not completed.stderr
                report['cases'].append(dict(arguments=arguments, expected=expected,
                                             exit_code=completed.returncode, passed=True))
                save()
        report['source_unchanged'] = sha(__file__) == report['source_sha256']
        report['binary_sources_current'] = all(sha(ROOT / name) == value for name, value in manifest['sources'].items())
        report['passed'] = report['source_unchanged'] and report['binary_sources_current']; save()
    except BaseException as error:
        report['failure'] = dict(type=type(error).__name__, message=str(error)[-3000:]); save(); raise
    print(json.dumps(dict(passed=report['passed'], cases=len(report['cases']),
                          report_sha256=sha(output / 'report.json'))))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

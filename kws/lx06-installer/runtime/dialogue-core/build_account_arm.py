"""Build/test account pure boundaries against stock ARM32 glibc 2.25 offline.

Host test_account.py separately exercises actual script processes, cancellation
and generated provider adapters. This QEMU fixture does not measure target CPU,
launch scripts, call the official SDK or establish acoustic acceptance.
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
ROOT = HERE.parent.parent
IMAGE = ROOT / 'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def call(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, capture_output=True, text=True, **kwargs)


def main(output, kind='account'):
    assert os.geteuid() == 0, 'local stock-library chroot needs build-host root'
    output = Path(output).resolve()
    assert output.parent == (ROOT / 'build').resolve() and not output.exists()
    assert digest(IMAGE) == json.loads((IMAGE.parent / 'firmware.json').read_text())['sha256']
    output.mkdir()
    report = dict(passed=False, actual_device=False, actual_audio=False, provider_requests=0,
                  stock_glibc_225=True, image_sha256=digest(IMAGE),
                  scope='offline stock ARM ' + kind + ' pure boundaries; process runner tested on host')
    paths = [HERE / x for x in (kind + '.c', kind + '.h', kind + '_fixture.c', 'types.h')]
    if kind == 'tts':
        paths.append(HERE.parent / 'model-adapter/experiments/native-tts-ipc.h')
    before = {p.name: digest(p) for p in paths}
    try:
        with tempfile.TemporaryDirectory(prefix='dc-account-arm-') as temporary:
            temp = Path(temporary)
            tree = temp / 'root'
            call(['unsquashfs', '-q', '-d', tree, IMAGE], timeout=180)
            flags = ['-std=c11', '-O2', '-fPIE', '-Wall', '-Wextra', '-Werror',
                     '-fstack-usage', '-D_TIME_BITS=32', '-D_FILE_OFFSET_BITS=32']
            objects = output / 'objects'
            objects.mkdir()
            call(['arm-linux-gnueabihf-gcc', *flags, '-c', HERE / (kind + '_fixture.c'), '-o', objects / 'fixture.o'])
            crt = Path('/usr/arm-linux-gnueabihf/lib')
            gcc = Path(call(['arm-linux-gnueabihf-gcc', '-print-file-name=crtbeginS.o']).stdout.strip()).parent
            binary = output / (kind + '-boundary-arm')
            call(['arm-linux-gnueabihf-gcc', '-nostdlib', '-pie',
                  '-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3', '-Wl,-z,now,-z,relro',
                  '-Wl,--unresolved-symbols=ignore-in-shared-libs',
                  crt / 'Scrt1.o', crt / 'crti.o', gcc / 'crtbeginS.o', objects / 'fixture.o',
                  tree / 'lib/libc.so.6', tree / 'lib/libgcc_s.so.1', '-lgcc',
                  gcc / 'crtendS.o', crt / 'crtn.o', '-o', binary])
            symbols = call(['arm-linux-gnueabihf-readelf', '--dyn-syms', '-W', binary]).stdout
            assert not re.search(r'@GLIBC_2\.(?:2[6-9]|[3-9][0-9])\b', symbols)
            (output / 'dynsyms.txt').write_text(symbols)
            qemu = tree / 'qemu-arm-static'
            shutil.copyfile(shutil.which('qemu-arm-static'), qemu)
            qemu.chmod(0o755)
            (tree / 'tmp').mkdir(exist_ok=True)
            target = tree / ('tmp/' + kind + '-boundary')
            shutil.copyfile(binary, target)
            target.chmod(0o755)
            trace = output / 'network.strace'
            result = call(['unshare', '--net', 'strace', '-f', '-qq', '-e', 'trace=network', '-o', trace,
                           'chroot', tree, '/qemu-arm-static', '/tmp/' + kind + '-boundary'], timeout=20,
                          env=dict(PATH='/usr/sbin:/usr/bin:/sbin:/bin', LANG='C'))
            fixture = json.loads(result.stdout)
            assert fixture['passed']
            assert not re.search(r'\bAF_INET6?\b', trace.read_text())
            stacks = []
            for path in objects.glob('*.su'):
                for line in path.read_text().splitlines():
                    function, amount, kind = line.split('\t')
                    stacks.append(dict(function=function, bytes=int(amount), kind=kind))
            (output / 'stack-usage.json').write_text(json.dumps(stacks, indent=2) + '\n')
            report.update(passed=True, fixture=fixture, binary_bytes=binary.stat().st_size,
                          binary_sha256=digest(binary), internet_socket_syscalls=0,
                          max_stack_frame_bytes=max(x['bytes'] for x in stacks))
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        report['sources'] = before
        report['sources_unchanged'] = all(digest(p) == before[p.name] for p in paths)
        report['passed'] = report['passed'] and report['sources_unchanged']
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--kind', choices=['account', 'tts'], default='account')
    args = parser.parse_args()
    main(args.output, args.kind)

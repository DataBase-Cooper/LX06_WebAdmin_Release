"""Local connection-cue generator, host/sanitizer/stock ARM; no audio hardware."""
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
    result = subprocess.run([str(x) for x in command], capture_output=True, timeout=20, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr[-2000:].decode(errors='replace'))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-name', required=True)
    parser.add_argument('--arm', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'build/rc24-native-tts-offline' / args.output_name; output.mkdir(exist_ok=False)
    source = [HERE.parent / 'media.c', HERE / 'test_media.c']
    evidence = source + [HERE.parent / 'media.h', HERE.parent / 'types.h', Path(__file__)]
    report = dict(passed=False, actual_device=False, actual_audio=False, cloud_requests=0,
                  sources={str(path.relative_to(ROOT)): sha(path) for path in evidence})
    def save():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    try:
        flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
        host = output / 'media-fixture'
        run(['gcc', *flags, *source, '-lm', '-o', host])
        report['host'] = json.loads(run(['unshare', '--net', host])); save()
        sanitized = output / 'media-fixture-sanitized'
        run(['gcc', *flags, '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             *source, '-lm', '-o', sanitized])
        report['sanitized'] = json.loads(run(['unshare', '--net', sanitized],
                                             env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'))); save()
        if args.arm:
            image = ROOT / 'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'
            with tempfile.TemporaryDirectory(prefix='dc-media-stock-') as temporary:
                temp = Path(temporary); stock = temp / 'stock'
                run(['unsquashfs', '-q', '-d', stock, image])
                objects = []
                for index, path in enumerate(source):
                    obj = temp / (str(index) + '.o')
                    run(['arm-linux-gnueabihf-gcc', *flags, '-D_TIME_BITS=32', '-D_FILE_OFFSET_BITS=32',
                         '-fPIE', '-c', path, '-o', obj]); objects.append(obj)
                crt = Path('/usr/arm-linux-gnueabihf/lib')
                gcc = Path(run(['arm-linux-gnueabihf-gcc', '-print-file-name=crtbeginS.o']).decode().strip()).parent
                target = output / 'media-fixture-arm'
                run(['arm-linux-gnueabihf-gcc', '-nostdlib', '-pie', '-Wl,-z,now,-z,relro',
                     '-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3', '-Wl,--unresolved-symbols=ignore-in-shared-libs',
                     '-Wl,-rpath-link,' + str(stock / 'lib'), crt / 'Scrt1.o', crt / 'crti.o', gcc / 'crtbeginS.o',
                     *objects, stock / 'lib/libm.so.6', stock / 'lib/libc.so.6', stock / 'lib/libgcc_s.so.1',
                     '-lgcc', gcc / 'crtendS.o', crt / 'crtn.o', '-o', target])
                symbols = run(['arm-linux-gnueabihf-readelf', '--dyn-syms', '-W', target]).decode()
                (output / 'dynsyms.txt').write_text(symbols)
                versions = [(int(a), int(b)) for a, b in re.findall(r'@GLIBC_(\d+)\.(\d+)', symbols)]
                assert versions and all(v <= (2, 25) for v in versions)
                (stock / 'tmp').mkdir(exist_ok=True)
                shutil.copyfile(shutil.which('qemu-arm-static'), stock / 'qemu-arm-static')
                (stock / 'qemu-arm-static').chmod(0o755)
                shutil.copyfile(target, stock / 'tmp/media-fixture'); (stock / 'tmp/media-fixture').chmod(0o755)
                report['stock_arm'] = json.loads(run(['unshare', '--net', 'chroot', stock, '/qemu-arm-static', '/tmp/media-fixture']))
                report['stock_image_sha256'] = sha(image); report['arm_binary_sha256'] = sha(target)
                report['stock_glibc225'] = True; save()
        report['source_unchanged'] = all(sha(ROOT / path) == value for path, value in report['sources'].items())
        report['passed'] = report['source_unchanged']; save()
    except BaseException as error:
        report['failure'] = dict(type=type(error).__name__, message=str(error)[-2000:]); save(); raise
    print(json.dumps(dict(passed=report['passed'], report_sha256=sha(output / 'report.json'))))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()

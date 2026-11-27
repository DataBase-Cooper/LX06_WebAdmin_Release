"""Synthetic local I/O fixtures. No speaker/device/account/cloud access."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / 'runtime/dialogue-core/io'
CASES = ['player-fifo', 'player-partial', 'player-recover', 'player-stop',
         'player-stale', 'player-backpressure', 'player-inflight-stop', 'player-old-stop-owner',
         'player-fixed-worker', 'player-idle-no-spin', 'player-phrase-resume', 'player-validation', 'player-events-overflow',
         'lights-active', 'lights-stale', 'lights-latest', 'lights-failure',
         'lights-fd-reuse', 'lights-destroy-active', 'lights-destroy-closed', 'lights-untrusted', 'lights-two-segments',
         'capture-basic', 'capture-lock', 'capture-stall', 'capture-untrusted',
         'capture-replaced-path', 'capture-malformed', 'capture-v2-basic',
         'capture-v2-malformed', 'capture-v2-invalid-metadata', 'capture-v2-explicit',
         'alsa-public-abi']
OWNER_CASES = ['transition', 'same-effect', 'failure-keeps-light',
               'client-end-active', 'slow-obsolete', 'close-slow']


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    completed = subprocess.run([str(v) for v in command], capture_output=True, **kwargs)
    if completed.returncode:
        raise RuntimeError((completed.returncode, completed.stderr[-1800:].decode(errors='replace')))
    return completed.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-name', required=True)
    parser.add_argument('--arm', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'build/rc24-native-tts-offline' / args.output_name
    output.mkdir(parents=True, exist_ok=False)
    sources = [SOURCE / f for f in ['player.c', 'alsa.c', 'capture.c', 'lights.c', 'tests/fixture.c']]
    common = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-Wl,--wrap=pthread_create']
    evidence_sources = sources + [SOURCE / p for p in [
        'io.h', 'compat.h', 'tests/test_io.py', 'tests/led-fixture.h',
        'tests/fake-ubus.c', 'tests/light-owner-fixture.c']]
    evidence_sources += [ROOT/'runtime/dialogue-core/types.h',
                         ROOT/'hooks/xiaoai/dialog-light-owner.h']
    report = dict(passed=False, actual_device=False, cloud_requests=0, old_sources_changed=True,
                  trusted_light_owner_change_authorized=True,
                  sources={str(p.relative_to(ROOT)): sha(p) for p in evidence_sources}, host=[], sanitized=[], stock_arm=[],
                  light_owner_host=[], light_owner_sanitized=[], light_owner_stock_arm=[])
    report['light_owner_source_sha256'] = sha(ROOT/'hooks/xiaoai/dialog-light-owner.h')
    def save():
        (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    save()
    binary = output / 'io-fixture'
    run(['gcc', *common, *sources, '-ldl', '-o', binary])
    for case in CASES:
        record = json.loads(run(['unshare', '--net', binary, case], timeout=12))
        report['host'].append(dict(case=case, **record))
        save()
    sanitized = output / 'io-fixture-sanitized'
    run(['gcc', *common, '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         *sources, '-ldl', '-o', sanitized])
    for case in CASES:
        record = json.loads(run(['unshare', '--net', sanitized, case], timeout=12,
                               env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1')))
        report['sanitized'].append(dict(case=case, **record))
        save()
    fake = output/'fake-ubus'
    run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-static', SOURCE/'tests/fake-ubus.c', '-o', fake])
    owner = output/'light-owner-fixture'
    owner_flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function']
    run(['gcc', *owner_flags, '-DLX06_LED_UBUS="'+str(fake)+'"', SOURCE/'tests/light-owner-fixture.c', '-o', owner])
    owner_sanitized = output/'light-owner-fixture-sanitized'
    run(['gcc', *owner_flags, '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
         '-DLX06_LED_UBUS="'+str(fake)+'"', SOURCE/'tests/light-owner-fixture.c', '-o', owner_sanitized])
    for case in OWNER_CASES:
        report['light_owner_host'].append(dict(case=case, **json.loads(run(['unshare', '--net', owner, case], timeout=12))))
        report['light_owner_sanitized'].append(dict(case=case, **json.loads(run(['unshare', '--net', owner_sanitized, case], timeout=12,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1')))))
        save()
    if args.arm:
        image = ROOT / 'build/releases/LX06Installer-V20261031.1-rc23/official-ssh/root.squashfs'
        with tempfile.TemporaryDirectory(prefix='dialogue-core-io-arm-') as temporary:
            temp = Path(temporary)
            stock = temp / 'stock'
            run(['unsquashfs', '-q', '-d', stock, image])
            objects = []
            for index, source in enumerate(sources):
                obj = temp / f'{index}.o'
                run(['arm-linux-gnueabihf-gcc', *common[:7], '-D_TIME_BITS=32', '-D_FILE_OFFSET_BITS=32',
                     '-fPIE', '-c', source, '-o', obj])
                objects.append(obj)
            crt = Path('/usr/arm-linux-gnueabihf/lib')
            gcc = Path(run(['arm-linux-gnueabihf-gcc', '-print-file-name=crtbeginS.o']).decode().strip()).parent
            target = output / 'io-fixture-arm'
            run(['arm-linux-gnueabihf-gcc', '-nostdlib', '-pie', '-Wl,-z,now,-z,relro',
                 '-Wl,--wrap=pthread_create', '-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3',
                 '-Wl,--unresolved-symbols=ignore-in-shared-libs',
                 '-Wl,-rpath-link,'+str(stock/'lib'), '-Wl,-rpath-link,'+str(stock/'usr/lib'),
                 crt/'Scrt1.o', crt/'crti.o', gcc/'crtbeginS.o', *objects,
                 stock/'lib/libpthread.so.0', stock/'lib/libdl.so.2', stock/'lib/librt.so.1',
                 stock/'lib/libc.so.6', stock/'lib/libgcc_s.so.1', '-lgcc',
                 gcc/'crtendS.o', crt/'crtn.o', '-o', target])
            symbols = run(['arm-linux-gnueabihf-readelf', '--dyn-syms', '-W', target]).decode()
            (output/'arm-dynsyms.txt').write_text(symbols)
            versions = re.findall(r'@GLIBC_(\d+)\.(\d+)', symbols)
            assert all((int(a), int(b)) <= (2, 25) for a, b in versions)
            for folder in ['proc', 'tmp']:
                (stock/folder).mkdir(exist_ok=True)
            (stock/'dev').mkdir(exist_ok=True)
            if not (stock/'dev/null').exists():
                import stat
                os.mknod(stock/'dev/null', stat.S_IFCHR | 0o666, os.makedev(1, 3))
            shutil.copyfile(shutil.which('qemu-arm-static'), stock/'qemu-arm-static')
            (stock/'qemu-arm-static').chmod(0o755)
            shutil.copyfile(target, stock/'tmp/io-fixture')
            (stock/'tmp/io-fixture').chmod(0o755)
            # A private local mount namespace supplies real FD procfs. Its
            # lifetime ends with this command; no host or device paths changed.
            script = 'mount --bind /dev/null "$1/dev/null" && mount -t proc proc "$1/proc" && exec chroot "$1" /qemu-arm-static /tmp/io-fixture "$2"'
            for case in CASES:
                record = json.loads(run(['unshare', '--mount', '--net', 'sh', '-c', script, 'io-arm', stock, case], timeout=15))
                report['stock_arm'].append(dict(case=case, **record))
                save()
            owner_obj = temp/'owner.o'
            run(['arm-linux-gnueabihf-gcc', *owner_flags, '-D_TIME_BITS=32', '-D_FILE_OFFSET_BITS=32',
                 '-fPIE', '-DLX06_LED_UBUS="/tmp/fake-ubus"', '-c', SOURCE/'tests/light-owner-fixture.c', '-o', owner_obj])
            owner_arm = output/'light-owner-fixture-arm'
            run(['arm-linux-gnueabihf-gcc', '-nostdlib', '-pie', '-Wl,-z,now,-z,relro',
                 '-Wl,--dynamic-linker,/lib/ld-linux-armhf.so.3',
                 '-Wl,--unresolved-symbols=ignore-in-shared-libs', '-Wl,-rpath-link,'+str(stock/'lib'),
                 crt/'Scrt1.o', crt/'crti.o', gcc/'crtbeginS.o', owner_obj,
                 stock/'lib/libc.so.6', stock/'lib/libgcc_s.so.1', '-lgcc',gcc/'crtendS.o',crt/'crtn.o','-o',owner_arm])
            shutil.copyfile(owner_arm,stock/'tmp/light-owner-fixture');(stock/'tmp/light-owner-fixture').chmod(0o755)
            shutil.copyfile(fake,stock/'tmp/fake-ubus');(stock/'tmp/fake-ubus').chmod(0o755)
            owner_script = 'mount --bind /dev/null "$1/dev/null" && mount -t proc proc "$1/proc" && exec chroot "$1" /qemu-arm-static /tmp/light-owner-fixture "$2"'
            for case in OWNER_CASES:
                record = json.loads(run(['unshare','--mount','--net','sh','-c',owner_script,'owner-arm',stock,case],timeout=15))
                report['light_owner_stock_arm'].append(dict(case=case,**record));save()
            report['stock_image_sha256'] = sha(image)
            report['stock_glibc225'] = True
            report['arm_binary_sha256'] = sha(target)
    report['passed'] = True
    save()
    print(json.dumps(dict(passed=True, host=len(report['host']), sanitized=len(report['sanitized']),
                          stock_arm=len(report['stock_arm']), light_owner_cases=len(report['light_owner_host']),
                          report_sha256=sha(output/'report.json'))))


if __name__ == '__main__':
    main()

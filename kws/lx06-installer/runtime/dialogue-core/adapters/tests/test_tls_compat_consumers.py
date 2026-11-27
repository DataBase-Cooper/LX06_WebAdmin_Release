"""Exercise real TLS certificate consumers with synthetic in-memory peers.

Native OpenSSL runs by default. To compile with actual 1.0 target headers,
provide LX06_TLS_TARGET_PREFIX and LX06_TLS_SYSROOT (containing include/, crt/,
lib/); ARM executables run under QEMU with copied target DSOs. No devices,
network sockets, provider accounts or production registration are used.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class TlsConsumers(unittest.TestCase):
    def test_actual_certificate_consumers(self):
        prefix = os.environ.get('LX06_TLS_TARGET_PREFIX')
        sysroot = os.environ.get('LX06_TLS_SYSROOT')
        self.assertEqual(bool(prefix), bool(sysroot), 'provide both target prefix and sysroot')
        with tempfile.TemporaryDirectory(prefix='lx06-tls-consumers-') as tmp:
            work = Path(tmp)
            for index, name in enumerate(('phone', 'xiaozhi', 'doubao')):
                with self.subTest(consumer=name):
                    binary, obj = work / name, work / (name + '.o')
                    cc = 'arm-linux-gnueabihf-gcc' if prefix else 'gcc'
                    flags = ['-std=c11', '-O2', '-Wall', '-Wextra',
                             '-Wno-deprecated-declarations', '-Wno-misleading-indentation',
                             '-D_POSIX_C_SOURCE=200809L', '-ffunction-sections', '-fdata-sections',
                             '-DTLS_CONSUMER=' + str(index)]
                    if prefix:
                        target, sdk = Path(prefix), Path(sysroot)
                        flags += ['-DXZ_OPENSSL_1_0_COMPAT', '-fPIE', '-D_TIME_BITS=32',
                                  '-D_FILE_OFFSET_BITS=32', '-nostdinc', '-isystem', str(sdk / 'include'),
                                  '-isystem', str(sdk / 'gcc-include'), '-I' + str(target / 'include')]
                    subprocess.run([cc, *flags, '-c', str(HERE / 'tls_compat_consumers.c'),
                                    '-o', str(obj)], check=True)
                    if not prefix:
                        link = [cc, '-Wl,--gc-sections', str(obj), '-lssl', '-lcrypto', '-o', str(binary)]
                        launch = [str(binary)]
                    else:
                        crt, lib, root = sdk / 'crt', sdk / 'lib', work / 'root'
                        (root / 'lib').mkdir(parents=True, exist_ok=True)
                        for directory in (lib, target / 'lib'):
                            for path in directory.iterdir():
                                if path.is_file() and '.so' in path.name:
                                    shutil.copyfile(path, root / 'lib' / path.name)
                        link = [cc, '-nostdlib', '-pie', '-Wl,--gc-sections,-z,now,-z,relro,-z,defs',
                                '-Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3',
                                '-Xlinker', '-rpath-link', '-Xlinker', str(root / 'lib'),
                                *map(str, (crt / n for n in ('Scrt1.o', 'crti.o', 'crtbeginS.o', 'elf-init.o'))),
                                str(obj), str(target / 'lib/libssl.so.1.0.0'), str(target / 'lib/libcrypto.so.1.0.0'),
                                *map(str, (lib / n for n in ('libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'libgcc_s.so.1'))),
                                str(crt / 'crtendS.o'), str(crt / 'crtn.o'), '-o', str(binary)]
                        launch = ['qemu-arm', '-L', str(root), str(binary)]
                    subprocess.run(link, check=True)
                    result = subprocess.run(launch, check=True, capture_output=True, text=True, timeout=120)
                    self.assertIn('actual phone peer_verified:' if index == 0 else 'actual ws verify_cert_host:', result.stdout)
                    self.assertIn('rejected', result.stdout)


if __name__ == '__main__':
    unittest.main()

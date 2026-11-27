"""Offline byte-level declaration seam; not a session identity test."""
import ctypes as C
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AccountDeclarationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='dc-declaration-')
        lib = Path(cls.tmp.name) / 'account.so'
        subprocess.run(['gcc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-shared', '-fPIC', str(ROOT / 'account.c'),
                        '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def parse(self, data):
        self.assertTrue(hasattr(self.lib, 'dc_account_parse_declaration'),
                        'account declaration parser is not available')
        fn = self.lib.dc_account_parse_declaration
        fn.argtypes = [C.c_void_p, C.c_size_t]
        fn.restype = C.c_int
        buf = C.create_string_buffer(data)
        return fn(buf, len(data))

    def test_generated_account_declaration_is_opt_in_and_body_unchanged(self):
        generator = ROOT.parent / 'model-adapter' / 'build_accounts.py'
        with tempfile.TemporaryDirectory(prefix='dc-generated-') as directory:
            base = Path(directory) / 'legacy'
            subprocess.run(['python3', str(generator), str(base)], check=True)
            for policy, expected in [('keyword', 1), ('voice', 2)]:
                output = Path(directory) / policy
                subprocess.run(['python3', str(generator), str(output),
                                '--experimental-account-declaration', policy], check=True)
                for name in ('chatgpt_account.sh', 'grok_account.sh'):
                    legacy = (base / name).read_bytes()
                    declared = (output / name).read_bytes()
                    header = (b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n'
                              + b'# LX06-INTERRUPTION: ' + policy.encode() + b'\n')
                    self.assertTrue(declared.startswith(header))
                    self.assertEqual(declared, header + legacy[len(b'#!/bin/sh\n'):])
                    self.assertEqual(self.parse(legacy), 0)
                    self.assertEqual(self.parse(declared), expected)

    def test_exact_opt_in_and_legacy(self):
        self.assertEqual(self.parse(b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n'
                                    b'# LX06-INTERRUPTION: keyword\nexit 0\n'), 1)
        self.assertEqual(self.parse(b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n'
                                    b'# LX06-INTERRUPTION: voice\nexit 0\n'), 2)
        self.assertEqual(self.parse(b'#!/bin/sh\nexit 0\n'), 0)

    def test_header_prefixes_and_duplicates_fail_closed(self):
        identity = b'# LX06-ACCOUNT-IDENTITY: v1\n'
        mode = b'# LX06-INTERRUPTION: keyword\n'
        for prefix in (b' ', b'\t', b'\xc2\xa0', b'\xe2\x80\x8b', b'\xef\xbb\xbf'):
            for tag in (identity, mode):
                for data in (b'#!/bin/sh\n' + prefix + tag,
                             b'#!/bin/sh\n' + identity + mode + prefix + tag):
                    with self.subTest(data=data):
                        self.assertEqual(self.parse(data), -1)

    def test_body_is_not_header_even_for_literal_declaration_lines(self):
        header = b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\n'
        body = b"cat <<'EOF'\n# LX06-INTERRUPTION: voice\nEOF\nprintf '%s\\n' '\n# LX06-ACCOUNT-IDENTITY: v2\n'\n"
        self.assertEqual(self.parse(header + body), 1)
        self.assertEqual(self.parse(b'#!/bin/sh\n' + body), 0)

    def test_malformed_header_spellings_and_order_fail_closed(self):
        cases = (b'#LX06-INTERRUPTION: keyword\n', b'#  LX06-INTERRUPTION: keyword\n',
                 b'#\xc2\xa0LX06-INTERRUPTION: keyword\n',
                 b'# LX06-ACCOUNT-IDENTITY: v1\r\n',
                 b'# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword',
                 b'# ordinary\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: voice\n',
                 b'# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\n\n# LX06-INTERRUPTION: voice\n')
        for case in cases:
            with self.subTest(case=case):
                self.assertEqual(self.parse(b'#!/bin/sh\n' + case), -1)

    def test_invalid_declarations_fail_closed(self):
        cases = [
            b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v2\n# LX06-INTERRUPTION: keyword\n',
            b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: other\n',
            b'#!/bin/sh\n# LX06-INTERRUPTION: keyword\n',
            b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\nexit 0\n',
            b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\n# LX06-INTERRUPTION: voice\n',
            b'#!/bin/sh\n# LX06-ACCOUNT-IDENTITY: v1\n# LX06-INTERRUPTION: keyword\n\0exit 0\n',
        ]
        for data in cases:
            with self.subTest(data=data):
                self.assertEqual(self.parse(data), -1)


if __name__ == '__main__':
    unittest.main()

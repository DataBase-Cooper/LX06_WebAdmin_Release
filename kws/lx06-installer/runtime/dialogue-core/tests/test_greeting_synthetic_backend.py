"""Offline fixture contract, not a production cache or provider identity."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parent

class SyntheticBackendContract(unittest.TestCase):
    def test_upload_versioned_cache_and_reject_stale_or_tampered(self):
        path = HERE / 'greeting_synthetic_backend.py'
        self.assertTrue(path.is_file(), 'test-only synthetic backend not implemented')
        spec = importlib.util.spec_from_file_location('fixture_backend', path)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            backend = module.Backend(Path(tmp))
            backend.upload('fixture greeting', 1)
            self.assertEqual(backend.lookup('fixture greeting', 1), module.PCM)
            self.assertIsNone(backend.lookup('fixture greeting', 2))
            self.assertIsNone(backend.lookup('different greeting', 1))
            backend.cache.write_bytes(backend.cache.read_bytes()[:-1] + b'X')
            self.assertIsNone(backend.lookup('fixture greeting', 1))

    def test_real_core_plays_synthetic_upload_cache(self):
        import os
        binary = os.environ.get('DC_GREETING_FIXTURE_CORE')
        if not binary:
            self.skipTest('requires isolated Linux fixture and DC_GREETING_FIXTURE_CORE')
        spec = importlib.util.spec_from_file_location('fixture_backend', HERE / 'greeting_synthetic_backend.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        self.assertTrue(hasattr(module, 'run_reactor'), 'real reactor fixture path missing')
        module.run_reactor(Path(binary))

if __name__ == '__main__': unittest.main()

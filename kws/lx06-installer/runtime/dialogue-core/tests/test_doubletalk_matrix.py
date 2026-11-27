"""Offline matrix invariants; run on Linux with SpeexDSP and gcc installed."""
import json
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / 'doubletalk_matrix.py'

class MatrixTest(unittest.TestCase):
    def run_matrix(self, seed):
        p = subprocess.run([sys.executable, str(RUNNER), '--seed', str(seed), '--quick'],
                           check=True, capture_output=True, text=True)
        return json.loads(p.stdout)

    def test_stress_scenarios_paired_and_deterministic(self):
        def run():
            return json.loads(subprocess.run([sys.executable, str(RUNNER), '--seed', '431', '--quick', '--stress'], check=True, capture_output=True, text=True).stdout)
        a = run()
        self.assertEqual(a, run())
        self.assertEqual({r['scenario'] for r in a['cases']}, {'oracle', 'late80', 'early80', 'drift160', 'path_gain_shift'})
        for scenario in {r['scenario'] for r in a['cases']}:
            rows = [r for r in a['cases'] if r['scenario'] == scenario]
            self.assertEqual(len(rows), 48)
            self.assertEqual({r['class'] for r in rows}, {'far', 'mixed', 'near'})
            self.assertIn('false_accepts', a['scenario_summary'][scenario]['guard'])
            self.assertIn('misses', a['scenario_summary'][scenario]['legacy_vad'])
        def paired(scenario):
            return {(r['far_family'], r['near_family'], r['delay_samples'], r['echo_gain'], r['class']): r for r in a['cases'] if r['scenario'] == scenario}
        oracle = paired('oracle')
        for scenario in ('late80', 'early80', 'drift160'):
            for key, row in paired(scenario).items():
                self.assertEqual((row['peak'], row['decisions']['guard'], row['guard_echo_frames']),
                                 (oracle[key]['peak'], oracle[key]['decisions']['guard'], oracle[key]['guard_echo_frames']))
            self.assertTrue(any(row['legacy_residual_ratio'] != oracle[key]['legacy_residual_ratio'] for key, row in paired(scenario).items()))
        self.assertTrue(any(row['peak'] != oracle[key]['peak'] for key, row in paired('path_gain_shift').items() if key[-1] == 'far'))

    def test_matrix_reproducible_and_balanced(self):
        a = self.run_matrix(431)
        b = self.run_matrix(431)
        self.assertEqual(a, b)
        self.assertEqual({r['class'] for r in a['cases']}, {'far', 'mixed', 'near'})
        self.assertGreaterEqual(len({r['far_family'] for r in a['cases']}), 2)
        self.assertGreaterEqual(len({r['near_family'] for r in a['cases']}), 2)
        for row in a['cases']:
            self.assertEqual(set(row['decisions']), {'guard', 'speex_vad', 'legacy_vad'})
            self.assertLess(row['peak'], 32768)
        self.assertNotEqual(a['signal_digest'], self.run_matrix(432)['signal_digest'])

if __name__ == '__main__':
    unittest.main()

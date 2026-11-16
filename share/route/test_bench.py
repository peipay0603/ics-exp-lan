import json
from pathlib import Path
import tempfile
import unittest

import bench


class BenchmarkTests(unittest.TestCase):
    def test_failed_or_too_short_trials_do_not_enter_summary(self):
        rows = [dict(case='A', status=status, bytes=1000000, seconds=seconds)
                for status, seconds in [('OK', 2), ('OK', 4), ('ERROR', .1), ('TOO_SHORT', .2)]]
        value = bench.summarize(rows)['A']
        self.assertEqual(value['success'], 2)
        self.assertEqual(value['failed'], 2)
        self.assertEqual(value['median_s'], 3)
        self.assertAlmostEqual(value['median_mbps'], 8 / 3)

    def test_mixed_sizes_are_rejected(self):
        with self.assertRaises(ValueError):
            bench.summarize([dict(case='A', status='OK', bytes=size, seconds=1) for size in [10, 20]])

    def test_metrics_reject_failure_missing_duplicate_and_nan(self):
        with tempfile.TemporaryDirectory() as folder:
            log = Path(folder) / 'client.log'
            for contents in ['', 'ROUTE_METRICS {"seconds": 1, "exit": 1}',
                             'ROUTE_METRICS {"seconds": NaN, "exit": 0}',
                             'ROUTE_METRICS {"seconds": 1, "exit": 0}\n' * 2]:
                log.write_text(contents)
                with self.assertRaises(ValueError):
                    bench.metrics(log)
            log.write_text('child output\nROUTE_METRICS ' + json.dumps({'seconds': 2.5, 'exit': 0}))
            self.assertEqual(bench.metrics(log)['seconds'], 2.5)


if __name__ == '__main__':
    unittest.main()

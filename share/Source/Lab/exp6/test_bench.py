"""ホストPCで実行。稼働中のnode1〜5とビルド済み.outを使う測定スクリプトの統合テスト。"""
import csv
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

import bench


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        (bench.HERE / 'results').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=str(bench.HERE / 'results'))
        self.directory = Path(self.temp.name)
        self.source = self.directory / 'source.dat'
        self.source.write_bytes(os.urandom(4097))
        self.csv = self.directory / 'bench.csv'
        self.env = dict(os.environ, FILE=str(self.source), REPEAT='1', PORT='24970',
                        TIMEOUT='4', MIN_SECONDS='0', CSV=str(self.csv), SEND_OPTS='', RECV_OPTS='',
                        LABEL='quoted, label\n2行目')

    def tearDown(self):
        self.temp.cleanup()

    def run_bench(self, destination='172.21.0.10'):
        r = subprocess.run(['python3', str(bench.HERE / 'bench.py'), 'tcp', destination, '1000'],
                           env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        with self.csv.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        return r, rows

    def test_repeated_success_and_csv_quoting(self):
        self.env['REPEAT'] = '2'
        result, rows = self.run_bench()
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(len(rows), 2)
        self.assertNotIn(b'\r\n', self.csv.read_bytes())
        for row in rows:
            self.assertEqual(row['status'], 'OK')
            self.assertEqual(row['cmp'], 'OK')
            self.assertEqual(row['bytes'], '4097')
            self.assertEqual(row['confirmed'], '1')
            self.assertEqual(row['label'], self.env['LABEL'])
            log = bench.HERE / row['logs'] / 'receiver.log'
            self.assertTrue(log.is_file())

    def test_explicit_legacy_mode_is_recorded(self):
        self.env['SEND_OPTS'] = '-L'
        result, rows = self.run_bench()
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(rows[0]['confirmed'], '0')
        self.assertEqual(rows[0]['cmp'], 'OK')

    def test_failure_does_not_stop_an_unrelated_receiver(self):
        other_pid = uuid.uuid4().hex
        other = bench.Job('node1', [bench.REMOTE + '/tcp_recv.out', '24971', '/tmp/' + other_pid],
                          self.directory / 'unrelated.log', 30)
        try:
            other.ready('waiting connection')
            result, rows = self.run_bench('172.21.0.10:24972')
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(rows[0]['status'], 'ERROR')
            self.assertEqual(rows[0]['cmp'], 'NOT_RUN')
            self.assertNotIn(b'AVERAGE ', result.stdout)
            self.assertIsNone(other.process.poll())
        finally:
            other.close()
            bench.docker('node1', 'rm', '-f', '--', '/tmp/' + other_pid)

    def test_short_transfer_is_not_accepted_as_long_run(self):
        self.env['MIN_SECONDS'] = '100'
        result, rows = self.run_bench()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(rows[0]['status'], 'TOO_SHORT')
        self.assertEqual(rows[0]['cmp'], 'OK')
        self.assertNotIn(b'AVERAGE ', result.stdout)

    def test_occupied_port_does_not_consume_or_stop_another_receiver(self):
        other = bench.Job('node1', [bench.REMOTE + '/tcp_recv.out', self.env['PORT'], '/tmp/exp6-unused'],
                          self.directory / 'occupied.log', 30)
        try:
            other.ready('waiting connection')
            result, rows = self.run_bench()
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(rows[0]['status'], 'ERROR')
            self.assertEqual(rows[0]['sender_exit'], '')
            self.assertIsNone(other.process.poll())
        finally:
            other.close()

    def test_cleanup_when_docker_client_exits_before_remote_job(self):
        job = bench.Job('node1', [bench.REMOTE + '/tcp_recv.out', '24973', '/tmp/exp6-unused'],
                        self.directory / 'detached.log', 30)
        try:
            job.ready('waiting connection')
            pid = bench.docker('node1', 'cat', job.pidfile).stdout.decode().strip()
            job.process.terminate()  # docker execクライアントだけが中断された状況
            job.process.wait(timeout=3)
        finally:
            job.close()
        probe = bench.docker('node1', 'sh', '-c', 'kill -0 "$1" 2>/dev/null', 'probe', pid)
        self.assertNotEqual(probe.returncode, 0)

    def test_timeout_removes_only_its_partial_receive_file(self):
        listing = ['sh', '-c', 'find /tmp -maxdepth 1 -name "exp6-recv-*.part.*" -print']
        before = set(bench.docker('node1', *listing).stdout.splitlines())
        self.env['TIMEOUT'] = '1'
        result = subprocess.run(['python3', str(bench.HERE / 'bench.py'), 'hyb',
                                 '172.21.0.10:24972@10', '1464'], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        self.assertNotEqual(result.returncode, 0, result.stdout.decode())
        after = set(bench.docker('node1', *listing).stdout.splitlines())
        self.assertEqual(after, before)
        with self.csv.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(rows[0]['status'], 'ERROR')
        self.assertNotIn(b'AVERAGE ', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)

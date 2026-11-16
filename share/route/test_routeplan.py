import json
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest

import services
from test_transfer import HERE, free_port


class PlanTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='route-plan-')
        self.root = Path(self.temp.name)
        self.plans = []

    def tearDown(self):
        for manifest in self.plans:
            services.control('stop', manifest, 3)
        self.temp.cleanup()

    def plan(self, name, options):
        out = self.root / name
        result = subprocess.run(['python3', str(HERE / 'routeplan.py'), '--links', str(HERE / 'links_tc2.txt'),
                                 '--outdir', str(out)] + options,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        return out

    def test_maxflow_and_single_route(self):
        out = self.plan('four', ['--standby-hops', '0'])
        value = json.loads((out / 'plan.json').read_text())
        self.assertEqual(value['nominal_mbps'], 69)
        self.assertEqual(sorted(rate for _, rate in value['chosen']), [9, 10, 10, 40])
        out = self.plan('single', ['--mode', 'single', '--standby-hops', '0'])
        value = json.loads((out / 'plan.json').read_text())
        self.assertEqual(value['chosen'], [[[3, 4, 5, 1], 50]])

    def test_generated_scripts_preserve_spaces_and_shell_characters(self):
        # 実際に生成スクリプトから起動し、空白・$等を含む名前をargvとして扱う。
        source = self.root / "source ' $(touch SHOULD_NOT_EXIST)"
        source.write_bytes(b'source')
        port = free_port()
        out = self.plan('nested directory/plan', ['--path', '3-1', '--standby-hops', '0',
                        '--file', str(source), '--server-port', str(port)])
        self.plans.append(out / 'plan.json')
        subprocess.check_call(['bash', str(out / 'node3.sh')])
        with socket.create_connection(('127.0.0.1', port), timeout=2) as sock:
            sock.sendall(b'SIZE\n')
            self.assertEqual(sock.recv(100), b'SIZE 6\n')
        self.assertFalse((HERE / 'SHOULD_NOT_EXIST').exists())

    def test_stopping_a_plan_keeps_other_same_named_service_running(self):
        source = self.root / 'source'
        source.write_bytes(b'data')
        manifests, ports = [], []
        for index in range(2):
            folder = self.root / str(index)
            folder.mkdir()
            port = free_port()
            manifest = folder / 'plan.json'
            manifest.write_text(json.dumps({'services': {'3': [{
                'command': ['fserver.out', str(source), str(port)], 'ready': 'fserver:'}]}}))
            self.plans.append(manifest)
            services.control('start', manifest, 3)
            manifests.append(manifest)
            ports.append(port)
        services.control('stop', manifests[0], 3)
        with socket.create_connection(('127.0.0.1', ports[1]), timeout=2) as sock:
            sock.sendall(b'SIZE\n')
            self.assertEqual(sock.recv(100), b'SIZE 4\n')
        # PIDが再利用された場合と同じく、開始時刻が違う記録では停止しない。
        state = manifests[1].parent / 'services-node3.json'
        records = json.loads(state.read_text())
        stale = json.loads(state.read_text())
        stale[0]['identity']['start'] = 'wrong-start-time'
        state.write_text(json.dumps(stale))
        services.control('stop', manifests[1], 3)
        self.assertTrue(services.owned(records[0]))
        state.write_text(json.dumps(records))

    def test_generated_client_forwards_extra_arguments(self):
        # 実ネットワークへ接続せず、生成スクリプトから実クライアントへ-hを渡す。
        # 引数が渡ればusageで即終了し、接続待ちには入らない。
        out = self.plan('client', ['--path', '3-1', '--standby-hops', '0'])
        result = subprocess.run(['bash', str(out / 'node1.sh'), '-h'], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Usage:', result.stdout)
        self.assertNotIn(b'connect ', result.stdout)


if __name__ == '__main__':
    unittest.main()

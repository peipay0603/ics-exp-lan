"""Linuxコンテナ内で実行する、実際のソケットを使った回帰テスト（Python 3.6対応）。"""
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

HERE = Path(__file__).resolve().parent


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class UDPProxy:
    """初回欠損・順序逆転・重複・別セッションのデータを混ぜる。"""
    def __init__(self, target, discard_all=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.sock.settimeout(0.01)
        self.port = self.sock.getsockname()[1]
        self.target = ('127.0.0.1', target)
        self.stopped = threading.Event()
        self.dropped = set()
        self.discard_all = discard_all
        self.worker = threading.Thread(target=self.run)
        self.worker.start()

    def run(self):
        held = None
        while not self.stopped.is_set():
            try:
                data, _ = self.sock.recvfrom(65536)
            except socket.timeout:
                if held:
                    self.sock.sendto(held, self.target)
                    held = None
                continue
            session, seq = struct.unpack('!II', data[:8])
            if self.discard_all:
                continue
            if seq % 13 == 0 and seq not in self.dropped:
                self.dropped.add(seq)
                continue
            if seq % 11 == 0:
                stale = struct.pack('!II', session ^ 1, seq) + data[8:]
                self.sock.sendto(stale, self.target)
                self.sock.sendto(data, self.target)
            if held is None:
                held = data
            else:
                self.sock.sendto(data, self.target)
                self.sock.sendto(held, self.target)
                held = None

    def close(self):
        self.stopped.set()
        self.worker.join(1)
        self.sock.close()


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.processes = []
        self.proxies = []

    def tearDown(self):
        for proxy in self.proxies:
            proxy.close()
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdout.close()
        self.temp.cleanup()

    def source(self, size):
        path = self.directory / 'source.dat'
        path.write_bytes(os.urandom(size))
        return path

    def launch(self, name, *args):
        p = subprocess.Popen([str(HERE / (name + '.out'))] + list(map(str, args)),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.processes.append(p)
        self.assertTrue(select.select([p.stdout], [], [], 3)[0], name + ' did not start')
        line = p.stdout.readline().decode()
        self.assertTrue(line.startswith(('waiting connection', 'tcp relay:', 'udp relay:')), line)
        return p

    def send(self, name, *args, success=True):
        r = subprocess.run([str(HERE / (name + '.out'))] + list(map(str, args)),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=12)
        if success:
            self.assertEqual(r.returncode, 0, r.stdout.decode())
        else:
            self.assertNotEqual(r.returncode, 0, r.stdout.decode())
        return r.stdout.decode()

    def receive(self, process, source, output):
        log, _ = process.communicate(timeout=12)
        self.assertEqual(process.returncode, 0, log.decode())
        self.assertEqual(output.read_bytes(), source.read_bytes())
        return log.decode()

    def test_tcp_two_relays_repeated_and_unaligned(self):
        target, first, second = free_port(), free_port(), free_port()
        relays = [self.launch('tcp_relay', '-b', 1000, second, '127.0.0.1', target),
                  self.launch('tcp_relay', '-b', '64K', first, '127.0.0.1', second)]
        for size, buf in [(0, 10), (1, 1000), (65537, '64K'), (1048593, 1000)]:
            with self.subTest(size=size):
                source = self.source(size)
                output = self.directory / 'received.dat'
                r = self.launch('tcp_recv', '-b', buf, target, output)
                self.send('tcp_send', '-b', buf, '127.0.0.1', first, source)
                self.receive(r, source, output)
        for relay in relays:
            self.assertIsNone(relay.poll())

    def test_tcp_relay_survives_unavailable_destination(self):
        target, via = free_port(), free_port()
        relay = self.launch('tcp_relay', via, '127.0.0.1', target)
        with socket.create_connection(('127.0.0.1', via), timeout=2) as sock:
            sock.settimeout(2)
            self.assertEqual(sock.recv(1), b'')
        source = self.source(131089)
        output = self.directory / 'received.dat'
        r = self.launch('tcp_recv', target, output)
        self.send('tcp_send', '127.0.0.1', via, source)
        self.receive(r, source, output)
        self.assertIsNone(relay.poll())

    def test_tcp_incomplete_transfer_is_an_error(self):
        target = free_port()
        output = self.directory / 'incomplete.dat'
        output.write_bytes(b'previous complete file')
        r = self.launch('tcp_recv', target, output)
        now = time.time()
        sec, nsec = int(now), int((now % 1) * 1e9)
        with socket.create_connection(('127.0.0.1', target), timeout=2) as sock:
            # ヘッダ自体も分割到着させる。
            header = struct.pack('!IIQqq', 0x45365443, 1000, 100, sec, nsec)
            sock.sendall(header[:3])
            time.sleep(0.01)
            sock.sendall(header[3:] + b'only 10!!!')
        log, _ = r.communicate(timeout=3)
        self.assertEqual(r.returncode, 2, log.decode())
        self.assertIn(b'INCOMPLETE', log)
        self.assertEqual(output.read_bytes(), b'previous complete file')
        self.assertFalse(list(self.directory.glob('*.part.*')))

    def test_tcp_sender_rejects_peer_that_discards_the_file(self):
        source = self.source(4097)
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        data = []

        def discard():
            with listener:
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(3)
                    while True:
                        block = sock.recv(65536)
                        if not block:
                            break
                        data.append(block)

        worker = threading.Thread(target=discard, daemon=True)
        worker.start()
        try:
            self.send('tcp_send', '127.0.0.1', port, source, success=False)
        finally:
            worker.join(4)
        self.assertFalse(worker.is_alive())
        self.assertEqual(b''.join(data)[32:], source.read_bytes())

    def test_tcp_receipts_are_required_and_validated(self):
        source = self.source(4097)
        for status, size_delta, success in [(0, 0, True), (1, 0, False), (0, -1, False)]:
            with self.subTest(status=status, size_delta=size_delta):
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0))
                    listener.listen(1)
                    listener.settimeout(3)
                    sender = subprocess.Popen([str(HERE / 'tcp_send.out'), '127.0.0.1',
                                               str(listener.getsockname()[1]), str(source)],
                                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    self.processes.append(sender)
                    peer, _ = listener.accept()
                    with peer:
                        peer.settimeout(3)
                        blocks = []
                        while True:
                            block = peer.recv(65536)
                            if not block:
                                break
                            blocks.append(block)
                        data = b''.join(blocks)
                        self.assertEqual(struct.unpack('!I', data[:4])[0], 0x45365432)
                        self.assertEqual(data[32:], source.read_bytes())
                        self.assertIsNone(sender.poll(), 'sender exited before a receipt')
                        receipt = struct.pack('!IIQ', 0x4536414b, status, source.stat().st_size + size_delta)
                        peer.sendall(receipt[:3])
                        time.sleep(0.01)
                        peer.sendall(receipt[3:])
                    log, _ = sender.communicate(timeout=3)
                    self.assertEqual(sender.returncode == 0, success, log.decode())

    def test_tcp_receipt_timeout(self):
        source = self.source(1)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(3)
            sender = subprocess.Popen([str(HERE / 'tcp_send.out'), '-t', '1', '127.0.0.1',
                                       str(listener.getsockname()[1]), str(source)],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.processes.append(sender)
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(3)
                while peer.recv(65536):
                    pass
                log, _ = sender.communicate(timeout=3)  # 接続は開いたまま、返信しない
            self.assertNotEqual(sender.returncode, 0, log.decode())
            self.assertIn(b'did not confirm', log)

    def test_tcp_atomic_replacement_and_legacy_mode(self):
        target = free_port()
        output = self.directory / 'atomic.dat'
        output.write_bytes(b'previous file')
        receiver = self.launch('tcp_recv', target, output)
        with socket.create_connection(('127.0.0.1', target), timeout=3) as peer:
            now = time.time()
            peer.sendall(struct.pack('!IIQqq', 0x45365443, 1000, 6, int(now), 0) + b'abc')
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                parts = list(self.directory.glob('atomic.dat.part.*'))
                if parts and parts[0].stat().st_size == 3:
                    break
                time.sleep(0.01)
            self.assertEqual(output.read_bytes(), b'previous file')
            self.assertEqual(len(parts), 1)
            peer.sendall(b'def')
        log, _ = receiver.communicate(timeout=3)
        self.assertEqual(receiver.returncode, 0, log.decode())
        self.assertEqual(output.read_bytes(), b'abcdef')
        self.assertFalse(list(self.directory.glob('*.part.*')))
        source = self.source(131089)
        receiver = self.launch('tcp_recv', target, output)
        log = self.send('tcp_send', '-L', '127.0.0.1', target, source)
        self.assertIn('confirmed=0', log)
        self.receive(receiver, source, output)

    def test_tcp_save_failure_is_reported_to_sender(self):
        target = free_port()
        output = self.directory / 'existing-directory'
        output.mkdir()
        source = self.source(4097)
        receiver = self.launch('tcp_recv', target, output)
        self.send('tcp_send', '127.0.0.1', target, source, success=False)
        log, _ = receiver.communicate(timeout=3)
        self.assertNotEqual(receiver.returncode, 0, log.decode())
        self.assertTrue(output.is_dir())
        self.assertFalse(list(self.directory.glob('*.part.*')))

    def test_tcp_relay_recovers_after_idle_connection(self):
        target, via = free_port(), free_port()
        relay = self.launch('tcp_relay', '-t', 1, via, '127.0.0.1', target)
        output = self.directory / 'received.dat'
        receiver = self.launch('tcp_recv', target, output)
        with socket.create_connection(('127.0.0.1', via), timeout=3) as peer:
            peer.settimeout(3)
            self.assertEqual(peer.recv(1), b'')
        log, _ = receiver.communicate(timeout=3)
        self.assertNotEqual(receiver.returncode, 0, log.decode())
        source = self.source(65537)
        receiver = self.launch('tcp_recv', target, output)
        self.send('tcp_send', '127.0.0.1', via, source)
        self.receive(receiver, source, output)
        self.assertIsNone(relay.poll())

    def test_hybrid_empty_and_partial_chunk(self):
        for size in (0, 1, 1465, 131089):
            with self.subTest(size=size):
                target = free_port()
                source = self.source(size)
                output = self.directory / 'received.dat'
                r = self.launch('hyb_recv', target, output)
                self.send('hyb_send', '127.0.0.1', target, source,
                          '127.0.0.1:%d@20' % target)
                self.receive(r, source, output)

    def test_hybrid_save_failure_is_not_reported_as_done(self):
        target = free_port()
        output = self.directory / 'existing-directory'
        output.mkdir()
        source = self.source(1465)
        receiver = self.launch('hyb_recv', target, output)
        self.send('hyb_send', '127.0.0.1', target, source, '127.0.0.1:%d@20' % target, success=False)
        log, _ = receiver.communicate(timeout=3)
        self.assertNotEqual(receiver.returncode, 0, log.decode())
        self.assertTrue(output.is_dir())
        self.assertFalse(list(self.directory.glob('*.part.*')))

    def test_hybrid_two_relays_and_weighted_paths(self):
        target, first, second = free_port(), free_port(), free_port()
        source = self.source(1048593)
        output = self.directory / 'received.dat'
        r = self.launch('hyb_recv', target, output)
        self.launch('udp_relay', second, '127.0.0.1', target)
        self.launch('udp_relay', first, '127.0.0.1', second)
        log = self.send('hyb_send', '127.0.0.1', target, source,
                        '127.0.0.1:%d@10' % target, '127.0.0.1:%d@20' % first)
        self.receive(r, source, output)
        paths = [line for line in log.splitlines() if line.startswith('Path ')]
        self.assertEqual(len(paths), 2)
        counts = [int(line.split('sent=')[1].split()[0]) for line in paths]
        self.assertGreater(counts[0], 0)
        self.assertAlmostEqual(counts[1] / counts[0], 2, delta=0.1)

    def test_hybrid_recovers_loss_reordering_duplicates_and_stale_session(self):
        target = free_port()
        source = self.source(131089)
        output = self.directory / 'received.dat'
        r = self.launch('hyb_recv', '-w', 30, target, output)
        proxy = UDPProxy(target)
        self.proxies.append(proxy)
        sender = self.send('hyb_send', '127.0.0.1', target, source,
                           '127.0.0.1:%d@20' % proxy.port)
        receiver = self.receive(r, source, output)
        self.assertGreater(len(proxy.dropped), 0)
        self.assertIn('-> resend', sender)
        result = dict(item.split('=', 1) for item in receiver.splitlines()[-1].split()[1:])
        self.assertGreater(int(result['dup']), 0)
        self.assertGreater(int(result['nacked']), 0)

    def test_hybrid_stops_at_retry_limit(self):
        target = free_port()
        source = self.source(1465)
        output = self.directory / 'incomplete.dat'
        output.write_bytes(b'previous complete file')
        r = self.launch('hyb_recv', '-w', 10, target, output)
        proxy = UDPProxy(target, discard_all=True)
        self.proxies.append(proxy)
        log = self.send('hyb_send', '-m', 1, '127.0.0.1', target, source,
                        '127.0.0.1:%d@20' % proxy.port, success=False)
        self.assertIn('too many rounds', log)
        receiver, _ = r.communicate(timeout=3)
        self.assertNotEqual(r.returncode, 0, receiver.decode())
        self.assertEqual(output.read_bytes(), b'previous complete file')
        self.assertFalse(list(self.directory.glob('*.part.*')))

    def test_invalid_rate_and_overflow_are_rejected(self):
        source = self.source(1)
        for rate in ('nan', 'inf', '-1', '0', 'nonsense'):
            with self.subTest(rate=rate):
                self.send('hyb_send', '127.0.0.1', 1, source,
                          '127.0.0.1:1@' + rate, success=False)
        self.send('tcp_send', '-b', '9223372036854775807M', '127.0.0.1', 1, source, success=False)
        for rounds in ('-1', '2junk', '4294967296'):
            self.send('hyb_send', '-m', rounds, '127.0.0.1', 1, source,
                      '127.0.0.1:1@10', success=False)


if __name__ == '__main__':
    unittest.main(verbosity=2)

import hashlib
import json
import os
from pathlib import Path
import socket
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


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


class RangePeer:
    def __init__(self, data, delay=0, broken=False):
        self.data, self.delay, self.broken = data, delay, broken
        self.socket = socket.socket()
        self.socket.bind(('127.0.0.1', 0))
        self.socket.listen()
        self.socket.settimeout(0.1)
        self.port = self.socket.getsockname()[1]
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stopped.is_set():
            try:
                sock, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.serve, args=(sock,), daemon=True).start()

    def serve(self, sock):
        try:
            with sock:
                header = f'SIZE {len(self.data)}\n'.encode()
                sock.sendall(header[:3])
                time.sleep(0.002)
                sock.sendall(header[3:])
                with sock.makefile('rb') as requests:
                    for line in requests:
                        _, offset, length = line.split()
                        offset, length = int(offset), int(length)
                        if self.broken:
                            sock.sendall(self.data[offset:offset + min(length, 32768)])
                            return
                        for position in range(offset, offset + length, 16384):
                            if self.delay:
                                time.sleep(self.delay)
                            sock.sendall(self.data[position:min(position + 16384, offset + length)])
        except (OSError, ValueError):
            pass

    def close(self):
        self.stopped.set()
        self.socket.close()
        self.thread.join(timeout=1)


class TransferTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.processes, self.peers = [], []

    def tearDown(self):
        for peer in self.peers:
            peer.close()
        for process in reversed(self.processes):
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.temp.cleanup()

    def launch(self, name, *args):
        process = subprocess.Popen([str(HERE / name), *map(str, args)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.processes.append(process)
        port = int(args[-1] if name == 'mp_server.out' else args[0])
        for _ in range(100):
            if process.poll() is not None:
                self.fail(process.stderr.read().decode())
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                    return port
            except OSError:
                time.sleep(0.01)
        self.fail(f'{name} did not start')

    def client(self, output, paths, *options, success=True):
        command = [str(HERE / 'mp_client.out'), str(output), *map(str, options),
                   *[f'127.0.0.1:{port}' for port in paths]]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                universal_newlines=True, timeout=20)
        if not success:
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())
            self.assertFalse(list(output.parent.glob(output.name + '.part.*')))
            return
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(next(line[7:] for line in result.stdout.splitlines()
                               if line.startswith('RESULT ')))

    def test_binary_empty_and_unaligned_sizes(self):
        for index, size in enumerate((0, 1, 65537, 5 * 1024 * 1024 + 17)):
            with self.subTest(size=size):
                source = self.directory / f'input-{index}'
                source.write_bytes(os.urandom(size))
                port = self.launch('mp_server.out', source, free_port())
                output = self.directory / f'output-{index}'
                result = self.client(output, [port], '--lanes', 4, '--chunk', 65536)
                self.assertEqual(result['receive_bytes'], size)
                self.assertEqual(digest(source), digest(output))

    def test_fragmented_requests_and_invalid_ranges(self):
        data = os.urandom(131073)
        source = self.directory / 'input'
        source.write_bytes(data)
        port = self.launch('mp_server.out', source, free_port())
        with socket.create_connection(('127.0.0.1', port)) as sock:
            with sock.makefile('rb') as stream:
                self.assertEqual(stream.readline(), b'SIZE 131073\n')
                sock.sendall(b'G')
                sock.sendall(b'ET 257 ')
                sock.sendall(b'70001\nGET 0 13\n')
                self.assertEqual(stream.read(70014), data[257:70258] + data[:13])
        for request in (b'GET 131073 1\n', b'GET 0 18446744073709551615\n', b'BAD\n'):
            with socket.create_connection(('127.0.0.1', port), timeout=2) as sock:
                with sock.makefile('rb') as stream:
                    stream.readline()
                    sock.sendall(request)
                    self.assertEqual(stream.read(1), b'')
        self.assertIsNone(self.processes[0].poll())

    def test_two_relays_and_multiple_paths(self):
        source = self.directory / 'input'
        source.write_bytes(os.urandom(12 * 1024 * 1024 + 37))
        server = self.launch('mp_server.out', source, free_port())
        first = self.launch('mp_relay.out', free_port(), '127.0.0.1', server)
        second = self.launch('mp_relay.out', free_port(), '127.0.0.1', first)
        output = self.directory / 'output'
        self.client(output, [server, first, second], '--lanes', 3, '--chunk', 65536)
        self.assertEqual(digest(source), digest(output))

    def test_failed_path_is_reassigned(self):
        data = os.urandom(8 * 1024 * 1024 + 13)
        broken = RangePeer(data, broken=True)
        healthy = RangePeer(data, delay=0.0005)
        self.peers += [broken, healthy]
        output = self.directory / 'output'
        result = self.client(output, [broken.port, healthy.port], '--lanes', 1,
                             '--pipeline', 2, '--no-hedge')
        self.assertGreater(result['retries'], 0)
        self.assertEqual(hashlib.sha256(data).hexdigest(), digest(output))

    def test_slow_tail_can_finish_on_fast_path(self):
        data = os.urandom(16 * 1024 * 1024)
        slow = RangePeer(data, delay=0.02)
        fast = RangePeer(data, delay=0.0002)
        self.peers += [slow, fast]
        output = self.directory / 'output'
        result = self.client(output, [slow.port, fast.port], '--lanes', 1, '--pipeline', 1)
        self.assertGreater(result['hedges'], 0)
        self.assertEqual(hashlib.sha256(data).hexdigest(), digest(output))

    def test_complete_failure_leaves_no_output(self):
        data = b'x' * (1024 * 1024)
        broken = RangePeer(data, broken=True)
        self.peers.append(broken)
        self.client(self.directory / 'output', [broken.port], '--lanes', 1, success=False)

    def test_prefix_probe_and_no_clobber(self):
        source = self.directory / 'input'
        source.write_bytes(os.urandom(1024 * 1024))
        port = self.launch('mp_server.out', source, free_port())
        output = self.directory / 'prefix'
        result = self.client(output, [port], '--bytes', 70003, '--lanes', 2)
        self.assertEqual(result['receive_bytes'], 70003)
        self.assertEqual(output.read_bytes(), source.read_bytes()[:70003])
        before = digest(output)
        rejected = subprocess.run([str(HERE / 'mp_client.out'), str(output),
                                   f'127.0.0.1:{port}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertEqual(before, digest(output))


if __name__ == '__main__':
    unittest.main(verbosity=2)

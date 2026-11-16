"""Ubuntu用。実TCP、途中切断、無応答、保存エラーを確認する回帰テスト。"""
import os
from pathlib import Path
import signal
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


class Peer:
    def __init__(self, data, mode='normal', delay=0):
        self.data, self.mode, self.delay = data, mode, delay
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen(16)
        self.sock.settimeout(.1)
        self.port = self.sock.getsockname()[1]
        self.stop = threading.Event()
        self.connections = 0
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self.serve, args=(conn, self.connections), daemon=True).start()

    def serve(self, conn, connection):
        try:
            with conn:
                conn.settimeout(2)
                with conn.makefile('rb') as stream:
                    for line in stream:
                        args = line.split()
                        if self.mode == 'silent_size':
                            self.stop.wait(5)
                            return
                        if args == [b'SIZE']:
                            size = -1 if self.mode == 'negative_size' else len(self.data)
                            conn.sendall(('SIZE %d\n' % size).encode())
                        elif len(args) == 3 and args[0] == b'GET':
                            off, length = map(int, args[1:])
                            if self.mode == 'broken' or (self.mode == 'broken_except_second' and connection != 2):
                                conn.sendall(self.data[off:off + min(100, length)])
                                return
                            if self.mode == 'silent_get':
                                self.stop.wait(5)
                                return
                            for pos in range(off, off + length, 8192):
                                if self.delay:
                                    time.sleep(self.delay)
                                conn.sendall(self.data[pos:min(pos + 8192, off + length)])
                        else:
                            return
        except OSError:
            pass

    def close(self):
        self.stop.set()
        self.sock.close()
        self.thread.join(1)


class TransferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.check_call(['make', '-s'], cwd=str(HERE))

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='route-test-')
        self.root = Path(self.temp.name)
        self.processes, self.peers = [], []
        self.data = os.urandom(262161)
        self.source = self.root / 'source'
        self.source.write_bytes(self.data)
        self.output = self.root / 'received'
        self.output.write_bytes(b'previous complete file')

    def tearDown(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
        for peer in self.peers:
            peer.close()
        self.temp.cleanup()

    def peer(self, **kwargs):
        peer = Peer(self.data, **kwargs)
        self.peers.append(peer)
        return '127.0.0.1:%d@10=test' % peer.port

    def launch(self, name, args, port):
        process = subprocess.Popen([str(HERE / name)] + list(map(str, args)),
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   start_new_session=True)
        self.processes.append(process)
        for _ in range(100):
            self.assertIsNone(process.poll(), name + ' exited')
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.1):
                    return process
            except OSError:
                time.sleep(.01)
        self.fail(name + ' not ready')

    def client(self, paths, options=(), env=None):
        command = [str(HERE / 'fclient.out'), '-o', str(self.output), '-c', '64', '-d', '2', '-t', '1']
        return subprocess.run(command + list(options) + paths, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=12, env=env)

    def assert_preserved(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(self.output.read_bytes(), b'previous complete file')
        self.assertEqual(list(self.root.glob('received.part.*')), [])

    def test_direct_and_two_relays_reassemble(self):
        port, p1, p2 = free_port(), free_port(), free_port()
        self.launch('fserver.out', [self.source, port], port)
        self.launch('relay.out', [p1, '127.0.0.1', port], p1)
        self.launch('relay.out', [p2, '127.0.0.1', p1], p2)
        result = self.client(['127.0.0.1:%d@10=direct' % port,
                              '127.0.0.1:%d@40=relay' % p2], ['-S'])
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(self.output.read_bytes(), self.data)
        self.assertFalse(list(self.root.glob('received.part.*')))

    def test_empty_file(self):
        self.data = b''
        result = self.client([self.peer()], ['-S'])
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(self.output.read_bytes(), b'')

    def test_partial_disconnect_stops_and_preserves_previous_output(self):
        result = self.client([self.peer(mode='broken')], ['-S'])
        self.assert_preserved(result)
        self.assertIn(b'5 attempts', result.stdout)

    def test_silent_data_peer_has_finite_retries(self):
        self.assert_preserved(self.client([self.peer(mode='silent_get')], ['-S']))

    def test_silent_metadata_does_not_truncate_output(self):
        self.assert_preserved(self.client([self.peer(mode='silent_size')], ['-S']))

    def test_negative_metadata_is_rejected(self):
        self.assert_preserved(self.client([self.peer(mode='negative_size')], ['-S']))

    def test_first_path_unreachable_falls_back(self):
        result = self.client(['127.0.0.1:%d=unreachable' % free_port(), self.peer()], ['-S'])
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(self.output.read_bytes(), self.data)

    def test_broken_path_returns_work_to_healthy_path(self):
        result = self.client([self.peer(mode='broken'), self.peer()], ['-S'])
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(self.output.read_bytes(), self.data)

    def test_failed_connection_keeps_healthy_connection_on_same_path(self):
        self.data = os.urandom(1024 * 1024 + 17)
        peer = self.peer(mode='broken_except_second', delay=.04)
        peer = peer.replace('@10', 'x2@10')
        result = self.client([peer], ['-S'])
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn(b'5 attempts', result.stdout)
        self.assertEqual(self.output.read_bytes(), self.data)

    def test_adaptive_standby_promotion(self):
        self.data = os.urandom(2 * 1024 * 1024 + 17)
        slow, fast = self.peer(delay=.05), self.peer()
        result = self.client([slow], ['-s', fast])
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn(b'standby -> active', result.stdout)
        self.assertEqual(self.output.read_bytes(), self.data)

    def test_signal_cleans_up_without_publishing_partial_file(self):
        path = self.peer(mode='silent_get')
        process = subprocess.Popen([str(HERE / 'fclient.out'), '-S', '-o', str(self.output), path],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                   start_new_session=True)
        self.processes.append(process)
        for _ in range(100):
            if list(self.root.glob('received.part.*')):
                break
            time.sleep(.01)
        self.assertTrue(list(self.root.glob('received.part.*')))
        process.terminate()
        self.assertNotEqual(process.wait(timeout=3), 0)
        self.assertEqual(self.output.read_bytes(), b'previous complete file')
        self.assertFalse(list(self.root.glob('received.part.*')))

    def test_partial_pwrite_and_eintr_are_retried(self):
        shim = self.root / 'short.c'
        shim.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    static int calls;
    ssize_t (*real_write)(int,const void*,size_t,off_t) = dlsym(RTLD_NEXT,"pwrite");
    if (__sync_fetch_and_add(&calls,1) == 0) { errno=EINTR; return -1; }
    return real_write(fd,buf,n > 17 ? 17 : n,off);
}
''')
        library = self.root / 'short.so'
        subprocess.check_call(['gcc', '-shared', '-fPIC', str(shim), '-o', str(library), '-ldl'])
        env = dict(os.environ, LD_PRELOAD=str(library))
        result = self.client([self.peer()], ['-S'], env)
        self.assertEqual(result.returncode, 0, result.stdout.decode())
        self.assertEqual(self.output.read_bytes(), self.data)

    def test_disk_write_failure_preserves_previous_output(self):
        shim = self.root / 'full.c'
        shim.write_text('''#include <errno.h>
#include <unistd.h>
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    (void)fd; (void)buf; (void)n; (void)off; errno=ENOSPC; return -1;
}
''')
        library = self.root / 'full.so'
        subprocess.check_call(['gcc', '-shared', '-fPIC', str(shim), '-o', str(library)])
        result = self.client([self.peer()], ['-S'], dict(os.environ, LD_PRELOAD=str(library)))
        self.assert_preserved(result)
        self.assertIn(b'pwrite', result.stdout)

    def test_publish_failure_leaves_existing_directory(self):
        self.output.unlink()
        self.output.mkdir()
        marker = self.output / 'keep'
        marker.write_text('keep')
        result = self.client([self.peer()], ['-S'])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(marker.read_text(), 'keep')
        self.assertFalse(list(self.root.glob('received.part.*')))

    def test_invalid_options_and_path_ports(self):
        for options, path in [(['-c', '99999999999999999999'], '127.0.0.1:1'),
                              (['-m', 'nan'], '127.0.0.1:1'), (['-t', '0'], '127.0.0.1:1'),
                              ([], '127.0.0.1:65536'), ([], '127.0.0.1:1junk'),
                              ([], '127.0.0.1:1x9999999999999')]:
            with self.subTest(options=options, path=path):
                self.assert_preserved(self.client([path], options))

    def test_server_rejects_overflow_and_invalid_commands(self):
        port = free_port()
        self.launch('fserver.out', [self.source, port], port)
        for command in [b'GET 9223372036854775800 100\n', b'GET -1 10\n',
                        b'GET 0 10 junk\n', b'NOPE\n', b'x' * 200 + b'\n']:
            with socket.create_connection(('127.0.0.1', port), timeout=2) as sock:
                sock.sendall(command)
                try:
                    self.assertEqual(sock.recv(10), b'')
                except ConnectionResetError:
                    pass


if __name__ == '__main__':
    unittest.main()

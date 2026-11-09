#!/usr/bin/env python3
"""実験4-3の条件をlocalhostで比較し、内容一致確認とCSV保存を行う。"""
import argparse
import csv
import hashlib
import re
import select
import subprocess
from datetime import datetime
from pathlib import Path


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            value.update(block)
    return value.hexdigest()


def metrics(text):
    lines = [line for line in text.splitlines() if line.startswith('RESULT ')]
    if len(lines) != 1:
        raise RuntimeError('RESULT line missing or duplicated: ' + text)
    result = dict(item.split('=', 1) for item in lines[0].split()[1:])
    if result.get('status') != 'ok':
        raise RuntimeError(lines[0])
    return result


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=here / 'test.dat')
    parser.add_argument('--bin-dir', type=Path, default=here)
    parser.add_argument('--results', type=Path)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    if args.repeats < 1 or args.timeout <= 0:
        parser.error('repeats and timeout must be positive')
    source = args.input.resolve()
    expected_hash = digest(source)
    directory = args.results or here / 'results' / datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    directory = directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    # Cでは送信サイズだけ、Dでは受信待ち時間だけを基準条件から変更する。
    cases = [('baseline', 1000, 0), ('small', 100, 0),
             ('tiny', 10, 0), ('slow_receive', 1000, 1000)]
    fields = ['case', 'repeat', 'chunk_bytes', 'wait_us', 'receive_buffer',
              'payload_bytes', 'seconds', 'goodput_bps', 'goodput_mbps',
              'file_reads', 'send_calls', 'receive_calls', 'server_seconds', 'sha256']
    with (directory / 'results.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for name, chunk, wait in cases:
            for repeat in range(1, args.repeats + 1):
                prefix = directory / f'{name}-{repeat}'
                output = prefix.with_suffix('.dat')
                if output == source:
                    raise RuntimeError('input and output must differ')
                server = subprocess.Popen(
                    [str(args.bin_dir.resolve() / 'fserver.out'), str(source), '0', str(chunk), '1'],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
                try:
                    ready, _, _ = select.select([server.stdout], [], [], args.timeout)
                    if not ready:
                        raise RuntimeError('server startup timed out')
                    first = server.stdout.readline()
                    found = re.search(r'^LISTEN port=(\d+) ', first)
                    if not found:
                        raise RuntimeError('server did not start: ' + first)
                    client = subprocess.run(
                        [str(args.bin_dir.resolve() / 'fclient.out'), 'localhost', found[1],
                         str(output), str(wait), '1000'],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        universal_newlines=True, timeout=args.timeout)
                    server_out, server_err = server.communicate(timeout=args.timeout)
                    prefix.with_suffix('.server.log').write_text(first + server_out + server_err)
                    prefix.with_suffix('.client.log').write_text(client.stdout + client.stderr)
                    if client.returncode or server.returncode:
                        raise RuntimeError('transfer failed; see logs at ' + str(prefix))
                    if digest(output) != expected_hash:
                        raise RuntimeError('file contents differ: ' + str(output))
                    received = metrics(client.stdout)
                    sent = metrics(server_out)
                    if int(received['payload_bytes']) != source.stat().st_size:
                        raise RuntimeError('received size differs from input')
                    row = dict(case=name, repeat=repeat, chunk_bytes=chunk,
                               sha256=expected_hash)
                    for key in ('wait_us', 'receive_buffer', 'payload_bytes', 'seconds',
                                'goodput_bps', 'goodput_mbps', 'receive_calls'):
                        row[key] = received[key]
                    for key in ('file_reads', 'send_calls', 'server_seconds'):
                        row[key] = sent[key]
                    writer.writerow(row)
                    stream.flush()
                    print(f'{name} #{repeat}: {received["seconds"]} s, '
                          f'{received["goodput_mbps"]} Mbps, '
                          f'write={sent["send_calls"]}, read={received["receive_calls"]}, SHA-256一致',
                          flush=True)
                finally:
                    if server.poll() is None:
                        server.terminate()
                        try:
                            server.communicate(timeout=2)
                        except subprocess.TimeoutExpired:
                            server.kill()
                            server.communicate()
    print('CSV: ' + str(directory / 'results.csv'))


if __name__ == '__main__':
    main()

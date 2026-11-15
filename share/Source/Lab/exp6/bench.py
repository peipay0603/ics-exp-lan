#!/usr/bin/env python3
"""ホストPC用の転送測定。Python 3.6対応。失敗を平均に含めず、ログを保存する。"""
import csv
import hashlib
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent
SHARE = HERE.parents[2]
REMOTE = '/home/ubuntu/share/Source/Lab/exp6'
FIELDS = ('datetime mode dest param bytes run time_s goodput_mbps send_cpu_s recv_cpu_s '
          'rounds retrans cmp label status sender_exit receiver_exit relay_buf_len logs').split()


def docker(node, *args):
    return subprocess.run(['docker', 'exec', node] + list(map(str, args)),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)


def positive_env(name, default):
    value = int(os.environ.get(name, default))
    if value < 1:
        raise ValueError(name + ' must be positive')
    return value


class Job:
    """固有のPIDファイルを使い、この測定で起動したプロセスだけを終了する。"""
    def __init__(self, node, command, log, seconds):
        self.node = node
        self.token = uuid.uuid4().hex
        self.pidfile = '/tmp/exp6-job-' + self.token + '.pid'
        self.log = log
        self.stream = log.open('wb')
        launcher = 'printf "%s\\n" "$$" > "$1"; shift; exec "$@"'
        args = ['docker', 'exec', node, 'sh', '-c', launcher, 'exp6-job', self.pidfile,
                'timeout', '--kill-after=5s', str(seconds), 'env',
                'EXP6_JOB_ID=' + self.token] + list(map(str, command))
        self.process = subprocess.Popen(args, stdout=self.stream, stderr=subprocess.STDOUT)

    def ready(self, prefix):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            lines = self.log.read_text(errors='replace').splitlines()
            if any(line.startswith(prefix) for line in lines) and self.process.poll() is None:
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.05)
        raise RuntimeError('did not become ready: ' + str(self.log))

    def wait(self, seconds):
        try:
            return self.process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            self.close()
            return 124

    def close(self):
        try:
            # docker exec側だけが終了しても、コンテナ内のtimeoutが残る場合がある。
            # PIDに加えargvの固有トークンも確認し、再利用されたPIDを止めない。
            script = ('if [ -r "$1" ]; then read p < "$1"; '
                      'case "$p" in ""|*[!0-9]*) exit 1;; esac; '
                      'if [ -r "/proc/$p/cmdline" ] && '
                      'grep -zFxq -- "EXP6_JOB_ID=$2" "/proc/$p/cmdline"; then '
                      'kill -TERM "$p" 2>/dev/null || :; fi; fi')
            docker(self.node, 'sh', '-c', script, 'exp6-stop', self.pidfile, self.token)
            if self.process.poll() is None:
                try:
                    self.process.wait(timeout=7)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            docker(self.node, 'rm', '-f', '--', self.pidfile)
        finally:
            self.stream.close()


def result(log, program):
    lines = [line for line in log.read_text(errors='replace').splitlines() if line.startswith('RESULT ')]
    if len(lines) != 1:
        raise ValueError('expected one RESULT line in ' + str(log))
    values = dict(item.split('=', 1) for item in lines[0].split()[1:])
    if values.get('prog') != program:
        raise ValueError('unexpected RESULT program in ' + str(log))
    return values


def append_row(path, row):
    path.parent.mkdir(parents=True, exist_ok=True)
    exists = path.exists() and path.stat().st_size > 0
    if exists:
        with path.open(newline='') as stream:
            if next(csv.reader(stream)) != FIELDS:
                raise ValueError('CSV schema differs; choose a new CSV path: ' + str(path))
    with path.open('a', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, lineterminator='\n')
        if not exists:
            writer.writeheader()
        writer.writerow(row)


def snapshot(directory, check_tc2=False):
    metadata = {'recorded_at': datetime.now().astimezone().isoformat(), 'nodes': {}}
    links = {
        'node1': [(20, 1, 5), (21, 10, 2.5), (22, 10, 0), (23, 50, 10)],
        'node2': [(20, 1, 5), (24, 10, 0), (25, 1, 5), (26, 10, 0)],
        'node3': [(21, 10, 2.5), (24, 10, 0), (27, 50, 10), (28, 1, 5)],
        'node4': [(22, 10, 0), (25, 1, 5), (27, 50, 10), (29, 50, 10)],
        'node5': [(23, 50, 10), (26, 10, 0), (28, 1, 5), (29, 50, 10)],
    }
    problems = []
    for node, expected in links.items():
        ips, qdisc = docker(node, 'ip', '-o', '-4', 'addr', 'show'), docker(node, 'tc', 'qdisc', 'show')
        if ips.returncode or qdisc.returncode:
            raise RuntimeError('cannot read network settings: ' + node)
        addresses, queues = ips.stdout.decode(), qdisc.stdout.decode()
        metadata['nodes'][node] = {'addresses': addresses, 'qdisc': queues}
        if check_tc2:
            for subnet, rate, delay in expected:
                address = '172.%d.0.%d0' % (subnet, int(node[-1]))
                interface = next((line.split()[1].split('@')[0] for line in addresses.splitlines()
                                  if 'inet ' + address + '/' in line), None)
                lines = [line for line in queues.splitlines() if 'dev ' + str(interface) + ' ' in line]
                rates = [float(m.group(1)) * {'': 0.000001, 'K': 0.001, 'M': 1, 'G': 1000}[m.group(2)]
                         for line in lines for m in [re.search(r'rate ([0-9.]+)([KMG]?)bit', line)] if m]
                delays = [float(m.group(1)) * {'us': 0.001, 'ms': 1, 's': 1000}[m.group(2)]
                          for line in lines for m in [re.search(r'delay ([0-9.]+)(us|ms|s)', line)] if m]
                if rates != [rate] or (delays or [0]) != [delay]:
                    problems.append('%s %s expected=%sMbps/%sms actual=%s/%s' %
                                    (node, address, rate, delay, rates, delays))
    git = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=str(HERE), stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL)
    metadata.update(git_head=git.stdout.decode().strip(), tc2_verified=check_tc2 and not problems,
                    profile_errors=problems)
    metadata['source_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in HERE.iterdir() if p.suffix in ('.c', '.h', '.py', '.sh')
                                 or p.name == 'Makefile'}
    (directory / 'environment.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2))
    if problems:
        raise ValueError('tc2 does not match. Run make tc2 at the repository root.\n' + '\n'.join(problems))


def run_condition(mode, destination, params, directory, csv_path, label='', relay_buf=''):
    repeat, timeout, port = positive_env('REPEAT', 3), positive_env('TIMEOUT', 900), positive_env('PORT', 20000)
    if port > 65533:
        raise ValueError('PORT must be <= 65533')
    source = (HERE / os.environ.get('FILE', 'test100M.dat')).resolve()
    remote_source = '/home/ubuntu/share/' + str(source.relative_to(SHARE))
    expected = source.stat().st_size
    if expected == 0:
        raise ValueError('benchmark file must not be empty')
    minimum = float(os.environ.get('MIN_SECONDS', '0'))
    if not math.isfinite(minimum) or minimum < 0:
        raise ValueError('MIN_SECONDS must be finite and nonnegative')
    send_options, recv_options = shlex.split(os.environ.get('SEND_OPTS', '')), shlex.split(os.environ.get('RECV_OPTS', ''))
    rows = []
    for param in params:
        for iteration in range(1, repeat + 1):
            attempt = directory / ('run-' + uuid.uuid4().hex[:12])
            attempt.mkdir()
            output = '/tmp/exp6-recv-' + uuid.uuid4().hex + '.dat'
            receiver = sender = None
            row = dict.fromkeys(FIELDS, '')
            row.update(datetime=datetime.now().astimezone().isoformat(), mode=mode, dest=destination,
                       param=param, run=iteration, label=label, status='ERROR', cmp='NOT_RUN',
                       relay_buf_len=relay_buf, logs=str(attempt.relative_to(HERE)))
            try:
                if mode == 'tcp':
                    host, sep, dest_port = destination.partition(':')
                    send_args = ['-b', param] + send_options + [host, dest_port if sep else str(port), remote_source]
                    recv_args = ['-b', param] + recv_options + [str(port), output]
                else:
                    send_args = send_options + ['-c', param, os.environ.get('CTRL', '172.21.0.10'),
                                                str(port), remote_source] + shlex.split(destination)
                    recv_args = recv_options + [str(port), output]
                receiver = Job('node1', [REMOTE + '/' + mode + '_recv.out'] + recv_args,
                               attempt / 'receiver.log', timeout)
                receiver.ready('waiting connection')
                sender = Job('node3', [REMOTE + '/' + mode + '_send.out'] + send_args,
                             attempt / 'sender.log', timeout)
                row['sender_exit'] = sender.wait(timeout + 10)
                if row['sender_exit'] != 0:
                    raise RuntimeError('sender failed: ' + str(row['sender_exit']))
                row['receiver_exit'] = receiver.wait(timeout + 10)
                if row['receiver_exit'] != 0:
                    raise RuntimeError('receiver failed: ' + str(row['receiver_exit']))
                sent, received = result(attempt / 'sender.log', mode + '_send'), result(attempt / 'receiver.log', mode + '_recv')
                if int(sent['bytes']) != expected or int(received['bytes']) != expected:
                    raise ValueError('RESULT byte count differs from source size')
                duration, goodput = float(received['time']), float(received['goodput_mbps'])
                if not all(math.isfinite(v) and v > 0 for v in (duration, goodput)):
                    raise ValueError('invalid measurement')
                comparison = docker('node1', 'cmp', '-s', '--', remote_source, output)
                row['cmp'] = 'OK' if comparison.returncode == 0 else 'NG'
                if row['cmp'] != 'OK':
                    raise ValueError('received contents differ from source')
                row.update(bytes=expected, time_s=duration, goodput_mbps=goodput,
                           send_cpu_s=sent['cpu'], recv_cpu_s=received['cpu'],
                           rounds=sent.get('rounds', ''), retrans=sent.get('retrans', ''))
                if duration < minimum:
                    row['status'] = 'TOO_SHORT'
                    raise ValueError('transfer shorter than MIN_SECONDS=' + str(minimum))
                row['status'] = 'OK'
            except (OSError, KeyError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                (attempt / 'error.txt').write_text(str(exc) + '\n')
                print('FAILED: %s (%s)' % (exc, attempt), file=sys.stderr, flush=True)
            finally:
                for job in (sender, receiver):
                    if job:
                        job.close()
                if receiver:
                    row['receiver_exit'] = receiver.process.returncode
                docker('node1', 'rm', '-f', '--', output)
            append_row(csv_path, row)
            rows.append(row)
            print('%s %s param=%s run=%d time=%ss goodput=%sMbps cmp=%s status=%s' %
                  (label or mode, destination, param, iteration, row['time_s'], row['goodput_mbps'],
                   row['cmp'], row['status']), flush=True)
    return rows


def suite(cases, directory, csv_path):
    port, timeout, repeat = positive_env('PORT', 20000), positive_env('TIMEOUT', 900), positive_env('REPEAT', 3)
    definitions = {
        'A': ('tcp', '172.21.0.10', '1000', ''), 'B': ('tcp', '172.27.0.40', '1000', '1000'),
        'C': ('tcp', '172.21.0.10', '64K', ''), 'D': ('tcp', '172.27.0.40', '64K', '64K'),
        'H': ('hyb', '172.27.0.40:%d@50' % port, '1464', ''),
        'E': ('hyb', ('172.27.0.40:%d@40 172.21.0.10:%d@9.5 '
                      '172.27.0.40:%d@10 172.24.0.20:%d@9.5') % (port, port, port + 2, port), '1464', ''),
    }
    if any(case not in definitions for case in cases):
        raise ValueError('suite cases: A B C D H E')
    # ARP解決を転送開始前に済ませる。回線設定は変更しない。
    for node, ip in [('node3', '172.21.0.10'), ('node3', '172.27.0.40'), ('node4', '172.29.0.50'),
                     ('node5', '172.23.0.10'), ('node4', '172.22.0.10'),
                     ('node3', '172.24.0.20'), ('node2', '172.26.0.50')]:
        if docker(node, 'ping', '-c', 1, '-W', 2, ip).returncode:
            raise RuntimeError('warmup ping failed: ' + node + ' -> ' + ip)
    rows = []
    for case in cases:
        mode, dest, param, relay_buf = definitions[case]
        relays, commands = [], []
        if case in ('B', 'D', 'H', 'E'):
            binary = 'tcp_relay.out' if mode == 'tcp' else 'udp_relay.out'
            options = ['-b', relay_buf] if mode == 'tcp' else []
            commands = [('node5', options + [port, '172.23.0.10', port]),
                        ('node4', options + [port, '172.29.0.50', port])]
            if case == 'E':
                commands += [('node4', [port + 2, '172.22.0.10', port]), ('node2', [port, '172.26.0.50', port])]
        try:
            for index, (node, args) in enumerate(commands):
                job = Job(node, [REMOTE + '/' + binary] + args,
                          directory / ('%s-relay-%d.log' % (case, index)), (timeout + 20) * repeat + 60)
                relays.append(job)
                job.ready('tcp relay:' if mode == 'tcp' else 'udp relay:')
            rows += run_condition(mode, dest, [param], directory, csv_path, case, relay_buf)
        finally:
            for job in reversed(relays):
                job.close()
    return rows


def main(args):
    if not args or args[0] not in ('tcp', 'hyb', 'suite') or (args[0] != 'suite' and len(args) < 3):
        print('Usage: ./bench.sh tcp DEST BUF_LEN [BUF_LEN ...]\n'
              '       ./bench.sh hyb "IP:PORT@Mbps ..." CHUNK [CHUNK ...]\n'
              '       ./bench.sh suite [A B C D H E]\n'
              'Env: FILE, REPEAT, PORT, TIMEOUT, CSV, LABEL, MIN_SECONDS, SEND_OPTS, RECV_OPTS')
        return 2
    directory = HERE / 'results' / (datetime.now().strftime('%Y%m%d-%H%M%S-') + uuid.uuid4().hex[:8])
    directory.mkdir(parents=True)
    csv_path = Path(os.environ.get('CSV', str(directory / 'bench.csv'))).resolve()
    snapshot(directory, check_tc2=args[0] == 'suite' or os.environ.get('PROFILE') == 'tc2')
    if args[0] == 'suite':
        rows = suite(args[1:] or ['A', 'B', 'C', 'D', 'H', 'E'], directory, csv_path)
    else:
        rows = run_condition(args[0], args[1], args[2:], directory, csv_path, os.environ.get('LABEL', ''))
    groups = {}
    for row in rows:
        if row['status'] == 'OK':
            key = (row['label'] or row['mode'], row['param'])
            groups.setdefault(key, []).append(row)
    for (label, param), values in groups.items():
        print('AVERAGE %s param=%s n=%d goodput=%.3fMbps time=%.3fs' %
              (label, param, len(values), sum(r['goodput_mbps'] for r in values) / len(values),
               sum(r['time_s'] for r in values) / len(values)), flush=True)
    print('CSV: ' + str(csv_path), flush=True)
    print('Logs: ' + str(directory), flush=True)
    return 0 if rows and all(row['status'] == 'OK' for row in rows) else 1


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)

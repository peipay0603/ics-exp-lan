#!/usr/bin/env python3
"""ホストPC用。PDFのA〜EをUbuntu Dockerで測り、照合・環境・生ログを保存する。"""
import argparse
import csv
from datetime import datetime
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import random
import statistics
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
SHARE = HERE.parent
REMOTE = '/home/ubuntu/share/route'
spec = importlib.util.spec_from_file_location('exp6_bench', str(SHARE / 'Source/Lab/exp6/bench.py'))
support = importlib.util.module_from_spec(spec)
spec.loader.exec_module(support)
FIELDS = ('case repeat bytes seconds goodput_mbps status cmp exit min_seconds '
          'source_md5 received_md5 logs').split()
CASES = {'A': ('3-1', True), 'B': ('3-4-5-1', True),
         'C': ('3-1', False), 'D': ('3-4-5-1', False), 'E': (None, False)}


def remote(path):
    return '/home/ubuntu/share/' + str(Path(path).resolve().relative_to(SHARE))


def digest(path, algorithm='md5'):
    value = hashlib.new(algorithm)
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def command(node, *args):
    result = support.docker(node, *args)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors='replace'))
    return result.stdout.decode(errors='replace')


def snapshot(directory):
    support.snapshot(directory, check_tc2=True)
    path = directory / 'environment.json'
    metadata = json.loads(path.read_text())
    metadata['exp6_support_sha256'] = metadata.pop('source_sha256')
    metadata['source_sha256'] = {p.name: digest(p, 'sha256') for p in HERE.iterdir()
                                 if p.suffix in ('.c', '.h', '.py', '.sh') or p.name == 'Makefile'}
    metadata['baseline_sha256'] = {p.name: digest(p, 'sha256')
                                   for p in (SHARE / 'Source/Lab/exp43').iterdir()
                                   if p.suffix in ('.c', '.h')}
    metadata['runtime'] = command('node1', 'sh', '-c', 'uname -a; gcc --version | head -1; python3 --version')
    path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + '\n')


def metrics(log):
    lines = [line for line in Path(log).read_text(errors='replace').splitlines()
             if line.startswith('ROUTE_METRICS ')]
    if len(lines) != 1:
        raise ValueError('missing or duplicate timing record')
    value = json.loads(lines[0].split(' ', 1)[1])
    if value['exit'] != 0 or not math.isfinite(value['seconds']) or value['seconds'] <= 0:
        raise ValueError('invalid or unsuccessful timing record')
    return value


def summarize(rows):
    result = {}
    for label in sorted({row['case'] for row in rows}):
        found = [row for row in rows if row['case'] == label]
        good = [row for row in found if row['status'] == 'OK']
        item = {'success': len(good), 'failed': len(found) - len(good)}
        if good:
            if len({int(row['bytes']) for row in good}) != 1:
                raise ValueError('cannot summarize mixed file sizes')
            seconds = [float(row['seconds']) for row in good]
            item.update(median_s=statistics.median(seconds), min_s=min(seconds), max_s=max(seconds),
                        median_mbps=int(good[0]['bytes']) * 8 / statistics.median(seconds) / 1e6)
        result[label] = item
    return result


def plan_case(label, trial, source, output, port):
    path, baseline = CASES[label]
    args = ['python3', REMOTE + '/routeplan.py', '--links', remote(trial / 'links.txt'),
            '--outdir', remote(trial / 'plan'), '--file', remote(source), '--output', remote(output),
            '--standby-hops', '0', '--server-port', str(port), '--relay-port-base', str(port + 100)]
    if path:
        args += ['--path', path]
    (trial / 'plan.log').write_text(command('node1', *args))
    manifest = trial / 'plan/plan.json'
    plan = json.loads(manifest.read_text())
    if baseline:
        entry = plan['services']['3'][0]
        entry.update(command=['../Source/Lab/exp43/fserver.out', remote(source), str(port)],
                     ready='waiting connection')
        endpoint = plan['client'][-1].split('@')[0].split('=')[0]
        host, target_port = endpoint.split(':')
        client = [REMOTE + '/../Source/Lab/exp43/fclient.out', host, target_port, remote(output)]
    else:
        client = list(plan['client'])
        client[0] = REMOTE + '/fclient.out'
        if label != 'E':
            client.insert(1, '-S')
    plan['benchmark_client'] = client
    manifest.write_text(json.dumps(plan, ensure_ascii=False, indent=2) + '\n')
    return manifest, client


def run_trial(label, repeat, args, source, source_md5):
    trial = args.out / ('%s-%02d' % (label, repeat))
    trial.mkdir()
    snapshot(trial)
    links = []
    for node in range(1, 6):
        links.append(command('node%d' % node, 'bash', REMOTE + '/collect_links.sh'))
    # collect_links.shの表示から、コメントと実データだけをアーカイブする。
    (trial / 'links.txt').write_text(''.join(line + '\n' for block in links for line in block.splitlines()
                                          if line.startswith('#') or line[:1].isdigit()))
    output = trial / ('received-' + uuid.uuid4().hex + '.dat')
    manifest, client = plan_case(label, trial, source, output, args.port)
    row = dict.fromkeys(FIELDS, '')
    row.update(case=label, repeat=repeat, bytes=source.stat().st_size, status='ERROR', cmp='NOT_RUN',
               min_seconds=args.min_seconds, source_md5=source_md5, logs=str(trial.relative_to(args.out)))
    nodes, job = [], None
    try:
        plan = json.loads(manifest.read_text())
        for node in sorted(plan['services'], key=lambda n: (n != '3', n)):
            command('node' + node, 'python3', REMOTE + '/services.py', 'start', remote(manifest), '--node', node)
            nodes.append(node)
        job = support.Job('node1', ['python3', REMOTE + '/run_timed.py'] + client,
                          trial / 'client.log', args.timeout)
        row['exit'] = job.wait(args.timeout + 10)
        if row['exit']:
            raise RuntimeError('client failed: ' + str(row['exit']))
        timing = metrics(trial / 'client.log')
        row.update(seconds=timing['seconds'], goodput_mbps=row['bytes'] * 8 / timing['seconds'] / 1e6)
        row['cmp'] = 'OK' if support.docker('node1', 'cmp', '-s', '--', remote(source), remote(output)).returncode == 0 else 'NG'
        row['received_md5'] = digest(output)
        if row['cmp'] != 'OK' or row['received_md5'] != source_md5:
            raise ValueError('file contents differ')
        after = trial / 'after'
        after.mkdir()
        snapshot(after)
        if row['seconds'] < args.min_seconds:
            row['status'] = 'TOO_SHORT'
            raise ValueError('duration is below --min-seconds')
        row['status'] = 'OK'
    except (OSError, RuntimeError, ValueError) as exc:
        (trial / 'error.txt').write_text(str(exc) + '\n')
    finally:
        if job:
            job.close()
        for node in reversed(nodes):
            command('node' + node, 'python3', REMOTE + '/services.py', 'stop', remote(manifest), '--node', node)
        if output.exists():
            output.unlink()
        with (args.out / 'results.csv').open('a', newline='') as stream:
            csv.DictWriter(stream, FIELDS, lineterminator='\n').writerow(row)
    print(json.dumps(row, ensure_ascii=False), flush=True)
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cases', default='A,B,C,D,E')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--min-seconds', type=float, default=100)
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--port', type=int, default=31000)
    parser.add_argument('--seed', type=int, default=20261116)
    args = parser.parse_args()
    labels = args.cases.split(',')
    if (args.repeats < 1 or not labels or len(set(labels)) != len(labels) or any(c not in CASES for c in labels)
            or not math.isfinite(args.min_seconds) or args.min_seconds < 0 or args.timeout <= args.min_seconds
            or not 1024 <= args.port <= 65000):
        parser.error('invalid cases, repeats, timing, or port')
    args.out = args.out.resolve()
    source = args.file.resolve()
    if not source.is_file() or not source.stat().st_size:
        parser.error('--file must be a nonempty file')
    remote(source)
    remote(args.out)
    args.out.mkdir(parents=True, exist_ok=False)
    snapshot(args.out)
    command('node1', 'make', '-C', REMOTE)
    command('node1', 'make', '-C', REMOTE + '/../Source/Lab/exp43')
    (args.out / 'run.json').write_text(json.dumps({
        'started_at': datetime.now().astimezone().isoformat(), 'argv': sys.argv,
        'source': str(source.relative_to(SHARE)), 'bytes': source.stat().st_size,
        'source_sha256': digest(source, 'sha256'), 'timing': 'client process start through save and exit; excludes hash checking',
        'cases': labels, 'repeats': args.repeats, 'min_seconds': args.min_seconds}, ensure_ascii=False, indent=2) + '\n')
    with (args.out / 'results.csv').open('w', newline='') as stream:
        csv.DictWriter(stream, FIELDS, lineterminator='\n').writeheader()
    source_md5, rows = digest(source), []
    rng = random.Random(args.seed)
    for repeat in range(1, args.repeats + 1):
        order = list(labels)
        rng.shuffle(order)
        for label in order:
            print('START %s %d/%d' % (label, repeat, args.repeats), flush=True)
            rows.append(run_trial(label, repeat, args, source, source_md5))
            (args.out / 'summary.json').write_text(json.dumps(summarize(rows), ensure_ascii=False, indent=2) + '\n')
    return 0 if all(row['status'] == 'OK' for row in rows) else 1


if __name__ == '__main__':
    sys.exit(main())

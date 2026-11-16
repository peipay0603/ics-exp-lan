#!/usr/bin/env python3
"""Ubuntu上で、このplanが起動したサーバ・中継だけを管理する。"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

HERE = Path(__file__).resolve().parent
CHILDREN = {}


def identity(pid):
    try:
        # commには空白や括弧が含まれ得る。最後の')'より後だけを分割する。
        fields = Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()
        command = Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')[:-1]
        return {'start': fields[19], 'group': int(fields[2]),
                'command': [os.fsdecode(arg) for arg in command]}
    except (OSError, ValueError, IndexError):
        return None


def owned(record):
    current = identity(record['pid'])
    return current is not None and current == record['identity'] and current['group'] == record['pid']


def signal_owned(record, signum):
    if owned(record):
        try:
            os.killpg(record['pid'], signum)
        except ProcessLookupError:
            pass


def stop(records):
    for record in reversed(records):
        signal_owned(record, signal.SIGTERM)
    deadline = time.monotonic() + 3
    while any(owned(record) for record in records) and time.monotonic() < deadline:
        time.sleep(.05)
    for record in records:
        signal_owned(record, signal.SIGKILL)
        process = CHILDREN.get(record['pid'])
        if process is not None and process.poll() is not None:
            del CHILDREN[record['pid']]


def control(action, manifest, node):
    manifest = Path(manifest).resolve()
    plan = json.loads(manifest.read_text())
    state = manifest.parent / ('services-node%d.json' % node)
    records = json.loads(state.read_text()) if state.exists() else []
    if action == 'stop':
        stop(records)
        if state.exists():
            state.unlink()
        return []
    if action == 'status':
        return [{'pid': record['pid'], 'running': owned(record)} for record in records]
    if any(owned(record) for record in records):
        raise ValueError('this plan is already running; stop it before restarting')
    records = []
    logs = manifest.parent / 'logs'
    logs.mkdir(exist_ok=True)
    try:
        for index, entry in enumerate(plan['services'].get(str(node), [])):
            command = list(entry['command'])
            command[0] = str(HERE / command[0])
            log = logs / ('node%d-%d.log' % (node, index))
            with log.open('wb') as stream:
                process = subprocess.Popen(command, cwd=str(HERE), stdout=stream,
                                           stderr=subprocess.STDOUT, start_new_session=True)
            record = {'pid': process.pid, 'identity': identity(process.pid), 'log': str(log)}
            CHILDREN[process.pid] = process
            records.append(record)
            state.write_text(json.dumps(records, indent=2))
            os.chmod(str(state), 0o600)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('service exited: ' + log.read_text(errors='replace'))
                if any(line.startswith(entry['ready']) for line in log.read_text(errors='replace').splitlines()):
                    break
                time.sleep(.05)
            else:
                raise RuntimeError('service did not become ready: ' + str(log))
        return records
    except BaseException:
        stop(records)
        if state.exists():
            state.unlink()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('start', 'stop', 'status'))
    parser.add_argument('manifest')
    parser.add_argument('--node', type=int, default=None)
    args = parser.parse_args()
    hostname = socket.gethostname()
    if args.node is None and (not hostname.startswith('node') or not hostname[4:].isdigit()):
        parser.error('run on node1..node5, or specify --node')
    node = args.node if args.node is not None else int(hostname[4:])
    print(json.dumps(control(args.action, args.manifest, node), ensure_ascii=False))


if __name__ == '__main__':
    main()

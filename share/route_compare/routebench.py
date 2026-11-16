#!/usr/bin/env python3
"""Controlled TCP file-transfer comparison; Python 3.6+, standard library only."""
import argparse
import concurrent.futures
import csv
import datetime
import hashlib
import itertools
import json
import math
import os
import random
import socket
import socketserver
import statistics
import struct
import sys
import threading
import time

PROTOCOL = "routebench-1"
BLOCK = 1000
TIMEOUT = 30
ENDPOINTS = {
    "direct": ("172.21.0.30", 19000),
    "via2": ("172.20.0.20", 19001),
    "via4": ("172.22.0.40", 19001),
    "via5": ("172.23.0.50", 19001),
}
CASES = {"D": ["direct"]}
for count in (1, 2, 3):
    for nodes in itertools.combinations((2, 4, 5), count):
        CASES["D" + "".join(str(x) for x in nodes)] = ["direct"] + ["via" + str(x) for x in nodes]
FIELDS = ["time", "case", "paths", "path_count", "allocation", "weights", "bytes",
          "elapsed_sec", "goodput_mbps", "ok", "error", "output", "per_path"]


def recv_exact(sock, size):
    parts = []
    while size:
        data = sock.recv(min(size, 65536))
        if not data:
            raise IOError("connection closed before the requested data arrived")
        parts.append(data)
        size -= len(data)
    return b"".join(parts)


def send_json(sock, value):
    value = dict(value, protocol=PROTOCOL)
    data = json.dumps(value, separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack("!I", len(data)) + data)


def recv_json(sock):
    size = struct.unpack("!I", recv_exact(sock, 4))[0]
    if size < 2 or size > 16384:
        raise ValueError("invalid protocol header size")
    value = json.loads(recv_exact(sock, size).decode("utf-8"))
    if value.get("protocol") != PROTOCOL:
        raise ValueError("this port is not a compatible routebench service")
    return value


def connect(endpoint):
    sock = socket.create_connection(endpoint, timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    return sock


def query(endpoint, op):
    with connect(endpoint) as sock:
        send_json(sock, {"op": op})
        reply = recv_json(sock)
    if not reply.get("ok"):
        raise IOError(reply.get("error", "request failed"))
    return reply


class Service(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(TIMEOUT)
        replied = False
        try:
            req = recv_json(self.request)
            op = req.get("op")
            if op in ("status", "stop"):
                reply = {"ok": True, "role": self.server.role,
                         "file": self.server.source_file,
                         "upstream": self.server.upstream,
                         "block_size": self.server.block_size}
                send_json(self.request, reply)
                if op == "stop":
                    threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            if op not in ("info", "get"):
                raise ValueError("unknown operation")
            if self.server.role == "relay":
                with connect(self.server.upstream) as upstream:
                    send_json(upstream, req)
                    reply = recv_json(upstream)
                    send_json(self.request, reply)
                    replied = True
                    if op == "get" and reply.get("ok"):
                        remaining = int(reply["length"])
                        while remaining:
                            data = upstream.recv(min(self.server.block_size, remaining))
                            if not data:
                                raise IOError("upstream closed during transfer")
                            self.request.sendall(data)
                            remaining -= len(data)
                        self.request.sendall(recv_exact(upstream, 32))
                return
            with open(self.server.source_file, "rb", buffering=0) as source:
                size = os.fstat(source.fileno()).st_size
                if op == "info":
                    send_json(self.request, {"ok": True, "size": size,
                                             "source_file": self.server.source_file})
                    return
                offset, length = int(req["offset"]), int(req["length"])
                if offset < 0 or length <= 0 or offset + length > size:
                    raise ValueError("requested range is outside source file")
                send_json(self.request, {"ok": True, "offset": offset, "length": length})
                replied = True
                source.seek(offset)
                digest = hashlib.sha256()
                remaining = length
                while remaining:
                    data = source.read(min(self.server.block_size, remaining))
                    if not data:
                        raise IOError("source file changed or ended during transfer")
                    digest.update(data)
                    self.request.sendall(data)
                    remaining -= len(data)
                self.request.sendall(digest.digest())
        except Exception as exc:
            if not replied:
                try:
                    send_json(self.request, {"ok": False, "error": str(exc)})
                except OSError:
                    pass
            print("service error: " + str(exc), file=sys.stderr, flush=True)


def run_service(args):
    if args.role == "source" and not os.path.isfile(args.file):
        raise ValueError("source file does not exist: " + args.file)
    with Service((args.host, args.port), Handler) as server:
        server.role = args.role
        server.source_file = os.path.realpath(args.file) if args.role == "source" else None
        server.upstream = parse_endpoint(args.upstream) if args.role == "relay" else None
        server.block_size = args.block_size
        print("READY " + json.dumps({"role": server.role, "port": args.port,
                                    "file": server.source_file, "upstream": server.upstream,
                                    "block_size": server.block_size}), flush=True)
        try:
            server.serve_forever(poll_interval=0.1)
        except KeyboardInterrupt:
            pass


def parse_endpoint(text):
    host, port = text.rsplit(":", 1)
    port = int(port)
    if not host or not 1 <= port <= 65535:
        raise ValueError("endpoint must be host:port")
    return host, port


def partition(total, paths, weights):
    if total < len(paths):
        raise ValueError("file range is too small for this number of paths")
    values = [float(weights[p]) for p in paths]
    if any(not math.isfinite(x) or x <= 0 for x in values) or not math.isfinite(sum(values)):
        raise ValueError("all path weights must be finite and positive")
    # Reserve one byte per path, then distribute the remaining bytes exactly.
    raw = [(total - len(paths)) * x / sum(values) for x in values]
    sizes = [1 + int(x) for x in raw]
    remainder = total - sum(sizes)
    order = sorted(range(len(paths)), key=lambda i: raw[i] - int(raw[i]), reverse=True)
    for i in order[:remainder]:
        sizes[i] += 1
    ranges, offset = [], 0
    for path, length in zip(paths, sizes):
        ranges.append((path, offset, length))
        offset += length
    assert offset == total
    return ranges


def receive_range(path, endpoint, offset, length, output, gate):
    gate.wait()
    started = time.monotonic()
    digest = hashlib.sha256()
    with connect(endpoint) as sock, open(output, "r+b", buffering=0) as target:
        send_json(sock, {"op": "get", "offset": offset, "length": length})
        reply = recv_json(sock)
        if not reply.get("ok"):
            raise IOError(reply.get("error", "source rejected request"))
        if reply.get("offset") != offset or reply.get("length") != length:
            raise IOError("response range does not match the request")
        target.seek(offset)
        remaining = length
        while remaining:
            data = sock.recv(min(BLOCK, remaining))
            if not data:
                raise IOError("file data was truncated")
            digest.update(data)
            view = memoryview(data)
            while view:
                written = target.write(view)
                if not written:
                    raise IOError("output write failed")
                view = view[written:]
            remaining -= len(data)
        if recv_exact(sock, 32) != digest.digest():
            raise IOError("SHA-256 range verification failed on " + path)
    elapsed = time.monotonic() - started
    return {"path": path, "offset": offset, "bytes": length,
            "elapsed_sec": elapsed, "mbps": length * 8 / elapsed / 1e6}


def transfer(paths, weights, endpoints, allocation, requested_bytes, output, label):
    started = time.monotonic()
    info = query(endpoints[paths[0]], "info")
    total = int(info["size"]) if requested_bytes is None else requested_bytes
    if total <= 0 or total > int(info["size"]):
        raise ValueError("--bytes must be positive and no larger than the source file")
    selected_weights = {p: 1.0 if allocation == "equal" else weights[p] for p in paths}
    ranges = partition(total, paths, selected_weights)
    output = os.path.abspath(output)
    if os.path.realpath(output) == info.get("source_file"):
        raise ValueError("output must not overwrite the shared source file")
    os.makedirs(os.path.dirname(output), exist_ok=True)
    # Output must be a separate file from the source, including on shared mounts.
    with open(output, "wb") as target:
        target.truncate(total)
    gate = threading.Barrier(len(paths) + 1)
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(paths)) as pool:
        futures = [pool.submit(receive_range, p, endpoints[p], off, size, output, gate)
                   for p, off, size in ranges]
        gate.wait()
        results = [future.result() for future in futures]
    elapsed = time.monotonic() - started
    if sum(row["bytes"] for row in results) != total or os.path.getsize(output) != total:
        raise IOError("received byte count or output size does not match")
    return {"time": datetime.datetime.now().isoformat(timespec="seconds"),
            "case": label, "paths": ",".join(paths), "path_count": len(paths),
            "allocation": allocation, "weights": json.dumps(selected_weights, sort_keys=True),
            "bytes": total, "elapsed_sec": elapsed, "goodput_mbps": total * 8 / elapsed / 1e6,
            "ok": True, "error": "", "output": output,
            "per_path": json.dumps(results, sort_keys=True)}


def append_csv(filename, row):
    os.makedirs(os.path.dirname(os.path.abspath(filename)), exist_ok=True)
    exists = os.path.exists(filename) and os.path.getsize(filename) > 0
    with open(filename, "a", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        if not exists:
            writer.writeheader()
        writer.writerow(row)


def attempt(args, paths, label, output, csv_file):
    try:
        row = transfer(paths, args.weights, args.endpoints, args.allocation,
                       args.bytes, output, label)
        print("{}: {} paths, {} bytes, {:.4f} sec, {:.4f} Mbps, SHA256 OK".format(
            label, len(paths), row["bytes"], row["elapsed_sec"], row["goodput_mbps"]), flush=True)
        for stat in json.loads(row["per_path"]):
            print("  {}: {} bytes / {:.4f} sec".format(
                stat["path"], stat["bytes"], stat["elapsed_sec"]), flush=True)
    except Exception as exc:
        row = {"time": datetime.datetime.now().isoformat(timespec="seconds"),
               "case": label, "paths": ",".join(paths), "path_count": len(paths),
               "allocation": args.allocation, "weights": json.dumps(args.weights, sort_keys=True),
               "bytes": args.bytes or 0, "elapsed_sec": "", "goodput_mbps": "",
               "ok": False, "error": str(exc), "output": os.path.abspath(output), "per_path": ""}
        print(label + ": FAILED: " + str(exc), file=sys.stderr, flush=True)
    if csv_file:
        append_csv(csv_file, row)
    return row


def summary_rows(rows, allocation=None):
    groups = {}
    for row in rows:
        if allocation and row["allocation"] != allocation:
            continue
        if row["case"] not in CASES:
            continue
        key = (row["allocation"], int(row["bytes"]), row["case"])
        groups.setdefault(key, []).append(row)
    summary = []
    for (kind, size, label), group in sorted(groups.items()):
        good = [r for r in group if str(r["ok"]).lower() == "true"]
        bad = len(group) - len(good)
        item = {"case": label, "path_count": len(CASES[label]), "allocation": kind,
                "bytes": size, "successes": len(good), "failures": bad}
        if good:
            seconds = [float(r["elapsed_sec"]) for r in good]
            item.update(median_sec=statistics.median(seconds), min_sec=min(seconds),
                        max_sec=max(seconds),
                        median_mbps=statistics.median(float(r["goodput_mbps"]) for r in good))
        summary.append(item)
    return summary


def read_rows(filename):
    with open(filename, newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def best_cases(summary):
    sizes = {r["bytes"] for r in summary}
    if len(sizes) != 1:
        raise ValueError("best-case selection requires exactly one file size")
    result = []
    for count in (1, 2, 3, 4):
        eligible = [r for r in summary if r["path_count"] == count
                    and r["successes"] >= 3 and r["failures"] == 0]
        if not eligible:
            raise ValueError("need at least 3 successful trials and no failures for each path count")
        result.append(min(eligible, key=lambda r: r["median_sec"])["case"])
    return result


def show_summary(summary):
    for row in sorted(summary, key=lambda r: (r["allocation"], r["bytes"], r["path_count"], r["case"])):
        if row["successes"]:
            print("{case:5s} {allocation:8s} n={successes} fail={failures} "
                  "median={median_sec:.4f}s range={min_sec:.4f}..{max_sec:.4f}s "
                  "goodput={median_mbps:.4f}Mbps".format(**row))
        else:
            print(row["case"] + ": no successful trials")


def configure(args):
    args.endpoints = dict(ENDPOINTS)
    if getattr(args, "endpoints_file", None):
        with open(args.endpoints_file, encoding="utf-8") as stream:
            values = json.load(stream)
        for path, endpoint in values.items():
            if path not in ENDPOINTS:
                raise ValueError("unknown endpoint name: " + path)
            args.endpoints[path] = parse_endpoint(endpoint)
    args.weights = {"direct": 10.0, "via2": 1.0, "via4": 10.0, "via5": 1.0}
    if getattr(args, "weights_file", None):
        with open(args.weights_file, encoding="utf-8") as stream:
            args.weights = json.load(stream)
    for path in ENDPOINTS:
        value = float(args.weights[path])
        if not math.isfinite(value) or value <= 0:
            raise ValueError("invalid path weight: " + path)
        args.weights[path] = value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command")
    for command, role in (("serve", "source"), ("relay", "relay")):
        p = sub.add_parser(command)
        p.set_defaults(role=role)
        p.add_argument("--host", default="0.0.0.0")
        p.add_argument("--port", type=int, default=19000 if role == "source" else 19001)
        p.add_argument("--block-size", type=int, default=BLOCK)
        p.add_argument("--file", required=True) if role == "source" else p.add_argument("--upstream", required=True)
    for command in ("status", "stop"):
        p = sub.add_parser(command)
        p.add_argument("--host", default="127.0.0.1")
        p.add_argument("--port", type=int, default=19000)
    for command in ("client", "calibrate", "suite"):
        p = sub.add_parser(command)
        p.add_argument("--bytes", type=int)
        p.add_argument("--endpoints-file")
        p.add_argument("--weights-file")
        p.add_argument("--allocation", choices=("equal", "weighted"), default="weighted")
        if command == "client":
            p.add_argument("--paths", default="direct")
            p.add_argument("--output", required=True)
            p.add_argument("--csv")
        else:
            p.add_argument("--out-dir", required=True)
            p.add_argument("--repeats", type=int, default=2 if command == "calibrate" else 3)
            p.add_argument("--seed", type=int, default=20261116)
        if command == "suite":
            p.add_argument("--cases", default="all")
            p.add_argument("--best-from")
            p.add_argument("--case-weights-dir")
    p = sub.add_parser("refine")
    p.add_argument("csv_file")
    p.add_argument("--out-dir", required=True)
    p.add_argument("--allocation", choices=("equal", "weighted"), default="weighted")
    p = sub.add_parser("summary")
    p.add_argument("csv_file")
    p.add_argument("--allocation", choices=("equal", "weighted"), default="weighted")
    args = parser.parse_args(argv)
    if not args.command:
        parser.print_help()
        return 1
    try:
        if args.command in ("serve", "relay"):
            if args.block_size <= 0:
                raise ValueError("block size must be positive")
            run_service(args)
            return 0
        if args.command in ("status", "stop"):
            try:
                reply = query((args.host, args.port), args.command)
            except ConnectionRefusedError:
                return 3
            print(json.dumps(reply, sort_keys=True))
            return 0
        if args.command == "summary":
            show_summary(summary_rows(read_rows(args.csv_file), args.allocation))
            return 0
        if args.command == "refine":
            rows = [r for r in read_rows(args.csv_file) if r["allocation"] == args.allocation
                    and r["case"] in CASES and r["ok"].lower() == "true"]
            if not rows or len({int(r["bytes"]) for r in rows}) != 1:
                raise ValueError("refinement requires successful trials at exactly one file size")
            os.makedirs(args.out_dir, exist_ok=True)
            for label in sorted({r["case"] for r in rows}):
                weights = {"direct": 10.0, "via2": 1.0, "via4": 10.0, "via5": 1.0}
                observed = {p: [] for p in CASES[label]}
                for row in rows:
                    if row["case"] == label:
                        for stat in json.loads(row["per_path"]):
                            observed[stat["path"]].append(float(stat["mbps"]))
                for path, values in observed.items():
                    if not values:
                        raise ValueError("missing path statistics for " + label)
                    weights[path] = statistics.median(values)
                filename = os.path.join(args.out_dir, label + ".json")
                with open(filename, "w", encoding="utf-8") as stream:
                    json.dump(weights, stream, indent=2, sort_keys=True)
                print(filename + ": " + json.dumps(weights, sort_keys=True))
            return 0
        configure(args)
        if args.bytes is not None and args.bytes <= 0:
            raise ValueError("--bytes must be positive")
        if args.command == "client":
            paths = args.paths.split(",")
            if len(set(paths)) != len(paths) or any(p not in ENDPOINTS for p in paths):
                raise ValueError("--paths must contain unique known path names")
            row = attempt(args, paths, "+".join(paths), args.output, args.csv)
            return 0 if row["ok"] else 1
        if args.repeats <= 0:
            raise ValueError("--repeats must be positive")
        os.makedirs(args.out_dir, exist_ok=True)
        rng = random.Random(args.seed)
        rows = []
        if args.command == "calibrate":
            if args.bytes is None:
                args.bytes = 2097152
            args.allocation = "equal"
            csv_file = os.path.join(args.out_dir, "calibration.csv")
            for repeat in range(args.repeats):
                paths = list(ENDPOINTS)
                rng.shuffle(paths)
                for path in paths:
                    print("Calibration round {}/{}".format(repeat + 1, args.repeats), flush=True)
                    rows.append(attempt(args, [path], "cal_" + path,
                                        os.path.join(args.out_dir, "cal_" + path + ".bin"), csv_file))
            weights = {}
            for path in ENDPOINTS:
                found = [r for r in rows if r["case"] == "cal_" + path and r["ok"]]
                if len(found) != args.repeats:
                    raise ValueError("calibration failed; fix the failed path before proceeding")
                weights[path] = statistics.median(r["goodput_mbps"] for r in found)
            with open(os.path.join(args.out_dir, "weights.json"), "w", encoding="utf-8") as stream:
                json.dump(weights, stream, indent=2, sort_keys=True)
            print("Measured path weights: " + json.dumps(weights, sort_keys=True))
            return 0
        if args.best_from:
            selected = best_cases(summary_rows(read_rows(args.best_from), args.allocation))
        else:
            selected = list(CASES) if args.cases == "all" else args.cases.split(",")
        if not selected or len(set(selected)) != len(selected) or any(c not in CASES for c in selected):
            raise ValueError("invalid or duplicate case name")
        print("Cases: " + ",".join(selected), flush=True)
        csv_file = os.path.join(args.out_dir, "results.csv")
        for repeat in range(args.repeats):
            order = list(selected)
            rng.shuffle(order)
            for label in order:
                print("Round {}/{}".format(repeat + 1, args.repeats), flush=True)
                if args.case_weights_dir:
                    with open(os.path.join(args.case_weights_dir, label + ".json"), encoding="utf-8") as stream:
                        args.weights = json.load(stream)
                rows.append(attempt(args, CASES[label], label,
                                    os.path.join(args.out_dir, label + ".bin"), csv_file))
        summary = summary_rows(rows, args.allocation)
        show_summary(summary)
        with open(os.path.join(args.out_dir, "summary.json"), "w", encoding="utf-8") as stream:
            json.dump(summary, stream, indent=2)
        return 0 if all(r["ok"] for r in rows) else 1
    except (ValueError, OSError, KeyError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

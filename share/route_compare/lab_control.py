#!/usr/bin/env python3
"""Start/stop only routebench helpers in the existing lab containers."""
import argparse
import json
import os
import subprocess
import sys
import time

BASE = "/home/ubuntu/share/route_compare"
SERVICES = [
    ("node3", 19000, "source", None),
    ("node2", 19001, "relay", "172.24.0.30:19000"),
    ("node4", 19001, "relay", "172.27.0.30:19000"),
    ("node5", 19001, "relay", "172.28.0.30:19000"),
]


def run(command):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True)


def status(node, port):
    result = run(["docker", "exec", node, "python3", BASE + "/routebench.py",
                  "status", "--port", str(port)])
    if result.returncode == 3:
        return None
    if result.returncode:
        raise RuntimeError(node + ": " + result.stderr.strip())
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("start", "stop", "status", "snapshot"))
    args = parser.parse_args()
    try:
        if args.command == "snapshot":
            here = os.path.dirname(os.path.abspath(__file__))
            filename = os.path.join(here, "environment_start2.txt")
            with open(filename, "w", encoding="utf-8") as stream:
                stream.write("Network configuration must be make start2; this command does not change it.\n")
                for node in ("node1", "node2", "node3", "node4", "node5"):
                    for command in (["uname", "-r"], ["ip", "-br", "-4", "addr"], ["tc", "qdisc", "show"]):
                        result = run(["docker", "exec", node] + command)
                        stream.write("\n[{}: {}]\n{}{}".format(node, " ".join(command), result.stdout, result.stderr))
                        if result.returncode:
                            raise RuntimeError("snapshot failed for " + node)
            print("Saved " + filename)
            return 0
        services = SERVICES if args.command != "stop" else list(reversed(SERVICES))
        for node, port, role, upstream in services:
            current = status(node, port)
            if current:
                expected = list(upstream.rsplit(":", 1)) if upstream else None
                if expected:
                    expected[1] = int(expected[1])
                if (current.get("role") != role or current.get("upstream") != expected
                        or current.get("block_size") != 1000
                        or (role == "source" and current.get("file") != BASE + "/route_test.dat")):
                    raise RuntimeError(node + ": a different routebench configuration is already using the port")
            if args.command == "status":
                print(node + ": " + ("READY" if current else "STOPPED"))
            elif args.command == "stop":
                if current:
                    result = run(["docker", "exec", node, "python3", BASE + "/routebench.py",
                                  "stop", "--port", str(port)])
                    if result.returncode:
                        raise RuntimeError(result.stderr.strip())
                print(node + ": STOPPED")
            elif current:
                print(node + ": already READY")
            else:
                command = ["python3", BASE + "/routebench.py"]
                if role == "source":
                    command += ["serve", "--file", BASE + "/route_test.dat"]
                else:
                    command += ["relay", "--upstream", upstream]
                result = run(["docker", "exec", node, "mkdir", "-p", BASE + "/logs"])
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                # All paths and arguments are passed as argv, not interpolated shell code.
                result = run(["docker", "exec", "-d", node, "sh", "-c",
                              'log="$1"; shift; exec "$@" >"$log" 2>&1',
                              "routebench-launch", BASE + "/logs/" + node + ".log"] + command)
                if result.returncode:
                    raise RuntimeError(result.stderr.strip())
                for _ in range(30):
                    time.sleep(0.2)
                    if status(node, port):
                        break
                else:
                    raise RuntimeError(node + ": helper did not start; inspect logs/" + node + ".log")
                print(node + ": READY")
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

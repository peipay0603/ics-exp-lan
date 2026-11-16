"""Local correctness tests. These tests do not reproduce the lab bandwidth."""
import hashlib
import contextlib
import io
import json
import os
import socketserver
import tempfile
import threading
import unittest
from types import SimpleNamespace

import routebench as rb


class FileTransferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.data = os.urandom(131073)
        cls.source = os.path.join(cls.tmp.name, "source.bin")
        with open(cls.source, "wb") as stream:
            stream.write(cls.data)
        cls.servers = []
        source = cls.start_service("source")
        cls.endpoints = {"direct": source.server_address}
        for path in ("via2", "via4", "via5"):
            cls.endpoints[path] = cls.start_service("relay", source.server_address).server_address

    @classmethod
    def start_service(cls, role, upstream=None):
        server = rb.Service(("127.0.0.1", 0), rb.Handler)
        server.role = role
        server.source_file = cls.source if role == "source" else None
        server.upstream = upstream
        server.block_size = 1000
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        thread.start()
        cls.servers.append((server, thread))
        return server

    @classmethod
    def tearDownClass(cls):
        for server, thread in reversed(cls.servers):
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
        cls.tmp.cleanup()

    def test_all_eight_cases_reassemble_the_real_file(self):
        weights = {"direct": 10, "via2": 1, "via4": 10, "via5": 1}
        for label, paths in rb.CASES.items():
            for allocation in ("equal", "weighted"):
                with self.subTest(case=label, allocation=allocation):
                    output = os.path.join(self.tmp.name, label + allocation + ".bin")
                    row = rb.transfer(paths, weights, self.endpoints, allocation, len(self.data), output, label)
                    with open(output, "rb") as stream:
                        actual = stream.read()
                    self.assertEqual(actual, self.data)
                    self.assertTrue(row["ok"])
                    self.assertEqual(sum(s["bytes"] for s in json.loads(row["per_path"])), len(self.data))
                    self.assertEqual(row["path_count"], len(paths))

    def test_prefix_transfer_and_range_validation(self):
        weights = {p: 1 for p in self.endpoints}
        output = os.path.join(self.tmp.name, "prefix.bin")
        rb.transfer(["direct", "via5"], weights, self.endpoints, "equal", 10003, output, "prefix")
        with open(output, "rb") as stream:
            self.assertEqual(stream.read(), self.data[:10003])
        with self.assertRaises(ValueError):
            rb.transfer(["direct"], weights, self.endpoints, "equal", len(self.data) + 1, output, "bad")

    def test_shared_source_cannot_be_truncated_by_the_client(self):
        with self.assertRaisesRegex(ValueError, "overwrite"):
            rb.transfer(["direct"], {"direct": 1}, self.endpoints, "equal", 100, self.source, "bad")
        with open(self.source, "rb") as stream:
            self.assertEqual(stream.read(), self.data)

    def test_calibration_screen_refinement_and_final_selection_cli(self):
        root = os.path.join(self.tmp.name, "cli")
        os.makedirs(root)
        endpoint_file = os.path.join(root, "endpoints.json")
        with open(endpoint_file, "w", encoding="utf-8") as stream:
            json.dump({p: "{}:{}".format(*endpoint) for p, endpoint in self.endpoints.items()}, stream)
        common = ["--bytes", "4097", "--endpoints-file", endpoint_file]
        calibration = os.path.join(root, "calibration")
        screen = os.path.join(root, "screen")
        refined = os.path.join(root, "weights")
        final = os.path.join(root, "final")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(rb.main(["calibrate", "--out-dir", calibration, "--repeats", "1"] + common), 0)
            weights_file = os.path.join(calibration, "weights.json")
            self.assertEqual(rb.main(["suite", "--out-dir", screen, "--repeats", "3",
                                      "--weights-file", weights_file] + common), 0)
            csv_file = os.path.join(screen, "results.csv")
            self.assertEqual(rb.main(["refine", csv_file, "--out-dir", refined]), 0)
            self.assertEqual(rb.main(["suite", "--out-dir", final, "--repeats", "1",
                                      "--best-from", csv_file, "--case-weights-dir", refined] + common), 0)
        rows = rb.read_rows(os.path.join(final, "results.csv"))
        self.assertEqual(sorted(int(r["path_count"]) for r in rows), [1, 2, 3, 4])
        for row in rows:
            self.assertEqual(row["ok"], "True")
            with open(row["output"], "rb") as stream:
                self.assertEqual(stream.read(), self.data[:4097])

    def test_bad_hash_is_rejected_and_failure_is_logged(self):
        class CorruptHandler(socketserver.BaseRequestHandler):
            def handle(self):
                req = rb.recv_json(self.request)
                if req["op"] == "info":
                    rb.send_json(self.request, {"ok": True, "size": 4})
                else:
                    rb.send_json(self.request, {"ok": True, "offset": 0, "length": 4})
                    self.request.sendall(b"DATA" + hashlib.sha256(b"WRONG").digest())
        with rb.Service(("127.0.0.1", 0), CorruptHandler) as server:
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
            thread.start()
            args = SimpleNamespace(weights={"direct": 1}, endpoints={"direct": server.server_address},
                                   allocation="equal", bytes=4)
            csv_file = os.path.join(self.tmp.name, "bad.csv")
            row = rb.attempt(args, ["direct"], "D", os.path.join(self.tmp.name, "bad.bin"), csv_file)
            self.assertFalse(row["ok"])
            self.assertIn("SHA-256", row["error"])
            self.assertEqual(rb.read_rows(csv_file)[0]["ok"], "False")
            server.shutdown()
            thread.join(timeout=2)

    def test_protocol_status_and_graceful_stop(self):
        server = self.start_service("relay", self.endpoints["direct"])
        self.assertEqual(rb.query(server.server_address, "status")["role"], "relay")
        self.assertTrue(rb.query(server.server_address, "stop")["ok"])
        self.servers[-1][1].join(timeout=2)
        self.assertFalse(self.servers[-1][1].is_alive())


class AllocationAndSelectionTests(unittest.TestCase):
    def test_ranges_have_no_gaps_overlaps_or_empty_active_paths(self):
        paths = list(rb.ENDPOINTS)
        for size in (4, 7, 1000, 131073, 536870912):
            for weights in ({p: 1 for p in paths},
                            {"direct": 10, "via2": 1, "via4": 10, "via5": 1},
                            {"direct": 1e-9, "via2": 1000, "via4": 3, "via5": 2}):
                ranges = rb.partition(size, paths, weights)
                end = 0
                for _, offset, length in ranges:
                    self.assertEqual(offset, end)
                    self.assertGreater(length, 0)
                    end += length
                self.assertEqual(end, size)
        for bad in (0, -1, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                rb.partition(100, ["direct"], {"direct": bad})

    def test_selection_uses_complete_trials_at_equal_file_size(self):
        rows = []
        speeds = {"D": 10, "D2": 11, "D4": 20, "D5": 11,
                  "D24": 21, "D25": 12, "D45": 20.5, "D245": 22}
        for label, speed in speeds.items():
            for _ in range(3):
                rows.append({"allocation": "weighted", "bytes": "1000", "case": label,
                             "ok": "True", "elapsed_sec": str(8 / speed), "goodput_mbps": str(speed)})
        summary = rb.summary_rows(rows, "weighted")
        self.assertEqual(rb.best_cases(summary), ["D", "D4", "D24", "D245"])
        rows.append(dict(rows[0], ok="False"))
        with self.assertRaises(ValueError):
            rb.best_cases(rb.summary_rows(rows, "weighted"))
        rows.append(dict(rows[0], bytes="2000"))
        with self.assertRaises(ValueError):
            rb.best_cases(rb.summary_rows(rows, "weighted"))


if __name__ == "__main__":
    unittest.main(verbosity=2)

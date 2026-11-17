"""Tests for tools/latency/udp_vs_krpc.py: statistics, kRPC encoding and an end-to-end run against kspsim.

    python3 -m unittest tools/latency/test_udp_vs_krpc.py

The end-to-end test serves kspsim on 127.0.0.1 with ephemeral ports only (never 50000-50002, no discovery).
"""
from __future__ import annotations

import io
import json
import math
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import krpc_pb2_lite as K  # noqa: E402
import udp_vs_krpc as L  # noqa: E402


class StatsTest(unittest.TestCase):
    def test_percentile_interpolates(self):
        s = [1.0, 2.0, 3.0, 4.0, 5.0]
        self.assertEqual(L.percentile(s, 0.5), 3.0)
        self.assertAlmostEqual(L.percentile(s, 0.9), 4.6)
        self.assertAlmostEqual(L.percentile(s, 0.99), 4.96)
        self.assertEqual(L.percentile([7.0], 0.99), 7.0)
        self.assertTrue(math.isnan(L.percentile([], 0.5)))

    def test_summarize(self):
        s = L.summarize([10.0, 12.0, 11.0, 15.0], timeouts=2, errors=1)
        self.assertEqual((s["n"], s["timeouts"], s["errors"]), (4, 2, 1))
        self.assertEqual((s["min"], s["max"], s["mean"]), (10.0, 15.0, 12.0))
        self.assertEqual(s["median"], 11.5)
        self.assertAlmostEqual(s["stdev"], math.sqrt(14 / 3))
        self.assertAlmostEqual(s["mean_abs_diff"], (2 + 1 + 4) / 3)  # successive, in time order

    def test_summarize_empty_and_single(self):
        s = L.summarize([], timeouts=5)
        self.assertEqual((s["n"], s["timeouts"]), (0, 5))
        self.assertTrue(math.isnan(s["median"]) and math.isnan(s["stdev"]))
        one = L.summarize([3.0])
        self.assertEqual(one["p99"], 3.0)
        self.assertTrue(math.isnan(one["stdev"]) and math.isnan(one["mean_abs_diff"]))

    def test_linear_fit(self):
        a, b = L.linear_fit([0.0, 1.0, 2.0], [5.0, 7.0, 9.0])
        self.assertAlmostEqual(a, 5.0)
        self.assertAlmostEqual(b, 2.0)


class EncodingTest(unittest.TestCase):
    """The hand-written client encoder against the schema-driven stand-in (KRPC.proto field numbers)."""

    def test_connection_request(self):
        req = K.ConnectionRequest()
        req.ParseFromString(L.connection_request(0, L.CLIENT_NAME, b""))
        self.assertEqual((req.type, req.client_name, req.protocol_version), (0, L.CLIENT_NAME, 1))
        req = K.ConnectionRequest()
        req.ParseFromString(L.connection_request(1, "", b"\x01\x02"))
        self.assertEqual((req.type, req.client_identifier), (K.ConnectionRequest.STREAM, b"\x01\x02"))

    def test_call_and_result(self):
        call = K.ProcedureCall()
        call.ParseFromString(L.encode_call("SpaceCenter", "Vessel_get_Name", [L.varint(300), b"x"]))
        self.assertEqual((call.service, call.procedure), ("SpaceCenter", "Vessel_get_Name"))
        self.assertEqual([(a.position, a.value) for a in call.arguments], [(0, L.varint(300)), (1, b"x")])
        ok = K.ProcedureResult(value=b"\x00" * 7 + b"\x40")
        self.assertEqual(L.decode_result(ok.SerializeToString()), b"\x00" * 7 + b"\x40")
        self.assertIsNone(L.decode_result(K.ProcedureResult(is_null=True).SerializeToString()))
        bad = K.ProcedureResult(error=K.Error(service="KRPC", name="X", description="boom"))
        with self.assertRaisesRegex(L.RpcError, "boom"):
            L.decode_result(bad.SerializeToString())


class EndToEndTest(unittest.TestCase):
    def test_against_kspsim(self):
        import local_sim
        sim, server = local_sim.start(0, 0, 0)
        try:
            with tempfile.TemporaryDirectory() as out:
                stdout = io.StringIO()
                with redirect_stdout(stdout), redirect_stderr(io.StringIO()):
                    rc = L.main(["--host", "127.0.0.1", "--rpc-port", str(server.rpc_port),
                                 "--stream-port", str(server.stream_port), "--udp-port", str(server.context_port),
                                 "--count", "20", "--udp-rate", "100", "--rpc-rate", "100", "--stream-rate", "20",
                                 "--vessel", "--label", "test", "--out-dir", out])
                self.assertEqual(rc, 0)
                files = sorted(os.listdir(out))
                self.assertEqual([f[-4:] for f in files], [".csv", "json"])
                with open(os.path.join(out, files[1]), encoding="utf-8") as f:
                    report = json.load(f)
        finally:
            server.stop()
            sim.stop()
        res = report["results"]
        self.assertEqual(report["label"], "test")
        self.assertEqual(res["udp"]["summary"]["n"], 20)
        self.assertEqual(res["udp"]["summary"]["scenes"], {"flight": 20})
        for metric in ("get_UT", "get_ActiveVessel", "Vessel_get_Name"):
            self.assertEqual(res["rpc"]["summary"][metric]["n"], 20, metric)
        names = {s["value"] for s in res["rpc"]["samples"] if s["metric"] == "Vessel_get_Name"}
        self.assertEqual(len(names), 1)
        self.assertTrue(names.pop())
        self.assertEqual(res["stream"]["summary"]["interval"]["n"], 19)
        self.assertGreater(res["stream"]["summary"]["ut_step"]["median"], 0)

    def test_refuses_standard_ports(self):
        import local_sim
        with self.assertRaises(ValueError):
            local_sim.start(50000, 0, 0)


if __name__ == "__main__":
    unittest.main()

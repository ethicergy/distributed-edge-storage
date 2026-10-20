"""scripts/measure.py: parsers, statistics, RESULTS.md filling, and a
short run against the simulator (skipped if it is not built)."""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "measure.py")
SIM = os.path.join(ROOT, "build", "host", "edge_sim")

spec = importlib.util.spec_from_file_location("measure", SCRIPT)
measure = importlib.util.module_from_spec(spec)
sys.modules["measure"] = measure
spec.loader.exec_module(measure)


class ParseTest(unittest.TestCase):
    def test_log_lines_from_board_and_simulator(self):
        self.assertEqual(measure.parse_log("   12.345 view: AB-D"), (12.345, None, "view: AB-D"))
        self.assertEqual(measure.parse_log("  75.502 [A] view: AB-D"), (75.502, "A", "view: AB-D"))
        self.assertEqual(measure.parse_log("SET k: OK (served by D, 1 attempt(s), 11 ms)")[0], None)

    def test_console_results(self):
        m = measure.OP_RESULT.match("SET k: OK (served by D, 2 attempt(s), 611 ms)")
        self.assertEqual(m.groups(), ("SET", "k", "OK", "D", "2", "611"))
        m = measure.OP_RESULT.match("SET x: UNDER_REPLICATED (served by A, 1 attempt(s), 1 ms)")
        self.assertEqual(m.group(3), "UNDER_REPLICATED")
        m = measure.GET_SERVED.match("   (served by C, 1 attempt(s), 8 ms)")
        self.assertEqual(m.groups(), ("C", "1", "8"))
        m = measure.BENCH.match("bench: 50 ok, 0 failed in 29513 ms; latency min 7 avg 590 max 4310 ms")
        self.assertEqual(m.groups(), ("50", "0", "29513", "7", "590", "4310"))
        m = measure.RING_ALL.match("  all nodes up : primary C, replica B")
        self.assertEqual(m.groups(), ("C", "B"))
        self.assertTrue(measure.REFUSED.match("SET k: refused (BUSY)"))

    def test_status_and_stats(self):
        m = measure.STATUS_HEAD.match("node C  state ACTIVE  view ABCD  uptime 91s")
        self.assertEqual(m.groups(), ("C", "ACTIVE", "ABCD", "91"))
        m = measure.SENSOR.match(
            "sensor temp (hardware): on, 240 samples, last 21.500 degC STALE (last good value, no good read lately) "
            "seq 239, 470 ok, 6 under-replicated, 4 failed, 17 read failures")
        self.assertEqual(m.group(1), "temp")
        self.assertEqual(m.groups()[6:], ("470", "6", "4", "17"))
        m = measure.SENSOR.match(
            "sensor motion (simulated): on, 3 samples, last 3 events, last motion 0 s before the sample seq 2, "
            "6 ok, 0 under-replicated, 0 failed, 0 read failures")
        self.assertEqual(m.group(2), "simulated")
        self.assertEqual(measure.CLIENT.match("client  ok 120  failed 0  attempts 124  local 31").group(3), "124")
        self.assertEqual(measure.SERVER.match(
            "server  executed 90  not-ready 0  dedup replays 7  suppressed 1  table-full 0").group(3), "7")
        self.assertEqual(measure.RESYNC.search(
            "ae      rounds 4  chunks tx 11 rx 27  tables 8  pushes 0  resync tx 3 applied 10 stale 0").group(2), "10")
        m = measure.WEAR.match("eeprom page writes since boot: total 812, hottest page 1 with 27 (rated 1000000)")
        self.assertEqual(m.groups(), ("812", "1", "27"))


class StatsTest(unittest.TestCase):
    def test_percentile_nearest_rank(self):
        values = list(range(1, 101))
        self.assertEqual(measure.percentile(values, 95), 95)
        self.assertEqual(measure.percentile([5, 1, 3], 50), 3)
        self.assertEqual(measure.percentile([7], 95), 7)
        s = measure.summarize([10, 20, 30, 40])
        self.assertEqual((s["min"], s["avg"], s["p95"], s["max"], s["n"]), (10, 25, 40, 40, 4))
        self.assertEqual(measure.fmt_latency(measure.summarize([])), "n/a")

    def test_wear_projection_matches_soak_test(self):
        # cluster_test H13: 27 cycles on the hottest page in 2 h -> 8.5 years.
        self.assertAlmostEqual(measure.years_to_rating(27, 7200), 8.45, places=1)
        self.assertIsNone(measure.years_to_rating(0, 7200))


class FillTest(unittest.TestCase):
    def test_fills_only_the_named_column_by_id(self):
        text = (
            "intro\n\n"
            "| id | metric | simulator | hardware | why they differ |\n"
            "|---|---|---|---|---|\n"
            "| `set_bridge` | SET from A | 1 ms | | reason |\n"
            "| `other` | x | 2 | | |\n"
            "\n| id | simulator |\n|---|---|\n| `set_bridge` | old |\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "R.md")
            with open(path, "w") as handle:
                handle.write(text)
            changed = measure.fill_results(path, "hardware", {"set_bridge": "9 | 9 ms", "missing": "x"})
            with open(path) as handle:
                out = handle.read()
        self.assertEqual(changed, 1)  # the second table has no hardware column
        self.assertIn("| `set_bridge` | SET from A | 1 ms | 9 / 9 ms | reason |", out)
        self.assertIn("| `other` | x | 2 | | |", out)
        self.assertIn("| `set_bridge` | old |", out)


@unittest.skipUnless(os.path.exists(SIM), "simulator not built (make sim)")
class BoardWaitTest(unittest.TestCase):
    """Board consoles drop input for 1 s after the port opens (echo guard):
    measure.py waits POST_OPEN_WAIT_S after every (re)open before sending."""

    def test_first_command_waits_after_open(self):
        import time
        import types

        writes = []

        class FakeSerial:
            def __init__(self, port, baud, timeout):
                self.opened = time.monotonic()
                self.timeout = timeout

            def read(self, n):
                time.sleep(self.timeout)
                return b""

            def write(self, data):
                writes.append((time.monotonic() - self.opened, data))

            def close(self):
                pass

        fake = types.ModuleType("serial")
        fake.Serial = FakeSerial
        fake.SerialException = OSError
        saved = sys.modules.get("serial")
        sys.modules["serial"] = fake
        try:
            backend = measure.SerialBackend({"A": "/dev/fake"})
            backend.send("A", "status")
            backend.send("A", "members")
            backend.close()
        finally:
            if saved is None:
                del sys.modules["serial"]
            else:
                sys.modules["serial"] = saved

        self.assertEqual([d for _, d in writes], [b"status\n", b"members\n"])
        self.assertGreaterEqual(writes[0][0], measure.SerialBackend.POST_OPEN_WAIT_S - 0.01)
        # Only the first command after an open waits.
        self.assertLess(writes[1][0] - writes[0][0], 0.2)


class SimulatorRunTest(unittest.TestCase):
    def test_short_run_produces_every_metric(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "m.json")
            subprocess.run(
                [sys.executable, SCRIPT, "--sim", "--sim-binary", SIM, "--speed", "4",
                 "--count", "3", "--bench", "5", "--repair-keys", "2", "--json", out],
                check=True, capture_output=True, text=True, timeout=180,
            )
            with open(out) as handle:
                data = json.load(handle)
        metrics, raw = data["metrics"], data["raw"]
        for key in ["set_bridge", "set_other", "get_bridge", "get_other", "latency_retries",
                    "failover_detect", "failover_client", "outage_writes", "catchup", "catchup_view",
                    "keys_repaired", "loss10_A", "loss10_B", "loss10_retries", "loss20_A", "loss20_B",
                    "loss20_retries", "sensor_A", "sensor_D", "wear_A", "wear_D"]:
            self.assertIn(key, metrics)
            self.assertNotEqual(metrics[key], "n/a", key)
        # Timings are set by config.h (2 s membership timeout, 3 s attempt
        # deadline, 2.5 s + 3 s boot gate) and measured on the sim clock.
        self.assertTrue(1400 <= raw["failover_detect"] <= 2100, raw["failover_detect"])
        self.assertTrue(3000 <= raw["failover_client"] <= 4500, raw["failover_client"])
        self.assertTrue(5400 <= raw["catchup"] <= 5700, raw["catchup"])
        self.assertEqual(raw["keys_repaired"]["correct"], 2)
        self.assertEqual(raw["loss20_A"]["failed"], 0)
        self.assertIn("simulated", metrics["sensor_A"])


if __name__ == "__main__":
    unittest.main()

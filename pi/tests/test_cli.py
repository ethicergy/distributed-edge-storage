import struct
import unittest
from datetime import datetime, timedelta, timezone

from edgekv import cli, reading

NOW = datetime(2026, 10, 18, 12, 0, 30, tzinfo=timezone.utc)


class FakeKV:
    """wall_time() as EdgeKV after calibration: HLC ms 1000 = NOW - 30 s."""

    def wall_time(self, hlc):
        return NOW - timedelta(seconds=30) + timedelta(milliseconds=(hlc >> 16) - 1000)


def sample(kind, flags, value_milli, aux=None, hlc=1000 << 16):
    if aux is None:
        return reading.decode(struct.pack(">BBBBIiQ", 1, kind, flags, 0, 7, value_milli, hlc))
    return reading.decode(struct.pack(">BBBBIiQi", 2, kind, flags, 0, 7, value_milli, hlc, aux))


class FormatTest(unittest.TestCase):
    def test_level(self):
        line = cli._format_reading("A", sample(1, 0, 23202), FakeKV(), NOW)
        self.assertIn("temp", line)
        self.assertIn("23.202 degC", line)
        self.assertIn("seq 7", line)
        self.assertNotIn("simulated", line)

    def test_motion(self):
        # Last detection 12 s before a sample taken 30 s ago: 42 s ago.
        line = cli._format_reading("B", sample(5, 0, 2000, 12), FakeKV(), NOW)
        self.assertIn("2 events", line)
        self.assertIn("last motion 42 s ago", line)

        line = cli._format_reading("B", sample(5, 0, 0, -1), FakeKV(), NOW)
        self.assertIn("no motion since boot", line)

        line = cli._format_reading("B", sample(5, reading.FLAG_SETTLING, 0, 0), FakeKV(), NOW)
        self.assertIn("settling", line)

    def test_sound(self):
        line = cli._format_reading("C", sample(6, reading.FLAG_SIMULATED, 5000, 37500), FakeKV(), NOW)
        self.assertIn("5 events", line)
        self.assertIn("37.5 % active", line)
        self.assertIn("simulated", line)

    def test_stale_and_error(self):
        line = cli._format_reading("D", sample(1, reading.FLAG_STALE, 21500), FakeKV(), NOW)
        self.assertIn("21.500 degC", line)
        self.assertIn("STALE", line)
        self.assertNotIn("simulated", line)

        line = cli._format_reading("D", sample(1, reading.FLAG_ERROR, 0), FakeKV(), NOW)
        self.assertIn("ERROR: no good temp read since boot", line)

    def test_missing(self):
        self.assertEqual(cli._format_reading("D", None, FakeKV(), NOW), "D  (no reading yet)")


class CsvTest(unittest.TestCase):
    def row(self, s):
        values = cli._csv_row(NOW, "X", s, FakeKV())
        self.assertEqual(len(values), len(cli.CSV_HEADER))
        return dict(zip(cli.CSV_HEADER, values))

    def test_columns(self):
        r = self.row(sample(1, 0, 23202))
        self.assertEqual((r["kind"], r["value"], r["unit"]), ("temp", "23.202", "degC"))
        self.assertEqual((r["last_motion_s"], r["active_pct"], r["status"]), ("", "", "ok"))
        self.assertEqual(r["sampled_at"], (NOW - timedelta(seconds=30)).astimezone().isoformat(timespec="seconds"))

        r = self.row(sample(5, 0, 2000, 12))
        self.assertEqual((r["kind"], r["value"], r["last_motion_s"], r["status"]), ("motion", "2.000", "12", "ok"))

        r = self.row(sample(5, reading.FLAG_SETTLING, 0, 0))
        self.assertEqual((r["last_motion_s"], r["status"]), ("", "settling"))

        r = self.row(sample(1, reading.FLAG_STALE, 21500))
        self.assertEqual((r["value"], r["status"]), ("21.500", "stale"))

        r = self.row(sample(1, reading.FLAG_ERROR, 0))
        self.assertEqual(r["status"], "error")

        r = self.row(sample(6, reading.FLAG_SIMULATED, 5000, 37500))
        self.assertEqual((r["kind"], r["active_pct"], r["simulated"]), ("sound", "37.5", "1"))

    def test_header_keeps_v1_columns_first(self):
        self.assertEqual(
            cli.CSV_HEADER[:8],
            ["read_at", "node", "kind", "value", "unit", "sequence", "sampled_at", "simulated"],
        )


if __name__ == "__main__":
    unittest.main()

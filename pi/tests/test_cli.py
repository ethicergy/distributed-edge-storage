import contextlib
import importlib.util
import io
import os
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


class GetTest(unittest.TestCase):
    class KV(FakeKV):
        def __init__(self, value):
            self.value = value

        def get(self, key):
            return self.value

    def run_get(self, key, value):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.cmd_get(self.KV(value), type("Args", (), {"key": key})())
        return code, out.getvalue().strip()

    def test_sensor_keys_are_decoded_like_sensors(self):
        raw = struct.pack(">BBBBIiQ", 1, 1, 0, 0, 7, 23202, 1000 << 16)
        code, line = self.run_get("s/D/temp", raw)
        self.assertEqual(code, 0)
        self.assertEqual(line, cli._format_reading("D", reading.decode(raw), FakeKV()).strip())
        self.assertIn("23.202 degC", line)

        motion = struct.pack(">BBBBIiQi", 2, 5, 0, 0, 3, 2000, 1000 << 16, 12)
        _, line = self.run_get("h/B/motion/3", motion)
        self.assertTrue(line.startswith("B  motion"))
        self.assertIn("2 events", line)

    def test_other_values_unchanged(self):
        self.assertEqual(self.run_get("greeting", b"hello world"), (0, "hello world"))
        self.assertEqual(self.run_get("blob", b"\x00\x01"), (0, "0x0001"))
        # A sensor-looking key whose value is not a reading stays raw text.
        self.assertEqual(self.run_get("s/A/note", b"just text"), (0, "just text"))
        self.assertEqual(self.run_get("k", None), (1, "(not found)"))


class PortErrorTest(unittest.TestCase):
    def run_cli(self, argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = cli.main(argv)
        return code, err.getvalue()

    def test_messages(self):
        busy = OSError(11, "Could not exclusively lock port /dev/serial0: [Errno 11] Resource temporarily unavailable")
        self.assertEqual(cli._port_error("/dev/serial0", busy),
                         "error: port busy: another edgekv command is using it, stop `watch` first")
        late = OSError("device reports readiness to read but returned no data "
                       "(device disconnected or multiple access on port?)")
        self.assertIn("port busy", cli._port_error("/dev/serial0", late))
        self.assertIn("not found", cli._port_error("/dev/x", OSError(2, "could not open port /dev/x")))
        self.assertIn("dialout", cli._port_error("/dev/x", OSError(13, "could not open port: Permission denied")))
        self.assertTrue(cli._port_error("/dev/x", OSError(5, "I/O error")).startswith("error: serial port /dev/x:"))

    @unittest.skipUnless(importlib.util.find_spec("serial"), "pyserial not installed")
    def test_not_found_is_one_line(self):
        code, err = self.run_cli(["--port", "/dev/ttyEDGEKV-NOPE", "ping"])
        self.assertEqual(code, 4)
        self.assertEqual(err.count("\n"), 1)
        self.assertIn("not found", err)

    @unittest.skipUnless(importlib.util.find_spec("serial") and hasattr(os, "openpty"), "needs pyserial and a pty")
    def test_second_client_gets_port_busy(self):
        import serial

        master, slave = os.openpty()
        name = os.ttyname(slave)
        holder = serial.serial_for_url(name, baudrate=115200, timeout=0.05, exclusive=True)
        try:
            code, err = self.run_cli(["--port", name, "--timeout", "0.2", "ping"])
        finally:
            holder.close()
            os.close(master)
            os.close(slave)
        self.assertEqual(code, 4)
        self.assertEqual(err.strip(), "error: port busy: another edgekv command is using it, stop `watch` first")


if __name__ == "__main__":
    unittest.main()

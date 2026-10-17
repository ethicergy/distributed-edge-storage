"""End-to-end: the Python client against the bridge of the host
cluster simulator (build/host/edge_sim, built by `make sim`)."""

import os
import re
import select
import subprocess
import termios
import time
import tty
import unittest
from pathlib import Path

from edgekv import EdgeKV, Status

SIM = Path(__file__).resolve().parents[2] / "build" / "host" / "edge_sim"


class PtyTransport:
    """Minimal pyserial-like transport over a pseudo-terminal."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        attributes = termios.tcgetattr(self.fd)
        attributes[3] &= ~termios.ECHO
        termios.tcsetattr(self.fd, termios.TCSANOW, attributes)

    def write(self, data):
        os.write(self.fd, data)

    def read(self, size):
        ready, _, _ = select.select([self.fd], [], [], 0.05)
        return os.read(self.fd, size) if ready else b""

    def close(self):
        os.close(self.fd)


@unittest.skipUnless(SIM.exists(), "run `make sim` first")
class SimulatorIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.sim = subprocess.Popen(
            [str(SIM), "--quiet", "--speed", "4"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        path = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and path is None:
            line = self.sim.stdout.readline()
            match = re.search(r"pseudo-terminal: (\S+)", line)
            if match:
                path = match.group(1)
        self.assertIsNotNone(path, "simulator did not report its pty")
        self.kv = EdgeKV(path, transport=PtyTransport(path), timeout=5.0)

    def tearDown(self):
        self.kv.close()
        try:
            self.sim.stdin.write("quit\n")
            self.sim.stdin.flush()
        except BrokenPipeError:
            pass
        self.sim.wait(timeout=10)
        self.sim.stdout.close()
        self.sim.stdin.close()

    def command(self, line):
        self.sim.stdin.write(line + "\n")
        self.sim.stdin.flush()

    def wait_ready(self):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                self.kv.set(b"probe", b"1")
                return
            except Exception:
                time.sleep(0.2)
        self.fail("cluster never became ready")

    def test_set_get_delete_and_sensors_with_node_down(self):
        self.assertEqual(self.kv.ping(), 1)
        self.wait_ready()

        for i in range(10):
            key = f"py/{i}".encode()
            self.assertIn(self.kv.set(key, f"value {i}".encode()).status, (Status.OK, Status.REPLICATION_FAILED))
            self.assertEqual(self.kv.get(key), f"value {i}".encode())

        self.assertIsNotNone(self.kv.delete(b"py/0"))
        self.assertIsNone(self.kv.get(b"py/0"))

        # Sensors write every 10 s of virtual time (x4 speed).
        deadline = time.monotonic() + 20
        sensors = {}
        while time.monotonic() < deadline:
            sensors = self.kv.sensors()
            if all(sample is not None for sample in sensors.values()):
                break
            time.sleep(0.5)
        self.assertTrue(all(sample is not None for sample in sensors.values()), sensors)
        self.assertEqual(sensors["A"].kind, "temp")
        self.assertIsNotNone(self.kv.wall_time(sensors["A"].hlc))

        # H14: a node other than the bridge goes down.
        self.command("off C")
        time.sleep(1.0)
        self.assertIsNotNone(self.kv.sensor("C"))
        self.kv.set(b"py/while-down", b"ok")
        self.assertEqual(self.kv.get(b"py/while-down"), b"ok")
        self.assertEqual(self.kv.get(b"py/5"), b"value 5")


if __name__ == "__main__":
    unittest.main()

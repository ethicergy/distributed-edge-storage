import io
import os
import tempfile
import unittest

from edgekv import cli, framing, protocol
from edgekv.client import EdgeKV
from edgekv.protocol import MessageType
from edgekv.rawlog import RawLog

ROM_BANNER = (
    b"ESP-ROM:esp32c3-api1-20210207\r\nBuild:Feb  7 2021\r\n"
    b"rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)\r\n"
)


def response(msg_type, request_id, payload, sender=1, hlc=(5000 << 16) | 3):
    header = protocol.HEADER.pack(1, int(msg_type), 0, 22, request_id, sender, sender, sender, 0, hlc, len(payload))
    return header + payload


class DiscardHookTest(unittest.TestCase):
    def test_every_discarded_byte_is_reported_in_order(self):
        seen = []
        decoder = framing.Decoder(lambda data, reason: seen.append((data, reason)))

        good = protocol.build_get(1, b"k")
        bad = bytearray(framing.encode(protocol.build_get(2, b"x")))
        bad[-1] ^= 0xFF
        stream = ROM_BANNER + framing.encode(good) + bytes(bad) + framing.encode(good) + b"tail"

        frames = []
        for i in range(0, len(stream), 7):
            frames += decoder.feed(stream[i : i + 7])

        self.assertEqual(frames, [good, good])
        dropped = b"".join(data for data, _ in seen)
        self.assertEqual(len(dropped), decoder.discarded)
        self.assertTrue(dropped.startswith(ROM_BANNER))
        self.assertTrue(dropped.endswith(b"tail"))
        reasons = {reason for _, reason in seen}
        self.assertIn("no sync", reasons)
        self.assertIn("bad crc", reasons)

    def test_idle_and_bad_length(self):
        seen = []
        decoder = framing.Decoder(lambda data, reason: seen.append(reason))
        decoder.feed(b"\xaa\x55\x05")  # length below the minimum
        self.assertIn("bad length", seen)
        decoder.feed(b"\xaa\x55\x30\x01")  # partial frame, then silence
        decoder.abandon_partial()
        self.assertIn("idle", seen)


class RawLogTest(unittest.TestCase):
    def test_merges_until_flush(self):
        out = io.StringIO()
        log = RawLog("unused", stream=out)
        log.add(ROM_BANNER[:20], "no sync")
        log.add(ROM_BANNER[20:], "no sync")
        log.add(b"\xaa", "bad crc")
        self.assertEqual(out.getvalue(), "")

        log.flush()
        text = out.getvalue()
        self.assertIn(f"discarded {len(ROM_BANNER) + 1} bytes (no sync {len(ROM_BANNER)}, bad crc 1)", text)
        self.assertIn("ESP-ROM:esp32c3-api1-20210207\\r\\n", text)
        self.assertIn("  hex:  45 53 50 2d", text)
        self.assertEqual((log.entries, log.bytes), (1, len(ROM_BANNER) + 1))

        log.flush()  # nothing pending: no new entry
        self.assertEqual(log.entries, 1)

        log.add(b"zz", "no sync")
        log.flush()
        self.assertIn("discarded 2 bytes (no sync)\n", out.getvalue())


class FakeBridge:
    """Answers every GET with a GET_RESP, preceded by node A's boot text."""

    def __init__(self):
        self._pending = b""

    def write(self, frame):
        packet = framing.Decoder().feed(frame)[0]
        request_id = protocol.parse_header(packet).request_id
        answer = response(MessageType.GET_RESP, request_id, b"\x00\x01k\x02ok")
        self._pending += ROM_BANNER + framing.encode(answer)

    def read(self, size):
        chunk, self._pending = self._pending[:size], self._pending[size:]
        return chunk

    def close(self):
        pass


class ClientRawLogTest(unittest.TestCase):
    def test_client_logs_boot_text_and_still_answers(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "raw.log")
            with EdgeKV("fake", transport=FakeBridge(), raw_log=RawLog(path), timeout=2.0) as kv:
                self.assertEqual(kv.get(b"k"), b"ok")
                self.assertEqual(kv.get(b"k"), b"ok")
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
        self.assertEqual(text.count(f"discarded {len(ROM_BANNER)} bytes (no sync)"), 2)
        self.assertIn("ESP-ROM:esp32c3", text)

    def test_cli_option(self):
        args = cli.build_parser().parse_args(["--raw-log", "boot.log", "ping"])
        self.assertEqual(args.raw_log, "boot.log")
        self.assertIsNone(cli.build_parser().parse_args(["ping"]).raw_log)


if __name__ == "__main__":
    unittest.main()

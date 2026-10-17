import struct
import unittest

from edgekv import protocol, reading
from edgekv.protocol import MessageType, Status


def response(msg_type, request_id, payload, sender=1, hlc=(5000 << 16) | 3):
    header = protocol.HEADER.pack(1, int(msg_type), 0, 22, request_id, sender, sender, sender, 0, hlc, len(payload))
    return header + payload


class ProtocolTest(unittest.TestCase):
    def test_header_layout(self):
        packet = protocol.build_set(0xAABBCCDD, b"k", b"vv", node_id=2)
        self.assertEqual(len(packet), 22 + 1 + 1 + 1 + 2)
        header = protocol.parse_header(packet)
        self.assertEqual(header.msg_type, MessageType.SET_REQ)
        self.assertEqual(header.request_id, 0xAABBCCDD)
        self.assertEqual((header.origin, header.sender, header.destination), (2, 2, 2))
        self.assertEqual(packet[22:], b"\x01k\x02vv")

    def test_ping_is_broadcast(self):
        header = protocol.parse_header(protocol.build_ping())
        self.assertEqual(header.msg_type, MessageType.HEARTBEAT)
        self.assertEqual(header.destination, 0xFF)

    def test_parse_get_response(self):
        parsed = protocol.parse_response(response(MessageType.GET_RESP, 7, b"\x00\x03key\x05value"))
        self.assertEqual(parsed.status, Status.OK)
        self.assertEqual(parsed.key, b"key")
        self.assertEqual(parsed.value, b"value")
        self.assertEqual(protocol.hlc_physical_ms(parsed.header.hlc), 5000)
        self.assertEqual(protocol.hlc_logical(parsed.header.hlc), 3)

    def test_parse_ack(self):
        parsed = protocol.parse_response(response(MessageType.SET_ACK, 8, b"\x07\x01k"))
        self.assertEqual(parsed.status, Status.REPLICATION_FAILED)
        self.assertIn(parsed.status, protocol.COMMITTED)

    def test_rejects(self):
        with self.assertRaises(ValueError):
            protocol.build_get(1, b"")
        with self.assertRaises(ValueError):
            protocol.build_get(1, b"x" * 33)
        with self.assertRaises(ValueError):
            protocol.build_set(1, b"k", b"v" * 193)
        with self.assertRaises(ValueError):
            protocol.parse_header(b"\x01" * 10)


class ReadingTest(unittest.TestCase):
    def test_decode(self):
        data = struct.pack(">BBBBIiQ", 1, 3, 1, 0, 123456, -40250, 0x0102030405060708)
        sample = reading.decode(data)
        self.assertEqual(sample.kind, "pressure")
        self.assertEqual(sample.unit, "hPa")
        self.assertAlmostEqual(sample.value, -40.25)
        self.assertEqual(sample.sequence, 123456)
        self.assertTrue(sample.simulated)
        self.assertIsNone(reading.decode(b"hello, this is text!"))
        self.assertIsNone(reading.decode(data[:-1]))

    def test_decode_event_kinds(self):
        # Same bytes as test/sensor_test.cpp test_codec_event_kinds.
        data = struct.pack(">BBBBIiQi", 2, 5, 2, 0, 7, 3000, 0x0102030405060708, -1)
        sample = reading.decode(data)
        self.assertEqual(sample.kind, "motion")
        self.assertEqual(sample.unit, "events")
        self.assertEqual(sample.events, 3)
        self.assertTrue(sample.settling)
        self.assertFalse(sample.simulated)
        self.assertIsNone(sample.last_motion_s)

        sample = reading.decode(struct.pack(">BBBBIiQi", 2, 5, 0, 0, 8, 2000, 1, 12))
        self.assertFalse(sample.settling)
        self.assertEqual(sample.last_motion_s, 12)
        self.assertIsNone(sample.active_pct)

        sample = reading.decode(struct.pack(">BBBBIiQi", 2, 6, 1, 0, 1, 5000, 42, 37500))
        self.assertEqual(sample.kind, "sound")
        self.assertEqual(sample.events, 5)
        self.assertAlmostEqual(sample.active_pct, 37.5)
        self.assertTrue(sample.simulated)

        # Format byte and length must agree.
        self.assertIsNone(reading.decode(data[:20]))
        self.assertIsNone(reading.decode(b"\x01" + data[1:]))
        self.assertIsNone(reading.decode(struct.pack(">BBBBIiQi", 2, 9, 0, 0, 1, 0, 0, 0)))

        stale = reading.decode(struct.pack(">BBBBIiQ", 1, 1, 0x04, 0, 9, 21500, 5))
        self.assertTrue(stale.stale)
        self.assertFalse(stale.error)
        self.assertEqual(stale.status, "stale")
        error = reading.decode(struct.pack(">BBBBIiQ", 1, 1, 0x08, 0, 9, 0, 5))
        self.assertEqual(error.status, "error")

        # Level kinds stay v1, without aux.
        level = reading.decode(struct.pack(">BBBBIiQ", 1, 4, 0, 0, 1, 320000, 5))
        self.assertIsNone(level.aux)
        self.assertIsNone(level.events)

    def test_keys(self):
        self.assertEqual(reading.latest_key("A"), b"s/A/temp")
        self.assertEqual(reading.latest_key("B"), b"s/B/motion")
        self.assertEqual(reading.history_key("C", 5), b"h/C/sound/1")
        self.assertEqual(reading.history_key("D", 6), b"h/D/light/2")


if __name__ == "__main__":
    unittest.main()

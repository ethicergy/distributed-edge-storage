import unittest

from edgekv import framing, protocol

# Same bytes as test/uart_frame_test.cpp: GET_REQ "s/A/temp", request
# id 0x01020304, node ids 1, HLC 0.
SHARED_VECTOR = bytes.fromhex(
    "aa551f"
    "01030016010203040101010000000000000000000009"
    "08732f412f74656d70"
    "5d"
)


class FramingTest(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(framing.crc8(b"123456789"), 0xF4)

    def test_shared_vector(self):
        packet = protocol.build_get(0x01020304, b"s/A/temp", node_id=1)
        self.assertEqual(framing.encode(packet), SHARED_VECTOR)
        self.assertEqual(framing.Decoder().feed(SHARED_VECTOR), [packet])

    def test_noise_and_split_delivery(self):
        a = protocol.build_get(1, b"alpha")
        b = protocol.build_set(2, b"beta", b"value")
        stream = b"\x00\xaa\x13\x55\xaa\xaa" + framing.encode(a) + b"\x42" + framing.encode(b)
        decoder = framing.Decoder()
        frames = []
        for byte in stream:
            frames += decoder.feed(bytes([byte]))
        self.assertEqual(frames, [a, b])

    def test_bad_crc_resync(self):
        bad = bytearray(framing.encode(protocol.build_get(7, b"corrupt")))
        bad[10] ^= 1
        good = protocol.build_get(8, b"survivor")
        decoder = framing.Decoder()
        self.assertEqual(decoder.feed(bytes(bad) + framing.encode(good)), [good])
        self.assertEqual(decoder.crc_errors, 1)

    def test_bogus_length_then_idle(self):
        real = protocol.build_get(9, b"hidden")
        decoder = framing.Decoder()
        self.assertEqual(decoder.feed(b"\xaa\x55\xf0" + framing.encode(real)), [])
        self.assertEqual(decoder.abandon_partial(), [real])

    def test_limits(self):
        with self.assertRaises(ValueError):
            framing.encode(b"\x00" * 21)
        with self.assertRaises(ValueError):
            framing.encode(b"\x00" * 251)


if __name__ == "__main__":
    unittest.main()

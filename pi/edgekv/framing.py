"""UART framing between the Raspberry Pi and the bridge node.

    AA 55 | LEN | ENVELOPE (LEN bytes, 22..250) | CRC

CRC is CRC-8/SMBUS (poly 0x07, init 0x00, no reflection, no final
xor) over LEN and ENVELOPE. Mirrors include/uart_frame.h.
"""

from __future__ import annotations

from typing import Iterator, List

SYNC = b"\xaa\x55"
MIN_ENVELOPE = 22
MAX_ENVELOPE = 250


def crc8(data: bytes, crc: int = 0x00) -> int:
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode(envelope: bytes) -> bytes:
    if not MIN_ENVELOPE <= len(envelope) <= MAX_ENVELOPE:
        raise ValueError(f"envelope length {len(envelope)} outside {MIN_ENVELOPE}..{MAX_ENVELOPE}")
    body = bytes([len(envelope)]) + envelope
    return SYNC + body + bytes([crc8(body)])


class Decoder:
    """Byte-stream decoder with resynchronisation.

    On a bad length or CRC only the first sync byte is discarded and
    the scan restarts after it, exactly like the firmware decoder.
    """

    def __init__(self) -> None:
        self._buffer = bytearray()
        self.frames = 0
        self.crc_errors = 0
        self.length_errors = 0
        self.discarded = 0

    def feed(self, data: bytes) -> List[bytes]:
        self._buffer.extend(data)
        return list(self._drain())

    def abandon_partial(self) -> List[bytes]:
        """Line went idle with a partial frame buffered: give it up."""
        if not self._buffer:
            return []
        del self._buffer[0]
        self.discarded += 1
        return list(self._drain())

    def _drain(self) -> Iterator[bytes]:
        buf = self._buffer
        while True:
            start = buf.find(SYNC)
            if start < 0:
                # Keep a trailing AA: it may be the start of a frame.
                keep = 1 if buf.endswith(b"\xaa") else 0
                self.discarded += len(buf) - keep
                del buf[: len(buf) - keep]
                return
            if start:
                self.discarded += start
                del buf[:start]
            if len(buf) < 3:
                return
            length = buf[2]
            if not MIN_ENVELOPE <= length <= MAX_ENVELOPE:
                self.length_errors += 1
                self.discarded += 1
                del buf[0]
                continue
            if len(buf) < length + 4:
                return
            if crc8(bytes(buf[2 : 3 + length])) != buf[3 + length]:
                self.crc_errors += 1
                self.discarded += 1
                del buf[0]
                continue
            envelope = bytes(buf[3 : 3 + length])
            del buf[: length + 4]
            self.frames += 1
            yield envelope

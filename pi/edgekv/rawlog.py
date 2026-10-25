"""--raw-log: record the bytes the frame decoder throws away.

Typical content: node A's ROM boot text after a reset, line noise, a
wrong baud rate (only bad frames), or a login prompt from a Pi whose
serial console is still enabled. Consecutive discards are merged into
one entry, written when a good frame arrives, the line goes idle, or
the log is closed:

    2026-10-18T10:00:00.123+02:00 discarded 312 bytes (no sync)
      text: 'ESP-ROM:esp32c3-api1-20210207\\r\\nBuild:Feb  7 2021\\r\\n...'
      hex:  45 53 50 2d 52 4f 4d ...
"""

from __future__ import annotations

from datetime import datetime
from typing import IO, Dict, Optional


class RawLog:
    def __init__(self, path: str, stream: Optional[IO[str]] = None) -> None:
        self._handle = stream if stream is not None else open(path, "a", encoding="utf-8")
        self._pending = bytearray()
        self._reasons: Dict[str, int] = {}
        self.entries = 0
        self.bytes = 0

    def add(self, data: bytes, reason: str) -> None:
        self._pending.extend(data)
        self._reasons[reason] = self._reasons.get(reason, 0) + len(data)

    def flush(self) -> None:
        if not self._pending:
            return
        data = bytes(self._pending)
        reasons = ", ".join(
            reason if len(self._reasons) == 1 else f"{reason} {count}"
            for reason, count in self._reasons.items()
        )
        stamp = datetime.now().astimezone().isoformat(timespec="milliseconds")
        self._handle.write(f"{stamp} discarded {len(data)} bytes ({reasons})\n")
        self._handle.write(f"  text: {_printable(data)}\n")
        self._handle.write(f"  hex:  {data.hex(' ')}\n")
        self._handle.flush()
        self.entries += 1
        self.bytes += len(data)
        self._pending.clear()
        self._reasons.clear()

    def close(self) -> None:
        self.flush()
        self._handle.close()


def _printable(data: bytes) -> str:
    return repr(data.decode("latin-1"))

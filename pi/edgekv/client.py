"""Client for the bridge node, over a serial port (pyserial)."""

from __future__ import annotations

import random
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Callable, Dict, List, Optional

from . import framing, protocol, reading
from .protocol import COMMITTED, MessageType, Response, Status


class EdgeKVError(Exception):
    """The cluster answered, but not with success."""

    def __init__(self, operation: str, key: bytes, status: Status):
        super().__init__(f"{operation} {key.decode(errors='replace')}: {status.name}")
        self.status = status


@dataclass
class WriteResult:
    status: Status

    @property
    def under_replicated(self) -> bool:
        """Committed on the primary only; the replica is repaired by
        anti-entropy (decision D5)."""
        return self.status == Status.REPLICATION_FAILED


class EdgeKV:
    """Talks to the cluster through the bridge node's UART.

    Each request carries a Pi-chosen request id that the bridge echoes.
    A request that gets no answer is re-sent once with the same id.
    Every answer carries the bridge's HLC, which is used to convert
    sample timestamps (HLC) into wall-clock time on the Pi.
    """

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        timeout: float = 10.0,
        retries: int = 1,
        transport=None,
    ) -> None:
        if transport is None:
            import serial  # pyserial; imported lazily so tests need not have it

            transport = serial.serial_for_url(port, baudrate=baudrate, timeout=0.05)

        self._io = transport
        self._timeout = timeout
        self._retries = retries
        self._decoder = framing.Decoder()
        self._next_id = random.randint(1, 0x7FFFFFFF)
        self.bridge_node: Optional[int] = None
        # wall-clock ms minus HLC physical ms, from the latest answer
        self._clock_offset_ms: Optional[int] = None

    # -------------------------------------------------- plumbing

    def close(self) -> None:
        self._io.close()

    def __enter__(self) -> "EdgeKV":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _request_id(self) -> int:
        self._next_id = (self._next_id % 0xFFFFFFFF) + 1
        return self._next_id

    def _calibrate(self, response: Response) -> None:
        if response.header.hlc:
            self.bridge_node = response.header.sender
            self._clock_offset_ms = int(time.time() * 1000) - protocol.hlc_physical_ms(response.header.hlc)

    def _transact(self, packet: bytes, match: Callable[[Response], bool]) -> Response:
        frame = framing.encode(packet)

        for _ in range(self._retries + 1):
            self._io.write(frame)
            deadline = time.monotonic() + self._timeout
            idle_since = time.monotonic()

            while time.monotonic() < deadline:
                data = self._io.read(256)

                if not data:
                    if time.monotonic() - idle_since > 0.1:
                        envelopes = self._decoder.abandon_partial()
                        idle_since = time.monotonic()
                    else:
                        continue
                else:
                    idle_since = time.monotonic()
                    envelopes = self._decoder.feed(data)

                for envelope in envelopes:
                    try:
                        response = protocol.parse_response(envelope)
                    except (ValueError, IndexError):
                        continue
                    self._calibrate(response)
                    if match(response):
                        return response

        raise TimeoutError("no answer from the bridge node")

    def _keyed(self, msg_type: MessageType, builder, *args) -> Response:
        request_id = self._request_id()
        packet = builder(request_id, *args)
        return self._transact(
            packet,
            lambda r: r.header.request_id == request_id and r.header.msg_type == msg_type,
        )

    # -------------------------------------------------- operations

    def ping(self) -> int:
        """Returns the bridge's node id (1..4)."""
        response = self._transact(protocol.build_ping(), lambda r: r.header.msg_type == MessageType.HEARTBEAT)
        return response.header.sender

    def set(self, key: bytes, value: bytes) -> WriteResult:
        response = self._keyed(MessageType.SET_ACK, protocol.build_set, key, value)
        if response.status not in COMMITTED:
            raise EdgeKVError("SET", key, response.status)
        return WriteResult(response.status)

    def get(self, key: bytes) -> Optional[bytes]:
        response = self._keyed(MessageType.GET_RESP, protocol.build_get, key)
        if response.status == Status.NOT_FOUND:
            return None
        if response.status != Status.OK:
            raise EdgeKVError("GET", key, response.status)
        return response.value

    def delete(self, key: bytes) -> WriteResult:
        response = self._keyed(MessageType.DEL_ACK, protocol.build_del, key)
        if response.status not in COMMITTED:
            raise EdgeKVError("DEL", key, response.status)
        return WriteResult(response.status)

    # -------------------------------------------------- sensors

    def sensor(self, node: str) -> Optional[reading.Reading]:
        data = self.get(reading.latest_key(node))
        return reading.decode(data) if data is not None else None

    def sensors(self) -> Dict[str, Optional[reading.Reading]]:
        return {node: self.sensor(node) for node in reading.NODE_SENSORS}

    def history(self, node: str) -> List[reading.Reading]:
        samples = []
        for slot in range(reading.HISTORY_SLOTS):
            data = self.get(reading.history_key(node, slot))
            decoded = reading.decode(data) if data is not None else None
            if decoded is not None:
                samples.append(decoded)
        return sorted(samples, key=lambda r: r.sequence)

    def wall_time(self, hlc: int) -> Optional[datetime]:
        """Wall-clock time of an HLC timestamp (D4).

        The physical part of the cluster's HLC is a shared millisecond
        clock with an arbitrary epoch; the bridge's answers tell us the
        offset to the Pi's clock (accurate to the UART latency).
        """
        if self._clock_offset_ms is None:
            return None
        ms = protocol.hlc_physical_ms(hlc) + self._clock_offset_ms
        return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).astimezone()

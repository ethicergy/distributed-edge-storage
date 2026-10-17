"""Sensor readings stored by the nodes (include/sensor.h, format v1).

Key schema:
    s/<node>/<kind>          latest reading,   e.g. s/A/temp
    h/<node>/<kind>/<slot>   history ring slot, slot = sequence % 4
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

from .protocol import hlc_physical_ms

READING = struct.Struct(">BBBBIiQ")
READING_FORMAT = 0x01
FLAG_SIMULATED = 0x01
HISTORY_SLOTS = 4

KINDS = {1: ("temp", "degC"), 2: ("humidity", "%RH"), 3: ("pressure", "hPa"), 4: ("light", "lux")}

# Default node -> sensor assignment (decision D1 placeholder).
NODE_SENSORS = {"A": "temp", "B": "humidity", "C": "pressure", "D": "light"}


@dataclass
class Reading:
    kind: str
    unit: str
    value: float
    sequence: int
    hlc: int
    simulated: bool

    @property
    def hlc_ms(self) -> int:
        return hlc_physical_ms(self.hlc)


def decode(data: bytes) -> Optional[Reading]:
    if len(data) != READING.size:
        return None
    fmt, kind, flags, reserved, sequence, value_milli, hlc = READING.unpack(data)
    if fmt != READING_FORMAT or kind not in KINDS or reserved != 0:
        return None
    name, unit = KINDS[kind]
    return Reading(name, unit, value_milli / 1000.0, sequence, hlc, bool(flags & FLAG_SIMULATED))


def latest_key(node: str) -> bytes:
    return f"s/{node}/{NODE_SENSORS[node]}".encode()


def history_key(node: str, slot: int) -> bytes:
    return f"h/{node}/{NODE_SENSORS[node]}/{slot % HISTORY_SLOTS}".encode()

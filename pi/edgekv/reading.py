"""Sensor readings stored by the nodes (include/sensor.h).

Key schema:
    s/<node>/<kind>          latest reading,   e.g. s/D/temp
    h/<node>/<kind>/<slot>   history ring slot, slot = sequence % 4

Formats:
    v1, 20 bytes: level kinds (temp, humidity, pressure, light)
    v2, 24 bytes: event kinds (motion, sound), v1 layout plus an i32 aux
        motion: seconds from the last detection to the sample (-1: none)
        sound:  milli-percent of the window the output was active
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

from .protocol import hlc_physical_ms

READING = struct.Struct(">BBBBIiQ")
READING_AUX = struct.Struct(">BBBBIiQi")
READING_FORMAT = 0x01
READING_FORMAT_AUX = 0x02
FLAG_SIMULATED = 0x01
FLAG_SETTLING = 0x02
FLAG_STALE = 0x04  # no good hardware read lately; value is the last good one
FLAG_ERROR = 0x08  # no good hardware read since boot; value meaningless
AUX_NONE = -1
HISTORY_SLOTS = 4

KINDS = {
    1: ("temp", "degC"),
    2: ("humidity", "%RH"),
    3: ("pressure", "hPa"),
    4: ("light", "lux"),
    5: ("motion", "events"),
    6: ("sound", "events"),
}
EVENT_KINDS = {"motion", "sound"}

# Node -> sensor (D1): A BH1750, B AM312 PIR, C LM393 sound, D DHT11.
NODE_SENSORS = {"A": "light", "B": "motion", "C": "sound", "D": "temp"}


@dataclass
class Reading:
    kind: str
    unit: str
    value: float
    sequence: int
    hlc: int
    simulated: bool
    aux: Optional[int] = None
    settling: bool = False
    stale: bool = False
    error: bool = False

    @property
    def hlc_ms(self) -> int:
        return hlc_physical_ms(self.hlc)

    @property
    def status(self) -> str:
        if self.error:
            return "error"
        if self.stale:
            return "stale"
        if self.settling:
            return "settling"
        return "ok"

    @property
    def events(self) -> Optional[int]:
        return int(self.value) if self.kind in EVENT_KINDS else None

    @property
    def last_motion_s(self) -> Optional[int]:
        """Seconds from the last detection to the sample time."""
        if self.kind != "motion" or self.settling or self.aux is None or self.aux == AUX_NONE:
            return None
        return self.aux

    @property
    def active_pct(self) -> Optional[float]:
        if self.kind != "sound" or self.aux is None:
            return None
        return self.aux / 1000.0


def decode(data: bytes) -> Optional[Reading]:
    if len(data) == READING.size and data[0] == READING_FORMAT:
        fmt, kind, flags, reserved, sequence, value_milli, hlc = READING.unpack(data)
        aux = None
    elif len(data) == READING_AUX.size and data[0] == READING_FORMAT_AUX:
        fmt, kind, flags, reserved, sequence, value_milli, hlc, aux = READING_AUX.unpack(data)
    else:
        return None
    if kind not in KINDS or reserved != 0:
        return None
    name, unit = KINDS[kind]
    return Reading(
        name, unit, value_milli / 1000.0, sequence, hlc,
        bool(flags & FLAG_SIMULATED), aux, bool(flags & FLAG_SETTLING),
        bool(flags & FLAG_STALE), bool(flags & FLAG_ERROR),
    )


def latest_key(node: str) -> bytes:
    return f"s/{node}/{NODE_SENSORS[node]}".encode()


def history_key(node: str, slot: int) -> bytes:
    return f"h/{node}/{NODE_SENSORS[node]}/{slot % HISTORY_SLOTS}".encode()

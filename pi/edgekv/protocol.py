"""The frozen 22-byte packet header and the payloads the Pi uses.

Mirrors include/protocol.h (big-endian on the wire):

    0  protocol_version (0x01)    9  sender_node_id
    1  msg_type                  10  destination_node_id
    2  flags                     11  ttl (0)
    3  header_len (22)           12  hlc (u64)
    4  request_id (u32)          20  payload_len (u16)
    8  origin_node_id
"""

from __future__ import annotations

import enum
import struct
from dataclasses import dataclass

PROTOCOL_VERSION = 0x01
HEADER = struct.Struct(">BBBBIBBBBQH")
HEADER_SIZE = HEADER.size
KEY_MAX = 32
VALUE_MAX = 192

assert HEADER_SIZE == 22


class MessageType(enum.IntEnum):
    SET_REQ = 0x01
    SET_ACK = 0x02
    GET_REQ = 0x03
    GET_RESP = 0x04
    DEL_REQ = 0x05
    DEL_ACK = 0x06
    REPL_REQ = 0x07
    REPL_ACK = 0x08
    HEARTBEAT = 0x09
    VERSION_TABLE = 0x0A
    RESYNC_DATA = 0x0B


class Status(enum.IntEnum):
    OK = 0x00
    NOT_FOUND = 0x01
    INVALID_REQUEST = 0x02
    KEY_TOO_LARGE = 0x03
    VALUE_TOO_LARGE = 0x04
    STORAGE_ERROR = 0x05
    NOT_RESPONSIBLE = 0x06
    REPLICATION_FAILED = 0x07
    STALE_VERSION = 0x08
    UNSUPPORTED = 0x09


# Through the bridge, REPLICATION_FAILED means "committed on the
# primary only; anti-entropy will copy it" (decision D5), and
# NOT_RESPONSIBLE means "the cluster could not serve it right now".
COMMITTED = {Status.OK, Status.REPLICATION_FAILED}


@dataclass
class Header:
    msg_type: MessageType
    flags: int
    request_id: int
    origin: int
    sender: int
    destination: int
    hlc: int
    payload_len: int


@dataclass
class Response:
    header: Header
    status: Status
    key: bytes = b""
    value: bytes = b""


def hlc_physical_ms(hlc: int) -> int:
    return (hlc >> 16) & ((1 << 48) - 1)


def hlc_logical(hlc: int) -> int:
    return hlc & 0xFFFF


def _key(key: bytes) -> bytes:
    if not 1 <= len(key) <= KEY_MAX:
        raise ValueError(f"key must be 1..{KEY_MAX} bytes")
    return bytes([len(key)]) + key


def build(msg_type: MessageType, request_id: int, payload: bytes = b"", node_id: int = 1) -> bytes:
    """Complete packet. Node id fields are ignored by the bridge but
    must be valid ids (1..4); heartbeats go to broadcast (0xFF)."""
    destination = 0xFF if msg_type == MessageType.HEARTBEAT else node_id
    header = HEADER.pack(
        PROTOCOL_VERSION, int(msg_type), 0, HEADER_SIZE,
        request_id & 0xFFFFFFFF, node_id, node_id, destination, 0, 0, len(payload),
    )
    return header + payload


def build_set(request_id: int, key: bytes, value: bytes, node_id: int = 1) -> bytes:
    if len(value) > VALUE_MAX:
        raise ValueError(f"value must be at most {VALUE_MAX} bytes")
    return build(MessageType.SET_REQ, request_id, _key(key) + bytes([len(value)]) + value, node_id)


def build_get(request_id: int, key: bytes, node_id: int = 1) -> bytes:
    return build(MessageType.GET_REQ, request_id, _key(key), node_id)


def build_del(request_id: int, key: bytes, node_id: int = 1) -> bytes:
    return build(MessageType.DEL_REQ, request_id, _key(key), node_id)


def build_ping(node_id: int = 1) -> bytes:
    return build(MessageType.HEARTBEAT, 0, b"", node_id)


def parse_header(packet: bytes) -> Header:
    if len(packet) < HEADER_SIZE:
        raise ValueError("packet shorter than header")
    (version, msg_type, flags, header_len, request_id, origin, sender,
     destination, ttl, hlc, payload_len) = HEADER.unpack_from(packet)
    if version != PROTOCOL_VERSION or header_len != HEADER_SIZE or ttl != 0:
        raise ValueError("unsupported header")
    if len(packet) != HEADER_SIZE + payload_len:
        raise ValueError("payload length mismatch")
    return Header(MessageType(msg_type), flags, request_id, origin, sender, destination, hlc, payload_len)


def parse_response(packet: bytes) -> Response:
    header = parse_header(packet)
    payload = packet[HEADER_SIZE:]

    if header.msg_type == MessageType.HEARTBEAT:
        return Response(header, Status.OK)

    if header.msg_type not in (MessageType.SET_ACK, MessageType.GET_RESP, MessageType.DEL_ACK):
        raise ValueError(f"unexpected message type {header.msg_type.name}")

    status = Status(payload[0])
    key_len = payload[1]
    key = payload[2 : 2 + key_len]
    value = b""

    if header.msg_type == MessageType.GET_RESP:
        offset = 2 + key_len
        value_len = payload[offset]
        value = payload[offset + 1 : offset + 1 + value_len]

    return Response(header, status, bytes(key), bytes(value))

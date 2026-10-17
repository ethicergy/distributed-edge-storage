#pragma once

#include <cstddef>
#include <cstdint>

#include "protocol.h"

/*
 * Builders for complete wire packets (22-byte header + payload).
 * Pure helpers on top of protocol.h; no transport, no state.
 */
namespace packet {

struct Buffer {
    std::uint8_t data[protocol::MAX_PACKET_SIZE];
    std::size_t length;
};

struct Route {
    std::uint32_t request_id;
    protocol::NodeId origin;
    protocol::NodeId sender;
    protocol::NodeId destination;
    protocol::Hlc hlc;
    std::uint8_t flags;
};

// SET_REQ, REPL_REQ, RESYNC_DATA (key + value);
// GET_REQ, DEL_REQ (key only, value ignored).
bool build_request(
    protocol::MessageType type,
    const Route& route,
    protocol::KeyView key,
    protocol::ValueView value,
    Buffer& out
);

// SET_ACK, GET_RESP (value used), DEL_ACK, REPL_ACK.
bool build_response(
    protocol::MessageType type,
    const Route& route,
    protocol::StatusCode status,
    protocol::KeyView key,
    protocol::ValueView value,
    Buffer& out
);

bool build_heartbeat(
    protocol::NodeId sender,
    protocol::Hlc hlc,
    Buffer& out
);

bool build_version_table(
    protocol::NodeId sender,
    protocol::Hlc hlc,
    const protocol::VersionTablePayload& table,
    Buffer& out
);

const char* to_string(protocol::MessageType type);
const char* to_string(protocol::StatusCode status);

} // namespace packet

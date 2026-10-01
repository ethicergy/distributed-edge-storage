#pragma once

#include <cstddef>
#include <cstdint>

#include "request_identity.h"
#include "protocol.h"

namespace request {

constexpr std::size_t RELIABLE_PACKET_MAX =
    protocol::MAX_PACKET_SIZE;

enum class PacketResult : std::uint8_t {
    OK = 0,
    INVALID_ARGUMENT,
    INVALID_PACKET,
    NOT_RELIABLE,
    BUFFER_TOO_SMALL
};

PacketResult extract_identity(
    const protocol::PacketView& packet,
    Identity& identity
);

PacketResult prepare_retry(
    const std::uint8_t* packet,
    std::size_t packet_length,
    std::uint8_t* retry_packet,
    std::size_t retry_capacity,
    std::size_t& retry_length
);

bool same_request_content(
    const std::uint8_t* original_packet,
    std::size_t original_length,
    const std::uint8_t* retry_packet,
    std::size_t retry_length
);

} // namespace request
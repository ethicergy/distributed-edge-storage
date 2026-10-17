#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "node_id.h"

namespace transport {

constexpr std::size_t MAX_FRAME_SIZE = 250;
constexpr std::size_t MAC_ADDRESS_SIZE = 6;
constexpr std::size_t RX_QUEUE_DEPTH = config::RADIO_RX_QUEUE_DEPTH;

enum class Result : std::uint8_t {
    OK = 0x00,
    INVALID_ARGUMENT,
    NOT_INITIALIZED,
    INVALID_NODE,
    FRAME_TOO_LARGE,
    PEER_NOT_FOUND,
    QUEUE_FULL,
    SEND_FAILED
};

struct ReceivedFrame {
    std::uint8_t data[MAX_FRAME_SIZE];
    std::uint8_t length;
    std::uint8_t source_mac[MAC_ADDRESS_SIZE];

    // Resolved from the static MAC table in the RX callback;
    // node::INVALID for an unknown MAC (those frames are dropped).
    node::Id source_node;
};

struct Stats {
    std::uint32_t rx_frames;
    std::uint32_t rx_queue_overflows;
    std::uint32_t rx_unknown_mac;
    std::uint32_t tx_status_ok[node::MAX - node::MIN + 1];
    std::uint32_t tx_status_failed[node::MAX - node::MIN + 1];
};

struct SendStatus {
    node::Id destination;
    bool success;
};

class EspNowTransport {
public:
    Result begin(node::Id local_node_id);

    Result send_unicast(
        node::Id destination,
        const std::uint8_t* data,
        std::size_t length
    );

    Result send_broadcast(
        const std::uint8_t* data,
        std::size_t length
    );

    bool receive(ReceivedFrame& frame);

    bool receive_send_status(SendStatus& status);

    // Drains the TX-status queue into the per-peer counters.
    void drain_send_status();

    Stats stats() const;
};

} // namespace transport
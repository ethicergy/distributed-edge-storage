#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "node_runtime.h"
#include "uart_frame.h"

/*
 * Raspberry Pi bridge role (work plan P8, decision D7).
 *
 * The Pi is not a node. It sends ordinary protocol packets inside
 * UART frames:
 *
 *   SET_REQ / GET_REQ / DEL_REQ  -> executed through the node's client
 *                                   API (same routing, replication and
 *                                   retries as any other request)
 *   HEARTBEAT                    -> ping; answered with a HEARTBEAT
 *                                   that carries the bridge's node id
 *                                   and current HLC (time calibration)
 *
 * The Pi's request_id is echoed in the answer so the Pi can match
 * responses; the bridge gives the cluster request its own identity
 * (origin = bridge node), so the Pi needs no node id. The node id
 * fields the Pi sends must be valid ids but are otherwise ignored.
 *
 * Response status for outcomes the frozen status table has no code
 * for: UNDER_REPLICATED -> REPLICATION_FAILED (write committed);
 * NOT_READY / BUSY / NO_ROUTE / TIMEOUT -> NOT_RESPONSIBLE (cluster
 * could not serve it now; retry).
 */
namespace bridge {

using WriteFn = void (*)(void* context, const std::uint8_t* data, std::size_t length);

struct Stats {
    std::uint32_t requests;
    std::uint32_t responses;
    std::uint32_t pings;
    std::uint32_t rejected_busy;
    std::uint32_t unsupported;
    std::uint32_t invalid;
};

class Bridge {
public:
    Bridge(edge::Runtime& runtime, WriteFn write, void* write_context);

    // Bytes received from the Pi.
    void on_bytes(const std::uint8_t* data, std::size_t length, std::uint64_t now_ms);

    // Call periodically; abandons a stalled partial frame.
    void tick(std::uint64_t now_ms);

    const Stats& stats() const { return stats_; }
    const uart::DecoderStats& decoder_stats() const { return decoder_.stats(); }

private:
    struct Pending {
        bool used;
        std::uint32_t pi_request_id;
        protocol::MessageType request_type;
    };

    static void on_complete(void* context, std::uint32_t tag, const edge::ClientResult& result);

    void handle_envelope(const std::uint8_t* envelope, std::size_t length, std::uint64_t now_ms);

    void respond(
        protocol::MessageType request_type,
        std::uint32_t pi_request_id,
        protocol::StatusCode status,
        const std::uint8_t* key,
        std::size_t key_length,
        const std::uint8_t* value,
        std::size_t value_length
    );

    void send_envelope(const std::uint8_t* envelope, std::size_t length);

    edge::Runtime& runtime_;
    WriteFn write_;
    void* write_context_;

    uart::Decoder decoder_;
    std::uint64_t last_byte_ms_ = 0;

    Pending pending_[config::BRIDGE_MAX_INFLIGHT]{};
    Stats stats_{};
};

protocol::StatusCode to_status(edge::ClientStatus status);

} // namespace bridge

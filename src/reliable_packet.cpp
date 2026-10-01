#include "reliable_packet.h"

#include <cstring>

namespace request {

namespace {

bool is_reliable_message(
    protocol::MessageType type
)
{
    switch (type) {
        case protocol::MessageType::SET_REQ:
        case protocol::MessageType::GET_REQ:
        case protocol::MessageType::DEL_REQ:
        case protocol::MessageType::REPL_REQ:
            return true;

        default:
            return false;
    }
}

} // namespace

PacketResult extract_identity(
    const protocol::PacketView& packet,
    Identity& identity
)
{
    identity = {};

    if (!is_reliable_message(
            packet.header.msg_type
        )) {

        return PacketResult::NOT_RELIABLE;
    }

    if (packet.header.origin_node_id <
            node::MIN ||
        packet.header.origin_node_id >
            node::MAX) {

        return PacketResult::INVALID_PACKET;
    }

    if (packet.header.request_id == 0) {
        return PacketResult::INVALID_PACKET;
    }

    identity = Identity{
        packet.header.origin_node_id,
        packet.header.request_id
    };

    return PacketResult::OK;
}

PacketResult prepare_retry(
    const std::uint8_t* packet,
    std::size_t packet_length,
    std::uint8_t* retry_packet,
    std::size_t retry_capacity,
    std::size_t& retry_length
)
{
    retry_length = 0;

    if (packet == nullptr ||
        retry_packet == nullptr) {

        return PacketResult::INVALID_ARGUMENT;
    }

    protocol::PacketView view{};

    const protocol::Result decode_result =
        protocol::deserialize_packet(
            packet,
            packet_length,
            view
        );

    if (decode_result !=
        protocol::Result::OK) {

        return PacketResult::INVALID_PACKET;
    }

    if (!is_reliable_message(
            view.header.msg_type
        )) {

        return PacketResult::NOT_RELIABLE;
    }

    /*
     * A packet marked as RETRY is already a retry.
     * We do not need to manufacture another state.
     */
    view.header.flags |=
        protocol::FLAG_RETRY;

    /*
     * Re-serialize exactly the same header/payload,
     * except for the RETRY flag.
     */
    const protocol::Result serialize_result =
        protocol::serialize_packet(
            view.header,
            view.payload,
            retry_packet,
            retry_capacity,
            retry_length
        );

    if (serialize_result ==
        protocol::Result::BUFFER_TOO_SMALL) {

        return PacketResult::BUFFER_TOO_SMALL;
    }

    if (serialize_result !=
        protocol::Result::OK) {

        return PacketResult::INVALID_PACKET;
    }

    return PacketResult::OK;
}

bool same_request_content(
    const std::uint8_t* original_packet,
    std::size_t original_length,
    const std::uint8_t* retry_packet,
    std::size_t retry_length
)
{
    if (original_packet == nullptr ||
        retry_packet == nullptr) {

        return false;
    }

    protocol::PacketView original{};
    protocol::PacketView retry{};

    if (
        protocol::deserialize_packet(
            original_packet,
            original_length,
            original
        ) != protocol::Result::OK
    ) {
        return false;
    }

    if (
        protocol::deserialize_packet(
            retry_packet,
            retry_length,
            retry
        ) != protocol::Result::OK
    ) {
        return false;
    }

    /*
     * RETRY is the only allowed flag difference.
     */
    const std::uint8_t original_flags =
        original.header.flags;

    const std::uint8_t retry_flags =
        retry.header.flags;

    if (
        (retry_flags & ~protocol::FLAG_RETRY) !=
        original_flags
    ) {
        return false;
    }

    if (
        (retry_flags & protocol::FLAG_RETRY) == 0
    ) {
        return false;
    }

    /*
     * Everything except RETRY must remain identical.
     */
    if (
        original.header.protocol_version !=
            retry.header.protocol_version ||
        original.header.msg_type !=
            retry.header.msg_type ||
        original.header.header_len !=
            retry.header.header_len ||
        original.header.request_id !=
            retry.header.request_id ||
        original.header.origin_node_id !=
            retry.header.origin_node_id ||
        original.header.sender_node_id !=
            retry.header.sender_node_id ||
        original.header.destination_node_id !=
            retry.header.destination_node_id ||
        original.header.ttl !=
            retry.header.ttl ||
        original.header.hlc !=
            retry.header.hlc ||
        original.header.payload_len !=
            retry.header.payload_len
    ) {
        return false;
    }

    if (
        original.payload.len !=
        retry.payload.len
    ) {
        return false;
    }

    if (original.payload.len > 0) {

        if (original.payload.data == nullptr ||
            retry.payload.data == nullptr) {

            return false;
        }

        if (
            std::memcmp(
                original.payload.data,
                retry.payload.data,
                original.payload.len
            ) != 0
        ) {
            return false;
        }
    }

    return true;
}

} // namespace request
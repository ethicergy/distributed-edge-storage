#include "heartbeat.h"

namespace heartbeat {

protocol::Result build_packet(
    node::Id local_node_id,
    hlc::Hlc timestamp,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
)
{
    bytes_written = 0;

    if (local_node_id < node::MIN ||
        local_node_id > node::MAX) {

        return protocol::Result::INVALID_ARGUMENT;
    }

    if (out == nullptr) {
        return protocol::Result::INVALID_ARGUMENT;
    }

    protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::HEARTBEAT,
        0,
        static_cast<std::uint8_t>(
            protocol::HEADER_SIZE
        ),

        0,

        local_node_id,
        local_node_id,
        protocol::NODE_ID_BROADCAST,

        0,

        timestamp,

        0
    };

    return protocol::serialize_packet(
        header,
        protocol::ByteView{
            nullptr,
            0
        },
        out,
        out_capacity,
        bytes_written
    );
}

} // namespace heartbeat
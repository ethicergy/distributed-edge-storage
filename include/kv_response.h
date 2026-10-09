#pragma once

#include <cstddef>
#include <cstdint>

#include "kv_engine.h"
#include "node_id.h"
#include "protocol.h"

namespace kv {

class ResponseBuilder {
public:
    static bool build(
        const protocol::PacketHeader& request_header,
        node::Id local_node_id,
        protocol::Hlc response_hlc,
        const OperationResponse& response,
        std::uint8_t* output,
        std::size_t output_capacity,
        std::size_t& output_length
    );

private:
    static bool build_payload(
        protocol::MessageType response_type,
        const OperationResponse& response,
        std::uint8_t* output,
        std::size_t output_capacity,
        std::size_t& payload_length
    );

    static protocol::StatusCode to_protocol_status(
        Result result
    );

    static protocol::MessageType response_type_for(
        protocol::MessageType request_type
    );
};

} // namespace kv
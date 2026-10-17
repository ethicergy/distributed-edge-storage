#include "kv_response.h"

#include <cstddef>
#include <cstdint>

namespace kv {

protocol::MessageType ResponseBuilder::response_type_for(
    protocol::MessageType request_type
)
{
    switch (request_type) {
        case protocol::MessageType::SET_REQ:
            return protocol::MessageType::SET_ACK;

        case protocol::MessageType::GET_REQ:
            return protocol::MessageType::GET_RESP;

        case protocol::MessageType::DEL_REQ:
            return protocol::MessageType::DEL_ACK;

        case protocol::MessageType::REPL_REQ:
            return protocol::MessageType::REPL_ACK;

        default:
            return request_type;
    }
}

protocol::StatusCode ResponseBuilder::to_protocol_status(
    Result result
)
{
    switch (result) {
        case Result::OK:
            return protocol::StatusCode::OK;

        case Result::NOT_FOUND:
            return protocol::StatusCode::NOT_FOUND;

        case Result::INVALID_REQUEST:
            return protocol::StatusCode::INVALID_REQUEST;

        case Result::KEY_TOO_LARGE:
            return protocol::StatusCode::KEY_TOO_LARGE;

        case Result::VALUE_TOO_LARGE:
            return protocol::StatusCode::VALUE_TOO_LARGE;

        case Result::STORAGE_ERROR:
            return protocol::StatusCode::STORAGE_ERROR;

        case Result::NOT_RESPONSIBLE:
            return protocol::StatusCode::NOT_RESPONSIBLE;

        case Result::STALE_VERSION:
            return protocol::StatusCode::STALE_VERSION;

        // A node that cannot serve yet (booting, storage offline)
        // tells the client to use another owner.
        case Result::NOT_READY:
            return protocol::StatusCode::NOT_RESPONSIBLE;

        case Result::REPLICATION_FAILED:
            return protocol::StatusCode::REPLICATION_FAILED;
    }

    return protocol::StatusCode::INVALID_REQUEST;
}

bool ResponseBuilder::build_payload(
    protocol::MessageType response_type,
    const OperationResponse& response,
    std::uint8_t* output,
    std::size_t output_capacity,
    std::size_t& payload_length
)
{
    payload_length = 0;

    const protocol::StatusCode status =
        to_protocol_status(response.result);

    const protocol::KeyView key{
        response.key,
        response.key_length
    };

    switch (response_type) {
        case protocol::MessageType::SET_ACK: {
            const protocol::SetAckPayload payload{
                status,
                key
            };

            return protocol::serialize_set_ack(
                payload,
                output,
                output_capacity,
                payload_length
            ) == protocol::Result::OK;
        }

        case protocol::MessageType::GET_RESP: {
            const protocol::ValueView value{
                response.value,
                static_cast<std::uint8_t>(
                    response.value_length
                )
            };

            const protocol::GetResponsePayload payload{
                status,
                key,
                value
            };

            return protocol::serialize_get_response(
                payload,
                output,
                output_capacity,
                payload_length
            ) == protocol::Result::OK;
        }

        case protocol::MessageType::DEL_ACK: {
            const protocol::DeleteAckPayload payload{
                status,
                key
            };

            return protocol::serialize_delete_ack(
                payload,
                output,
                output_capacity,
                payload_length
            ) == protocol::Result::OK;
        }

        case protocol::MessageType::REPL_ACK: {
            const protocol::ReplicationAckPayload payload{
                status,
                key
            };

            return protocol::serialize_replication_ack(
                payload,
                output,
                output_capacity,
                payload_length
            ) == protocol::Result::OK;
        }

        default:
            return false;
    }
}

bool ResponseBuilder::build(
    const protocol::PacketHeader& request_header,
    node::Id local_node_id,
    protocol::Hlc response_hlc,
    const OperationResponse& response,
    std::uint8_t* output,
    std::size_t output_capacity,
    std::size_t& output_length
)
{
    output_length = 0;

    if (
        output == nullptr ||
        output_capacity < protocol::HEADER_SIZE
    ) {
        return false;
    }

    if (
        local_node_id < node::MIN ||
        local_node_id > node::MAX
    ) {
        return false;
    }

    const protocol::MessageType response_type =
        response_type_for(
            request_header.msg_type
        );

    if (
        response_type != protocol::MessageType::SET_ACK &&
        response_type != protocol::MessageType::GET_RESP &&
        response_type != protocol::MessageType::DEL_ACK &&
        response_type != protocol::MessageType::REPL_ACK
    ) {
        return false;
    }

    std::uint8_t payload[
        protocol::MAX_PAYLOAD_SIZE
    ]{};

    std::size_t payload_length = 0;

    if (
        !build_payload(
            response_type,
            response,
            payload,
            sizeof(payload),
            payload_length
        )
    ) {
        return false;
    }

    if (
        payload_length >
        protocol::MAX_PAYLOAD_SIZE
    ) {
        return false;
    }

    const protocol::PacketHeader response_header{
        protocol::PROTOCOL_VERSION,
        response_type,
        0,
        protocol::HEADER_SIZE,

        request_header.request_id,

        request_header.origin_node_id,
        local_node_id,

        // A REPL_ACK goes back to the primary that sent the REPL_REQ;
        // client responses go to the request's origin.
        response_type == protocol::MessageType::REPL_ACK
            ? request_header.sender_node_id
            : request_header.origin_node_id,

        0,

        response_hlc,

        static_cast<std::uint16_t>(
            payload_length
        )
    };

    return protocol::serialize_packet(
        response_header,
        protocol::ByteView{
            payload,
            static_cast<std::uint8_t>(
                payload_length
            )
        },
        output,
        output_capacity,
        output_length
    ) == protocol::Result::OK;
}

} // namespace kv
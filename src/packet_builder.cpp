#include "packet_builder.h"

#include "heartbeat.h"

namespace packet {

namespace {

bool finish(
    protocol::MessageType type,
    const Route& route,
    const std::uint8_t* payload,
    std::size_t payload_length,
    Buffer& out
)
{
    const protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        type,
        route.flags,
        protocol::HEADER_SIZE,
        route.request_id,
        route.origin,
        route.sender,
        route.destination,
        0,
        route.hlc,
        static_cast<std::uint16_t>(payload_length)
    };

    if (protocol::validate_message_header(header) != protocol::Result::OK) {
        return false;
    }

    return protocol::serialize_packet(
        header,
        protocol::ByteView{
            payload,
            static_cast<std::uint8_t>(payload_length)
        },
        out.data,
        sizeof(out.data),
        out.length
    ) == protocol::Result::OK;
}

} // namespace

bool build_request(
    protocol::MessageType type,
    const Route& route,
    protocol::KeyView key,
    protocol::ValueView value,
    Buffer& out
)
{
    out.length = 0;

    std::uint8_t payload[protocol::MAX_PAYLOAD_SIZE]{};
    std::size_t payload_length = 0;
    protocol::Result result = protocol::Result::INVALID_MESSAGE;

    switch (type) {
        case protocol::MessageType::SET_REQ:
            result = protocol::serialize_set_request(
                protocol::SetRequestPayload{key, value},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::REPL_REQ:
            result = protocol::serialize_replication_request(
                protocol::ReplicationRequestPayload{key, value},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::RESYNC_DATA:
            result = protocol::serialize_resync_data(
                protocol::ResyncDataPayload{key, value},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::GET_REQ:
            result = protocol::serialize_get_request(
                protocol::GetRequestPayload{key},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::DEL_REQ:
            result = protocol::serialize_delete_request(
                protocol::DeleteRequestPayload{key},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        default:
            return false;
    }

    if (result != protocol::Result::OK) {
        return false;
    }

    return finish(type, route, payload, payload_length, out);
}

bool build_response(
    protocol::MessageType type,
    const Route& route,
    protocol::StatusCode status,
    protocol::KeyView key,
    protocol::ValueView value,
    Buffer& out
)
{
    out.length = 0;

    std::uint8_t payload[protocol::MAX_PAYLOAD_SIZE]{};
    std::size_t payload_length = 0;
    protocol::Result result = protocol::Result::INVALID_MESSAGE;

    switch (type) {
        case protocol::MessageType::SET_ACK:
            result = protocol::serialize_set_ack(
                protocol::SetAckPayload{status, key},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::GET_RESP:
            result = protocol::serialize_get_response(
                protocol::GetResponsePayload{status, key, value},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::DEL_ACK:
            result = protocol::serialize_delete_ack(
                protocol::DeleteAckPayload{status, key},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        case protocol::MessageType::REPL_ACK:
            result = protocol::serialize_replication_ack(
                protocol::ReplicationAckPayload{status, key},
                payload,
                sizeof(payload),
                payload_length
            );
            break;

        default:
            return false;
    }

    if (result != protocol::Result::OK) {
        return false;
    }

    return finish(type, route, payload, payload_length, out);
}

bool build_heartbeat(
    protocol::NodeId sender,
    protocol::Hlc hlc,
    Buffer& out
)
{
    return heartbeat::build_packet(
        sender,
        hlc,
        out.data,
        sizeof(out.data),
        out.length
    ) == protocol::Result::OK;
}

bool build_version_table(
    protocol::NodeId sender,
    protocol::Hlc hlc,
    const protocol::VersionTablePayload& table,
    Buffer& out
)
{
    out.length = 0;

    std::uint8_t payload[protocol::MAX_PAYLOAD_SIZE]{};
    std::size_t payload_length = 0;

    if (protocol::serialize_version_table(
            table,
            payload,
            sizeof(payload),
            payload_length
        ) != protocol::Result::OK) {
        return false;
    }

    const Route route{
        0,
        sender,
        sender,
        protocol::NODE_ID_BROADCAST,
        hlc,
        0
    };

    return finish(
        protocol::MessageType::VERSION_TABLE,
        route,
        payload,
        payload_length,
        out
    );
}

const char* to_string(protocol::MessageType type)
{
    switch (type) {
        case protocol::MessageType::SET_REQ: return "SET_REQ";
        case protocol::MessageType::SET_ACK: return "SET_ACK";
        case protocol::MessageType::GET_REQ: return "GET_REQ";
        case protocol::MessageType::GET_RESP: return "GET_RESP";
        case protocol::MessageType::DEL_REQ: return "DEL_REQ";
        case protocol::MessageType::DEL_ACK: return "DEL_ACK";
        case protocol::MessageType::REPL_REQ: return "REPL_REQ";
        case protocol::MessageType::REPL_ACK: return "REPL_ACK";
        case protocol::MessageType::HEARTBEAT: return "HEARTBEAT";
        case protocol::MessageType::VERSION_TABLE: return "VERSION_TABLE";
        case protocol::MessageType::RESYNC_DATA: return "RESYNC_DATA";
    }

    return "UNKNOWN";
}

const char* to_string(protocol::StatusCode status)
{
    switch (status) {
        case protocol::StatusCode::OK: return "OK";
        case protocol::StatusCode::NOT_FOUND: return "NOT_FOUND";
        case protocol::StatusCode::INVALID_REQUEST: return "INVALID_REQUEST";
        case protocol::StatusCode::KEY_TOO_LARGE: return "KEY_TOO_LARGE";
        case protocol::StatusCode::VALUE_TOO_LARGE: return "VALUE_TOO_LARGE";
        case protocol::StatusCode::STORAGE_ERROR: return "STORAGE_ERROR";
        case protocol::StatusCode::NOT_RESPONSIBLE: return "NOT_RESPONSIBLE";
        case protocol::StatusCode::REPLICATION_FAILED: return "REPLICATION_FAILED";
        case protocol::StatusCode::STALE_VERSION: return "STALE_VERSION";
        case protocol::StatusCode::UNSUPPORTED: return "UNSUPPORTED";
    }

    return "UNKNOWN";
}

} // namespace packet

#include "bridge.h"

#include <cstring>

#include "packet_builder.h"

namespace bridge {

namespace {

constexpr std::uint64_t PARTIAL_FRAME_IDLE_MS = 100;

protocol::MessageType response_type_for(protocol::MessageType request)
{
    switch (request) {
        case protocol::MessageType::SET_REQ: return protocol::MessageType::SET_ACK;
        case protocol::MessageType::GET_REQ: return protocol::MessageType::GET_RESP;
        default: return protocol::MessageType::DEL_ACK;
    }
}

} // namespace

protocol::StatusCode to_status(edge::ClientStatus status)
{
    switch (status) {
        case edge::ClientStatus::OK: return protocol::StatusCode::OK;
        case edge::ClientStatus::NOT_FOUND: return protocol::StatusCode::NOT_FOUND;
        case edge::ClientStatus::UNDER_REPLICATED: return protocol::StatusCode::REPLICATION_FAILED;
        case edge::ClientStatus::STORAGE_ERROR: return protocol::StatusCode::STORAGE_ERROR;
        case edge::ClientStatus::INVALID_REQUEST: return protocol::StatusCode::INVALID_REQUEST;
        case edge::ClientStatus::KEY_TOO_LARGE: return protocol::StatusCode::KEY_TOO_LARGE;
        case edge::ClientStatus::VALUE_TOO_LARGE: return protocol::StatusCode::VALUE_TOO_LARGE;
        case edge::ClientStatus::STALE_VERSION: return protocol::StatusCode::STALE_VERSION;
        case edge::ClientStatus::NOT_READY:
        case edge::ClientStatus::BUSY:
        case edge::ClientStatus::NO_ROUTE:
        case edge::ClientStatus::TIMEOUT:
            return protocol::StatusCode::NOT_RESPONSIBLE;
    }

    return protocol::StatusCode::INVALID_REQUEST;
}

Bridge::Bridge(edge::Runtime& runtime, WriteFn write, void* write_context)
    : runtime_(runtime), write_(write), write_context_(write_context)
{
}

void Bridge::on_bytes(const std::uint8_t* data, std::size_t length, std::uint64_t now_ms)
{
    if (length == 0) {
        return;
    }

    last_byte_ms_ = now_ms;
    decoder_.push(data, length);

    while (decoder_.next()) {
        handle_envelope(decoder_.envelope(), decoder_.envelope_length(), now_ms);
    }
}

void Bridge::tick(std::uint64_t now_ms)
{
    if (now_ms - last_byte_ms_ < PARTIAL_FRAME_IDLE_MS) {
        return;
    }

    while (decoder_.abandon_partial()) {
        if (decoder_.next()) {
            handle_envelope(decoder_.envelope(), decoder_.envelope_length(), now_ms);
        }
    }
}

void Bridge::send_envelope(const std::uint8_t* envelope, std::size_t length)
{
    std::uint8_t frame[uart::MAX_FRAME];
    std::size_t frame_length = 0;

    if (write_ != nullptr &&
        uart::encode(envelope, length, frame, sizeof(frame), frame_length)) {
        write_(write_context_, frame, frame_length);
    }
}

void Bridge::respond(
    protocol::MessageType request_type,
    std::uint32_t pi_request_id,
    protocol::StatusCode status,
    const std::uint8_t* key,
    std::size_t key_length,
    const std::uint8_t* value,
    std::size_t value_length
)
{
    const node::Id self = runtime_.local_node_id();

    const packet::Route route{
        pi_request_id,
        self,
        self,
        self,
        runtime_.clock().now(),
        0
    };

    packet::Buffer out{};

    if (!packet::build_response(
            response_type_for(request_type),
            route,
            status,
            protocol::KeyView{key, static_cast<std::uint8_t>(key_length)},
            protocol::ValueView{value, static_cast<std::uint8_t>(value_length)},
            out
        )) {
        return;
    }

    ++stats_.responses;
    send_envelope(out.data, out.length);
}

void Bridge::handle_envelope(const std::uint8_t* envelope, std::size_t length, std::uint64_t now_ms)
{
    protocol::PacketView packet{};
    protocol::DecodedMessage message{};

    if (protocol::deserialize_packet(envelope, length, packet) != protocol::Result::OK ||
        protocol::decode_message(packet, message) != protocol::Result::OK) {
        ++stats_.invalid;
        return;
    }

    const protocol::PacketHeader& header = message.header;

    if (header.msg_type == protocol::MessageType::HEARTBEAT) {
        ++stats_.pings;

        packet::Buffer out{};

        if (packet::build_heartbeat(runtime_.local_node_id(), runtime_.clock().now(), out)) {
            send_envelope(out.data, out.length);
        }

        return;
    }

    edge::OpType type{};
    protocol::KeyView key{};
    protocol::ValueView value{nullptr, 0};

    switch (header.msg_type) {
        case protocol::MessageType::SET_REQ:
            type = edge::OpType::SET;
            key = message.payload.set_request.key;
            value = message.payload.set_request.value;
            break;

        case protocol::MessageType::GET_REQ:
            type = edge::OpType::GET;
            key = message.payload.get_request.key;
            break;

        case protocol::MessageType::DEL_REQ:
            type = edge::OpType::DEL;
            key = message.payload.delete_request.key;
            break;

        default:
            ++stats_.unsupported;
            return;
    }

    ++stats_.requests;

    // A repeated request id that is still running is a Pi-side retry.
    for (const Pending& pending : pending_) {
        if (pending.used && pending.pi_request_id == header.request_id) {
            return;
        }
    }

    Pending* slot = nullptr;

    for (Pending& pending : pending_) {
        if (!pending.used) {
            slot = &pending;
            break;
        }
    }

    if (slot == nullptr) {
        ++stats_.rejected_busy;
        respond(header.msg_type, header.request_id, protocol::StatusCode::NOT_RESPONSIBLE,
                key.data, key.len, nullptr, 0);
        return;
    }

    const std::uint32_t tag = static_cast<std::uint32_t>(slot - pending_);

    const edge::ClientStatus accepted = runtime_.submit(
        type,
        key.data,
        key.len,
        value.data,
        value.len,
        on_complete,
        this,
        tag,
        now_ms
    );

    if (accepted != edge::ClientStatus::OK) {
        respond(header.msg_type, header.request_id, to_status(accepted),
                key.data, key.len, nullptr, 0);
        return;
    }

    *slot = Pending{true, header.request_id, header.msg_type};
}

void Bridge::on_complete(void* context, std::uint32_t tag, const edge::ClientResult& result)
{
    auto* self = static_cast<Bridge*>(context);

    if (tag >= config::BRIDGE_MAX_INFLIGHT || !self->pending_[tag].used) {
        return;
    }

    const Pending pending = self->pending_[tag];
    self->pending_[tag] = Pending{};

    self->respond(
        pending.request_type,
        pending.pi_request_id,
        to_status(result.status),
        result.key,
        result.key_length,
        result.value,
        result.value_length
    );
}

} // namespace bridge

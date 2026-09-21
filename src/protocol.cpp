#include "protocol.h"
#include <cstring>
namespace protocol {

bool is_valid_node_id(NodeId node_id) {
    return node_id >= NODE_ID_MIN &&
           node_id <= NODE_ID_MAX;
}

bool is_valid_destination(NodeId node_id) {
    return is_valid_node_id(node_id) ||
           node_id == NODE_ID_BROADCAST;
}

bool is_known_message_type(MessageType type) {
    const auto value = static_cast<std::uint8_t>(type);

    return value >= 0x01 &&
           value <= 0x0B;
}

bool has_valid_reserved_flags(std::uint8_t flags) {
    return (flags & FLAG_RESERVED_MASK) == 0;
}

Result serialize_set_request(
    const SetRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.key.len > KEY_MAX ||
        payload.value.len > VALUE_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > 0 && payload.key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.value.len > 0 && payload.value.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    const std::size_t required =
        1 + payload.key.len +
        1 + payload.value.len;

    if (required > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    if (out_capacity < required) {
        return Result::BUFFER_TOO_SMALL;
    }

    out[0] = payload.key.len;

    if (payload.key.len > 0) {
        std::memcpy(
            out + 1,
            payload.key.data,
            payload.key.len
        );
    }

    const std::size_t value_len_offset =
        1 + payload.key.len;

    out[value_len_offset] = payload.value.len;

    if (payload.value.len > 0) {
        std::memcpy(
            out + value_len_offset + 1,
            payload.value.data,
            payload.value.len
        );
    }

    bytes_written = required;

    return Result::OK;
}

Result deserialize_set_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    SetRequestPayload& payload
) {
    payload.key = {};
    payload.value = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload_len < 2 ||
        payload_len > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    const std::uint8_t key_len = data[0];

    if (key_len > KEY_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    const std::size_t value_len_offset =
        1 + key_len;

    if (value_len_offset >= payload_len) {
        return Result::INVALID_PAYLOAD;
    }

    const std::uint8_t value_len =
        data[value_len_offset];

    if (value_len > VALUE_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    const std::size_t expected_len =
        1 + key_len +
        1 + value_len;

    if (expected_len != payload_len) {
        return Result::INVALID_PAYLOAD;
    }

    payload.key = {
        data + 1,
        key_len
    };

    payload.value = {
        data + value_len_offset + 1,
        value_len
    };

    return Result::OK;
}








namespace {

class PayloadWriter {
public:
    PayloadWriter(
        std::uint8_t* data,
        std::size_t capacity
    )
        : data_(data), capacity_(capacity), position_(0) {}

    bool write_u8(std::uint8_t value) {
        if (position_ >= capacity_) {
            return false;
        }

        data_[position_++] = value;
        return true;
    }

    bool write_bytes(
        const std::uint8_t* data,
        std::size_t len
    ) {
        if (len > capacity_ - position_) {
            return false;
        }

        if (len > 0) {
            std::memcpy(
                data_ + position_,
                data,
                len
            );
        }

        position_ += len;
        return true;
    }

    bool write_byte_view(ByteView view) {
        if (!write_u8(view.len)) {
            return false;
        }

        return write_bytes(view.data, view.len);
    }

    std::size_t size() const {
        return position_;
    }

private:
    std::uint8_t* data_;
    std::size_t capacity_;
    std::size_t position_;
};

class PayloadReader {
public:
    PayloadReader(
        const std::uint8_t* data,
        std::size_t length
    )
        : data_(data), length_(length), position_(0) {}

    bool read_u8(std::uint8_t& value) {
        if (position_ >= length_) {
            return false;
        }

        value = data_[position_++];
        return true;
    }

    bool read_byte_view(ByteView& view) {
        std::uint8_t len = 0;

        if (!read_u8(len)) {
            return false;
        }

        if (len > length_ - position_) {
            return false;
        }

        view = {
            data_ + position_,
            len
        };

        position_ += len;

        return true;
    }

    bool finished() const {
        return position_ == length_;
    }




private:
    const std::uint8_t* data_;
    std::size_t length_;
    std::size_t position_;
};

} // namespace

Result serialize_replication_request(
    const ReplicationRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.key.len > KEY_MAX ||
        payload.value.len > VALUE_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > 0 &&
        payload.key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.value.len > 0 &&
        payload.value.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    const std::size_t required =
        static_cast<std::size_t>(1) + payload.key.len +
        static_cast<std::size_t>(1) + payload.value.len;

    if (required > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    if (out_capacity < required) {
        return Result::BUFFER_TOO_SMALL;
    }

    PayloadWriter writer(out, out_capacity);

    if (!writer.write_byte_view(payload.key) ||
        !writer.write_byte_view(payload.value)) {
        return Result::BUFFER_TOO_SMALL;
    }

    bytes_written = writer.size();

    return Result::OK;
}

Result deserialize_replication_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    ReplicationRequestPayload& payload
) {
    payload = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload_len > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    PayloadReader reader(data, payload_len);

    if (!reader.read_byte_view(payload.key) ||
        !reader.read_byte_view(payload.value)) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > KEY_MAX ||
        payload.value.len > VALUE_MAX ||
        !reader.finished()) {
        return Result::INVALID_PAYLOAD;
    }

    return Result::OK;
}

Result serialize_get_request(
    const GetRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.key.len > KEY_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > 0 &&
        payload.key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (out_capacity < static_cast<std::size_t>(1) + payload.key.len) {
        return Result::BUFFER_TOO_SMALL;
    }

    PayloadWriter writer(out, out_capacity);

    if (!writer.write_byte_view(payload.key)) {
        return Result::BUFFER_TOO_SMALL;
    }

    bytes_written = writer.size();
    return Result::OK;
}

Result deserialize_get_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    GetRequestPayload& payload
) {
    payload = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload_len > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    PayloadReader reader(data, payload_len);

    if (!reader.read_byte_view(payload.key)) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > KEY_MAX ||
        !reader.finished()) {
        return Result::INVALID_PAYLOAD;
    }

    return Result::OK;
}
Result serialize_set_ack(
    const SetAckPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.key.len > KEY_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > 0 &&
        payload.key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    const std::size_t required =
        1 + 1 + payload.key.len;

    if (out_capacity < required) {
        return Result::BUFFER_TOO_SMALL;
    }

    PayloadWriter writer(out, out_capacity);

    if (!writer.write_u8(
            static_cast<std::uint8_t>(payload.status)) ||
        !writer.write_byte_view(payload.key)) {
        return Result::BUFFER_TOO_SMALL;
    }

    bytes_written = writer.size();
    return Result::OK;
}

Result deserialize_set_ack(
    const std::uint8_t* data,
    std::size_t payload_len,
    SetAckPayload& payload
) {
    payload = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    PayloadReader reader(data, payload_len);

    std::uint8_t status = 0;

    if (!reader.read_u8(status) ||
        !reader.read_byte_view(payload.key) ||
        payload.key.len > KEY_MAX ||
        !reader.finished()) {
        return Result::INVALID_PAYLOAD;
    }

    payload.status =
        static_cast<StatusCode>(status);

    return Result::OK;
}
Result serialize_get_response(
    const GetResponsePayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.key.len > KEY_MAX ||
        payload.value.len > VALUE_MAX) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > 0 &&
        payload.key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.value.len > 0 &&
        payload.value.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.status != StatusCode::OK &&
        payload.value.len != 0) {
        return Result::INVALID_PAYLOAD;
    }

    const std::size_t required =
        1 + 1 + payload.key.len +
        1 + payload.value.len;

    if (required > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    if (out_capacity < required) {
        return Result::BUFFER_TOO_SMALL;
    }

    PayloadWriter writer(out, out_capacity);

    if (!writer.write_u8(
            static_cast<std::uint8_t>(payload.status)) ||
        !writer.write_byte_view(payload.key) ||
        !writer.write_byte_view(payload.value)) {
        return Result::BUFFER_TOO_SMALL;
    }

    bytes_written = writer.size();
    return Result::OK;
}

Result deserialize_get_response(
    const std::uint8_t* data,
    std::size_t payload_len,
    GetResponsePayload& payload
) {
    payload = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    PayloadReader reader(data, payload_len);

    std::uint8_t status = 0;

    if (!reader.read_u8(status) ||
        !reader.read_byte_view(payload.key) ||
        !reader.read_byte_view(payload.value)) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.key.len > KEY_MAX ||
        payload.value.len > VALUE_MAX ||
        !reader.finished()) {
        return Result::INVALID_PAYLOAD;
    }

    payload.status =
        static_cast<StatusCode>(status);

    if (payload.status != StatusCode::OK &&
        payload.value.len != 0) {
        return Result::INVALID_PAYLOAD;
    }

    return Result::OK;
}
Result serialize_delete_request(
    const DeleteRequestPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    GetRequestPayload equivalent{payload.key};

    return serialize_get_request(
        equivalent,
        out,
        out_capacity,
        bytes_written
    );
}

Result deserialize_delete_request(
    const std::uint8_t* data,
    std::size_t payload_len,
    DeleteRequestPayload& payload
) {
    payload = {};

    GetRequestPayload equivalent{};

    Result result = deserialize_get_request(
        data,
        payload_len,
        equivalent
    );

    if (result == Result::OK) {
        payload.key = equivalent.key;
    }

    return result;
}

Result serialize_delete_ack(
    const DeleteAckPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    SetAckPayload equivalent{
        payload.status,
        payload.key
    };

    return serialize_set_ack(
        equivalent,
        out,
        out_capacity,
        bytes_written
    );
}

Result deserialize_delete_ack(
    const std::uint8_t* data,
    std::size_t payload_len,
    DeleteAckPayload& payload
) {
    payload = {};

    SetAckPayload equivalent{};

    Result result = deserialize_set_ack(
        data,
        payload_len,
        equivalent
    );

    if (result == Result::OK) {
        payload.status = equivalent.status;
        payload.key = equivalent.key;
    }

    return result;
}
Result serialize_replication_ack(
    const ReplicationAckPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    SetAckPayload equivalent{
        payload.status,
        payload.key
    };

    return serialize_set_ack(
        equivalent,
        out,
        out_capacity,
        bytes_written
    );
}

Result deserialize_replication_ack(
    const std::uint8_t* data,
    std::size_t payload_len,
    ReplicationAckPayload& payload
) {
    payload = {};

    SetAckPayload equivalent{};

    Result result = deserialize_set_ack(
        data,
        payload_len,
        equivalent
    );

    if (result == Result::OK) {
        payload.status = equivalent.status;
        payload.key = equivalent.key;
    }

    return result;
}





namespace {

void write_u16_be(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<std::uint8_t>(value & 0xFF);
}

void write_u32_be(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(value & 0xFF);
}

void write_u64_be(std::uint8_t* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(
            (value >> (56 - 8 * i)) & 0xFF
        );
    }
}

std::uint16_t read_u16_be(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1])
    );
}

std::uint32_t read_u32_be(const std::uint8_t* data) {
    return
        (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_u64_be(const std::uint8_t* data) {
    std::uint64_t value = 0;

    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[i])
                 << (56 - 8 * i);
    }

    return value;
}

} 

Result serialize_version_table(
    const VersionTablePayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.entry_count > VERSION_TABLE_MAX_ENTRIES) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.entry_count > 0 && payload.entries == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.chunk_count == 0 ||
        payload.chunk_index >= payload.chunk_count) {
        return Result::INVALID_PAYLOAD;
    }

    std::size_t required = 9;

    for (std::size_t i = 0; i < payload.entry_count; ++i) {
        const VersionTableEntry& entry = payload.entries[i];

        if (entry.key.len > KEY_MAX) {
            return Result::INVALID_PAYLOAD;
        }

        if (entry.key.len > 0 &&
            entry.key.data == nullptr) {
            return Result::INVALID_ARGUMENT;
        }

        if (!is_valid_node_id(entry.version_origin_id)) {
            return Result::INVALID_PAYLOAD;
        }

        required +=
            static_cast<std::size_t>(11) +
            entry.key.len;
    }

    if (required > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    if (out_capacity < required) {
        return Result::BUFFER_TOO_SMALL;
    }

    write_u32_be(out, payload.sync_id);
    write_u16_be(out + 4, payload.chunk_index);
    write_u16_be(out + 6, payload.chunk_count);

    out[8] = payload.entry_count;

    std::size_t offset = 9;

    for (std::size_t i = 0; i < payload.entry_count; ++i) {
        const VersionTableEntry& entry = payload.entries[i];

        out[offset++] = entry.entry_flags;
        out[offset++] = entry.key.len;

        if (entry.key.len > 0) {
            std::memcpy(
                out + offset,
                entry.key.data,
                entry.key.len
            );

            offset += entry.key.len;
        }

        write_u64_be(out + offset, entry.version_hlc);
        offset += 8;

        out[offset++] = entry.version_origin_id;
    }

    bytes_written = offset;

    return Result::OK;
}


Result deserialize_version_table(
    const std::uint8_t* data,
    std::size_t payload_len,
    VersionTablePayload& payload,
    VersionTableEntry* entries_out,
    std::size_t entries_capacity
) {
    payload = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload_len < 9 ||
        payload_len > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    payload.sync_id = read_u32_be(data);
    payload.chunk_index = read_u16_be(data + 4);
    payload.chunk_count = read_u16_be(data + 6);
    payload.entry_count = data[8];

    if (payload.chunk_count == 0 ||
        payload.chunk_index >= payload.chunk_count) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.entry_count > VERSION_TABLE_MAX_ENTRIES) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.entry_count > entries_capacity) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (payload.entry_count > 0 &&
        entries_out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    std::size_t offset = 9;

    for (std::size_t i = 0;
         i < payload.entry_count;
         ++i) {

        if (payload_len - offset < 11) {
            return Result::INVALID_PAYLOAD;
        }

        VersionTableEntry& entry = entries_out[i];

        entry.entry_flags = data[offset++];

        const std::uint8_t key_len = data[offset++];

        if (key_len > KEY_MAX) {
            return Result::INVALID_PAYLOAD;
        }

        if (payload_len - offset <
            static_cast<std::size_t>(key_len) + 9) {
            return Result::INVALID_PAYLOAD;
        }

        entry.key = {
            data + offset,
            key_len
        };

        offset += key_len;

        entry.version_hlc =
            read_u64_be(data + offset);

        offset += 8;

        entry.version_origin_id = data[offset++];

        if (!is_valid_node_id(entry.version_origin_id)) {
            return Result::INVALID_PAYLOAD;
        }
    }

    if (offset != payload_len) {
        return Result::INVALID_PAYLOAD;
    }

    payload.entries = entries_out;

    return Result::OK;
}
Result serialize_resync_data(
    const ResyncDataPayload& payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    ReplicationRequestPayload equivalent{
        payload.key,
        payload.value
    };

    return serialize_replication_request(
        equivalent,
        out,
        out_capacity,
        bytes_written
    );
}

Result deserialize_resync_data(
    const std::uint8_t* data,
    std::size_t payload_len,
    ResyncDataPayload& payload
) {
    payload = {};

    ReplicationRequestPayload equivalent{};

    Result result = deserialize_replication_request(
        data,
        payload_len,
        equivalent
    );

    if (result == Result::OK) {
        payload.key = equivalent.key;
        payload.value = equivalent.value;
    }

    return result;
}




Result serialize_header(
    const PacketHeader& header,
    std::uint8_t* out,
    std::size_t out_capacity
) {
    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (out_capacity < HEADER_SIZE) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (header.protocol_version != PROTOCOL_VERSION ||
        header.header_len != HEADER_SIZE ||
        header.payload_len > MAX_PAYLOAD_SIZE ||
        header.ttl != 0 ||
        !is_valid_node_id(header.origin_node_id) ||
        !is_valid_node_id(header.sender_node_id) ||
        !is_valid_destination(header.destination_node_id) ||
        !is_known_message_type(header.msg_type) ||
        !has_valid_reserved_flags(header.flags)) {
        return Result::INVALID_PACKET;
    }

    out[0] = header.protocol_version;
    out[1] = static_cast<std::uint8_t>(header.msg_type);
    out[2] = header.flags;
    out[3] = header.header_len;

    write_u32_be(out + 4, header.request_id);

    out[8] = header.origin_node_id;
    out[9] = header.sender_node_id;
    out[10] = header.destination_node_id;
    out[11] = header.ttl;

    write_u64_be(out + 12, header.hlc);
    write_u16_be(out + 20, header.payload_len);

    return Result::OK;
}
Result deserialize_header(
    const std::uint8_t* data,
    std::size_t packet_len,
    PacketHeader& header
) {
    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (packet_len < HEADER_SIZE) {
        return Result::INVALID_PACKET;
    }

    header.protocol_version = data[0];
    header.msg_type =
        static_cast<MessageType>(data[1]);
    header.flags = data[2];
    header.header_len = data[3];

    header.request_id = read_u32_be(data + 4);

    header.origin_node_id = data[8];
    header.sender_node_id = data[9];
    header.destination_node_id = data[10];
    header.ttl = data[11];

    header.hlc = read_u64_be(data + 12);
    header.payload_len = read_u16_be(data + 20);

    if (header.protocol_version != PROTOCOL_VERSION ||
        header.header_len != HEADER_SIZE ||
        header.payload_len > MAX_PAYLOAD_SIZE ||
        packet_len != HEADER_SIZE + header.payload_len ||
        header.ttl != 0 ||
        !is_valid_node_id(header.origin_node_id) ||
        !is_valid_node_id(header.sender_node_id) ||
        !is_valid_destination(header.destination_node_id) ||
        !is_known_message_type(header.msg_type) ||
        !has_valid_reserved_flags(header.flags)) {
        return Result::INVALID_PACKET;
    }

    return Result::OK;
}

Result validate_message_header(
    const PacketHeader& header
) {
    switch (header.msg_type) {
        case MessageType::SET_REQ:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::SET_ACK:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::GET_REQ:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::GET_RESP:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::DEL_REQ:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::DEL_ACK:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::REPL_REQ:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::REPL_ACK:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::HEARTBEAT:
            if (header.destination_node_id != NODE_ID_BROADCAST) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::VERSION_TABLE:
            if (header.destination_node_id != NODE_ID_BROADCAST) {
                return Result::INVALID_PACKET;
            }
            break;

        case MessageType::RESYNC_DATA:
            if (!is_valid_node_id(header.destination_node_id)) {
                return Result::INVALID_PACKET;
            }
            break;

        default:
            return Result::INVALID_MESSAGE;
    }

    return Result::OK;
}
Result decode_message(
    const PacketView& packet,
    DecodedMessage& message
) {
    message = {};

    Result result = validate_message_header(packet.header);

    if (result != Result::OK) {
        return result;
    }

    message.header = packet.header;

    switch (packet.header.msg_type) {
        case MessageType::SET_REQ:
            return deserialize_set_request(
                packet.payload.data,
                packet.payload.len,
                message.payload.set_request
            );

        case MessageType::SET_ACK:
            return deserialize_set_ack(
                packet.payload.data,
                packet.payload.len,
                message.payload.set_ack
            );

        case MessageType::GET_REQ:
            return deserialize_get_request(
                packet.payload.data,
                packet.payload.len,
                message.payload.get_request
            );

        case MessageType::GET_RESP:
            return deserialize_get_response(
                packet.payload.data,
                packet.payload.len,
                message.payload.get_response
            );

        case MessageType::DEL_REQ:
            return deserialize_delete_request(
                packet.payload.data,
                packet.payload.len,
                message.payload.delete_request
            );

        case MessageType::DEL_ACK:
            return deserialize_delete_ack(
                packet.payload.data,
                packet.payload.len,
                message.payload.delete_ack
            );

        case MessageType::REPL_REQ:
            return deserialize_replication_request(
                packet.payload.data,
                packet.payload.len,
                message.payload.replication_request
            );

        case MessageType::REPL_ACK:
            return deserialize_replication_ack(
                packet.payload.data,
                packet.payload.len,
                message.payload.replication_ack
            );

        case MessageType::HEARTBEAT:
            if (packet.payload.len != 0) {
                return Result::INVALID_PAYLOAD;
            }

            return Result::OK;

        case MessageType::VERSION_TABLE:
            return deserialize_version_table(
                packet.payload.data,
                packet.payload.len,
                message.payload.version_table,
                message.version_table_entries,
                VERSION_TABLE_MAX_ENTRIES
            );

        case MessageType::RESYNC_DATA:
            return deserialize_resync_data(
                packet.payload.data,
                packet.payload.len,
                message.payload.resync_data
            );

        default:
            return Result::INVALID_MESSAGE;
    }
}



Result serialize_packet(
    const PacketHeader& header,
    ByteView payload,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (payload.len > MAX_PAYLOAD_SIZE) {
        return Result::INVALID_PAYLOAD;
    }

    if (payload.len > 0 && payload.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (header.payload_len != payload.len) {
        return Result::INVALID_PACKET;
    }

    const std::size_t required =
        HEADER_SIZE + payload.len;

    if (out_capacity < required) {
        return Result::BUFFER_TOO_SMALL;
    }

    Result result = serialize_header(
        header,
        out,
        out_capacity
    );

    if (result != Result::OK) {
        return result;
    }

    if (payload.len > 0) {
        std::memcpy(
            out + HEADER_SIZE,
            payload.data,
            payload.len
        );
    }

    bytes_written = required;

    return Result::OK;
}

Result deserialize_packet(
    const std::uint8_t* data,
    std::size_t packet_len,
    PacketView& packet
) {
    packet = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    Result result = deserialize_header(
        data,
        packet_len,
        packet.header
    );

    if (result != Result::OK) {
        return result;
    }

    packet.payload = {
        data + HEADER_SIZE,
        static_cast<std::uint8_t>(packet.header.payload_len)
    };

    return Result::OK;
}


}
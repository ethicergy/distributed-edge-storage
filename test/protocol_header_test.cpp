#include <cassert>
#include <cstdint>
#include <iostream>

#include "protocol.h"

using namespace protocol;

int main() {
    PacketHeader original{
        .protocol_version = PROTOCOL_VERSION,
        .msg_type = MessageType::SET_REQ,
        .flags = FLAG_RETRY,
        .header_len = HEADER_SIZE,
        .request_id = 0x12345678,
        .origin_node_id = NODE_A,
        .sender_node_id = NODE_A,
        .destination_node_id = NODE_B,
        .ttl = 0,
        .hlc = 0x1122334455667788ULL,
        .payload_len = 5
    };

    std::uint8_t buffer[MAX_PACKET_SIZE]{};

    Result result = serialize_header(
        original,
        buffer,
        sizeof(buffer)
    );

    assert(result == Result::OK);

    const std::uint8_t expected[HEADER_SIZE] = {
        0x01,
        0x01,
        0x02,
        0x16,

        0x12, 0x34, 0x56, 0x78,

        0x01,
        0x01,
        0x02,
        0x00,

        0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88,

        0x00, 0x05
    };

    for (std::size_t i = 0; i < HEADER_SIZE; ++i) {
        assert(buffer[i] == expected[i]);
    }

    PacketHeader decoded{};

    result = deserialize_header(
        buffer,
        HEADER_SIZE + original.payload_len,
        decoded
    );

    assert(result == Result::OK);

    assert(decoded.protocol_version == original.protocol_version);
    assert(decoded.msg_type == original.msg_type);
    assert(decoded.flags == original.flags);
    assert(decoded.header_len == original.header_len);
    assert(decoded.request_id == original.request_id);
    assert(decoded.origin_node_id == original.origin_node_id);
    assert(decoded.sender_node_id == original.sender_node_id);
    assert(decoded.destination_node_id == original.destination_node_id);
    assert(decoded.ttl == original.ttl);
    assert(decoded.hlc == original.hlc);
    assert(decoded.payload_len == original.payload_len);

        auto expect_invalid = [](
        std::uint8_t* buffer,
        std::size_t packet_len
    ) {
        PacketHeader decoded{};

        Result result = deserialize_header(
            buffer,
            packet_len,
            decoded
        );

        assert(result == Result::INVALID_PACKET);
    };

    // Wrong protocol version.
    buffer[0] = 0x02;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[0] = PROTOCOL_VERSION;

    // Invalid header length.
    buffer[3] = 21;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[3] = HEADER_SIZE;

    // Invalid origin node ID.
    buffer[8] = NODE_ID_INVALID;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[8] = NODE_A;

    // Invalid sender node ID.
    buffer[9] = 0x05;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[9] = NODE_A;

    // Invalid destination node ID.
    buffer[10] = 0x05;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[10] = NODE_B;

    // Non-zero TTL is forbidden in v1.
    buffer[11] = 0x01;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[11] = 0x00;

    // Reserved flag bit is set.
    buffer[2] = 0x08;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[2] = FLAG_RETRY;

    // Unknown message type.
    buffer[1] = 0x7F;
    expect_invalid(buffer, HEADER_SIZE + original.payload_len);
    buffer[1] = static_cast<std::uint8_t>(MessageType::SET_REQ);

    // Declared payload is larger than the v1 maximum.
    buffer[20] = 0x00;
    buffer[21] = 0xE5; // 229
    expect_invalid(buffer, MAX_PACKET_SIZE);

    // Declared payload length does not match the actual packet length.
    buffer[20] = 0x00;
    buffer[21] = 0x05;
    expect_invalid(buffer, HEADER_SIZE + 4);

    // Serialization buffer too small.
    std::uint8_t small_buffer[HEADER_SIZE - 1]{};

    result = serialize_header(
        original,
        small_buffer,
        sizeof(small_buffer)
    );

    assert(result == Result::BUFFER_TOO_SMALL);

    // Null output buffer.
    result = serialize_header(
        original,
        nullptr,
        HEADER_SIZE
    );

    assert(result == Result::INVALID_ARGUMENT);

    std::cout << "Header validation tests: PASS\n";
    std::cout << "Header round-trip test: PASS\n";

    return 0;
}
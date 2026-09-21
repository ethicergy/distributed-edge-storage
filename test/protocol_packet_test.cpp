#include "protocol.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace protocol;

    const std::uint8_t payload_data[] = {
        0x04, 't', 'e', 'm', 'p',
        0x02, '4', '2'
    };

    PacketHeader header{
        PROTOCOL_VERSION,
        MessageType::SET_REQ,
        0,
        static_cast<std::uint8_t>(HEADER_SIZE),
        42,
        NODE_A,
        NODE_A,
        NODE_B,
        0,
        0x0001020304050607ULL,
        8
    };

    ByteView payload{
        payload_data,
        8
    };

    std::uint8_t buffer[MAX_PACKET_SIZE] = {};
    std::size_t bytes_written = 0;

    Result result = serialize_packet(
        header,
        payload,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == HEADER_SIZE + 8);

    // Header bytes.
    assert(buffer[0] == PROTOCOL_VERSION);
    assert(buffer[1] ==
           static_cast<std::uint8_t>(MessageType::SET_REQ));

    // Big-endian request ID: 42 = 00 00 00 2A.
    assert(buffer[4] == 0x00);
    assert(buffer[5] == 0x00);
    assert(buffer[6] == 0x00);
    assert(buffer[7] == 0x2A);

    // Payload starts exactly after the 22-byte header.
    assert(std::memcmp(
        buffer + HEADER_SIZE,
        payload_data,
        sizeof(payload_data)
    ) == 0);

    // ------------------------------------------------------------
    // Packet round-trip
    // ------------------------------------------------------------

    PacketView decoded{};

    result = deserialize_packet(
        buffer,
        bytes_written,
        decoded
    );

    assert(result == Result::OK);

    assert(decoded.header.request_id == 42);
    assert(decoded.header.origin_node_id == NODE_A);
    assert(decoded.header.sender_node_id == NODE_A);
    assert(decoded.header.destination_node_id == NODE_B);
    assert(decoded.header.hlc == 0x0001020304050607ULL);
    assert(decoded.header.payload_len == 8);

    assert(decoded.payload.len == 8);
    assert(std::memcmp(
        decoded.payload.data,
        payload_data,
        8
    ) == 0);

    std::cout << "Packet round-trip test: PASS\n";

    // ------------------------------------------------------------
    // Header/payload length mismatch
    // ------------------------------------------------------------

    PacketHeader bad_header = header;
    bad_header.payload_len = 7;

    result = serialize_packet(
        bad_header,
        payload,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::INVALID_PACKET);

    std::cout << "Packet length consistency test: PASS\n";

    // ------------------------------------------------------------
    // Output buffer too small
    // ------------------------------------------------------------

    result = serialize_packet(
        header,
        payload,
        buffer,
        HEADER_SIZE + 7,
        bytes_written
    );

    assert(result == Result::BUFFER_TOO_SMALL);

    std::cout << "Packet buffer-size test: PASS\n";

    // ------------------------------------------------------------
    // Truncated packet
    // ------------------------------------------------------------

    result = deserialize_packet(
        buffer,
        HEADER_SIZE + 7,
        decoded
    );

    assert(result != Result::OK);

    std::cout << "Packet truncation test: PASS\n";

    return 0;
}
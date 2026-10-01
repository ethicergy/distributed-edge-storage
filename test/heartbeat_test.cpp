#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "heartbeat.h"

int main()
{
    using namespace protocol;

    std::uint8_t buffer[
        MAX_PACKET_SIZE
    ]{};

    std::size_t bytes_written = 0;

    const hlc::Hlc test_hlc =
        hlc::make(
            0x123456789ABCuLL,
            0x0042
        );

    /*
     * --------------------------------------------------
     * Build heartbeat
     * --------------------------------------------------
     */

    Result result =
        heartbeat::build_packet(
            node::A,
            test_hlc,
            buffer,
            sizeof(buffer),
            bytes_written
        );

    assert(result == Result::OK);

    assert(
        bytes_written == HEADER_SIZE
    );

    std::cout
        << "Heartbeat packet build: PASS\n";

    /*
     * --------------------------------------------------
     * Decode packet
     * --------------------------------------------------
     */

    PacketView packet{};

    result =
        deserialize_packet(
            buffer,
            bytes_written,
            packet
        );

    assert(result == Result::OK);

    assert(
        packet.header.protocol_version ==
        PROTOCOL_VERSION
    );

    assert(
        packet.header.msg_type ==
        MessageType::HEARTBEAT
    );

    assert(
        packet.header.flags == 0
    );

    assert(
        packet.header.header_len ==
        HEADER_SIZE
    );

    assert(
        packet.header.request_id == 0
    );

    assert(
        packet.header.origin_node_id ==
        node::A
    );

    assert(
        packet.header.sender_node_id ==
        node::A
    );

    assert(
        packet.header.destination_node_id ==
        NODE_ID_BROADCAST
    );

    assert(
        packet.header.ttl == 0
    );

    assert(
        packet.header.hlc ==
        test_hlc
    );

    assert(
        packet.header.payload_len == 0
    );

    assert(
        packet.payload.len == 0
    );

    std::cout
        << "Heartbeat packet decode: PASS\n";

    /*
     * --------------------------------------------------
     * Decode application message
     * --------------------------------------------------
     */

    DecodedMessage message{};

    result =
        decode_message(
            packet,
            message
        );

    assert(result == Result::OK);

    std::cout
        << "Heartbeat message validation: PASS\n";

    /*
     * --------------------------------------------------
     * Invalid local node
     * --------------------------------------------------
     */

    result =
        heartbeat::build_packet(
            node::INVALID,
            test_hlc,
            buffer,
            sizeof(buffer),
            bytes_written
        );

    assert(
        result == Result::INVALID_ARGUMENT
    );

    std::cout
        << "Heartbeat invalid-node test: PASS\n";

    /*
     * --------------------------------------------------
     * Buffer too small
     * --------------------------------------------------
     */

    result =
        heartbeat::build_packet(
            node::A,
            test_hlc,
            buffer,
            HEADER_SIZE - 1,
            bytes_written
        );

    assert(
        result == Result::BUFFER_TOO_SMALL
    );

    std::cout
        << "Heartbeat buffer-size test: PASS\n";

    /*
     * --------------------------------------------------
     * Confirm wire representation of important fields
     * --------------------------------------------------
     */

    result =
        heartbeat::build_packet(
            node::D,
            test_hlc,
            buffer,
            sizeof(buffer),
            bytes_written
        );

    assert(result == Result::OK);

    assert(
        buffer[1] ==
        static_cast<std::uint8_t>(
            MessageType::HEARTBEAT
        )
    );

    assert(buffer[4] == 0x00);
    assert(buffer[5] == 0x00);
    assert(buffer[6] == 0x00);
    assert(buffer[7] == 0x00);

    assert(buffer[8] == node::D);
    assert(buffer[9] == node::D);
    assert(
        buffer[10] ==
        NODE_ID_BROADCAST
    );

    assert(buffer[20] == 0x00);
    assert(buffer[21] == 0x00);

    /*
     * HLC starts at byte 12 and is big-endian.
     */
    for (int i = 0; i < 8; ++i) {
        const std::uint8_t expected =
            static_cast<std::uint8_t>(
                (test_hlc >> ((7 - i) * 8)) &
                0xFF
            );

        assert(
            buffer[12 + i] == expected
        );
    }

    std::cout
        << "Heartbeat wire-format test: PASS\n";

    std::cout
        << "Heartbeat tests: PASS\n";

    return 0;
}
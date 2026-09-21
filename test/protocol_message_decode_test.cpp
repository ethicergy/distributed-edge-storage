#include "protocol.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace protocol;

    // ------------------------------------------------------------
    // SET_REQ
    // ------------------------------------------------------------

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
        100,
        8
    };

    PacketView packet{
        header,
        {payload_data, 8}
    };

    DecodedMessage message{};

    Result result = decode_message(
        packet,
        message
    );

    assert(result == Result::OK);

    assert(message.header.msg_type ==
           MessageType::SET_REQ);

    assert(message.payload.set_request.key.len == 4);
    assert(std::memcmp(
        message.payload.set_request.key.data,
        "temp",
        4
    ) == 0);

    assert(message.payload.set_request.value.len == 2);
    assert(std::memcmp(
        message.payload.set_request.value.data,
        "42",
        2
    ) == 0);

    std::cout << "SET_REQ dispatch test: PASS\n";


    // ------------------------------------------------------------
    // GET_REQ
    // ------------------------------------------------------------

    const std::uint8_t get_payload[] = {
        0x04, 't', 'e', 'm', 'p'
    };

    packet.header.msg_type = MessageType::GET_REQ;
    packet.header.payload_len = 5;
    packet.payload = {
        get_payload,
        5
    };

    result = decode_message(
        packet,
        message
    );

    assert(result == Result::OK);

    assert(message.payload.get_request.key.len == 4);
    assert(std::memcmp(
        message.payload.get_request.key.data,
        "temp",
        4
    ) == 0);

    std::cout << "GET_REQ dispatch test: PASS\n";


    // ------------------------------------------------------------
    // HEARTBEAT
    // ------------------------------------------------------------

    packet.header.msg_type = MessageType::HEARTBEAT;
    packet.header.destination_node_id = NODE_ID_BROADCAST;
    packet.header.payload_len = 0;
    packet.payload = {
        nullptr,
        0
    };

    result = decode_message(
        packet,
        message
    );

    assert(result == Result::OK);

    std::cout << "HEARTBEAT dispatch test: PASS\n";


    // ------------------------------------------------------------
    // Invalid payload for declared message type
    // ------------------------------------------------------------

    const std::uint8_t bad_payload[] = {
        0x04, 't', 'e'
    };

    packet.header.msg_type = MessageType::SET_REQ;
    packet.header.destination_node_id = NODE_B;
    packet.header.payload_len = 3;
    packet.payload = {
        bad_payload,
        3
    };

    result = decode_message(
        packet,
        message
    );

    assert(result == Result::INVALID_PAYLOAD);

    std::cout << "Invalid payload dispatch test: PASS\n";


    return 0;
}
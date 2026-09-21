#include "protocol.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace protocol;

PacketHeader make_header(MessageType type, NodeId destination) {
    return PacketHeader{
        PROTOCOL_VERSION,
        type,
        0,
        static_cast<std::uint8_t>(HEADER_SIZE),
        1,
        NODE_A,
        NODE_A,
        destination,
        0,
        1,
        0
    };
}

int main() {
    // ------------------------------------------------------------
    // Request/response messages must use unicast destinations.
    // ------------------------------------------------------------

    assert(
        validate_message_header(
            make_header(MessageType::SET_REQ, NODE_B)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::GET_REQ, NODE_C)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::DEL_REQ, NODE_D)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::SET_ACK, NODE_A)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::GET_RESP, NODE_A)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::DEL_ACK, NODE_A)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::REPL_REQ, NODE_C)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::REPL_ACK, NODE_A)
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(MessageType::RESYNC_DATA, NODE_D)
        ) == Result::OK
    );

    std::cout << "Unicast message validation: PASS\n";


    // ------------------------------------------------------------
    // HEARTBEAT and VERSION_TABLE must be broadcast.
    // ------------------------------------------------------------

    assert(
        validate_message_header(
            make_header(
                MessageType::HEARTBEAT,
                NODE_ID_BROADCAST
            )
        ) == Result::OK
    );

    assert(
        validate_message_header(
            make_header(
                MessageType::VERSION_TABLE,
                NODE_ID_BROADCAST
            )
        ) == Result::OK
    );

    std::cout << "Broadcast message validation: PASS\n";


    // ------------------------------------------------------------
    // Reject broadcast destination for unicast messages.
    // ------------------------------------------------------------

    assert(
        validate_message_header(
            make_header(
                MessageType::SET_REQ,
                NODE_ID_BROADCAST
            )
        ) == Result::INVALID_PACKET
    );

    assert(
        validate_message_header(
            make_header(
                MessageType::GET_RESP,
                NODE_ID_BROADCAST
            )
        ) == Result::INVALID_PACKET
    );

    assert(
        validate_message_header(
            make_header(
                MessageType::REPL_REQ,
                NODE_ID_BROADCAST
            )
        ) == Result::INVALID_PACKET
    );

    std::cout << "Invalid broadcast usage tests: PASS\n";


    // ------------------------------------------------------------
    // Reject unicast destination for broadcast messages.
    // ------------------------------------------------------------

    assert(
        validate_message_header(
            make_header(
                MessageType::HEARTBEAT,
                NODE_B
            )
        ) == Result::INVALID_PACKET
    );

    assert(
        validate_message_header(
            make_header(
                MessageType::VERSION_TABLE,
                NODE_C
            )
        ) == Result::INVALID_PACKET
    );

    std::cout << "Invalid unicast usage tests: PASS\n";


    return 0;
}
#include "node_id.h"
#include "protocol.h"
#include "transport.h"

#include <Arduino.h>
#include <cstring>

namespace {

constexpr std::uint32_t TEST_REQUEST_ID = 1001;
constexpr std::uint64_t TEST_HLC = 0x0000000100000001ULL;

constexpr std::uint8_t NODE_A_MAC[transport::MAC_ADDRESS_SIZE] = {
    0x14, 0x63, 0x93, 0xC5, 0xD0, 0x18
};

constexpr std::uint8_t NODE_B_MAC[transport::MAC_ADDRESS_SIZE] = {
    0x14, 0x63, 0x93, 0xC5, 0xC3, 0x1C
};

constexpr std::uint8_t TEST_KEY[] = {
    't', 'e', 'm', 'p'
};

constexpr std::uint8_t TEST_VALUE[] = {
    '4', '2'
};

transport::EspNowTransport esp_transport;

void print_protocol_result(
    const char* operation,
    protocol::Result result
) {
    Serial.print(operation);
    Serial.print(": ");

    if (result == protocol::Result::OK) {
        Serial.println("OK");
    } else {
        Serial.print("FAIL (result=");
        Serial.print(static_cast<int>(result));
        Serial.println(")");
    }
}

void print_transport_result(
    const char* operation,
    transport::Result result
) {
    Serial.print(operation);
    Serial.print(": ");

    if (result == transport::Result::OK) {
        Serial.println("OK");
    } else {
        Serial.print("FAIL (result=");
        Serial.print(static_cast<int>(result));
        Serial.println(")");
    }
}

bool mac_matches(
    const std::uint8_t* actual,
    const std::uint8_t* expected
) {
    return std::memcmp(
        actual,
        expected,
        transport::MAC_ADDRESS_SIZE
    ) == 0;
}

bool wait_for_tx_status(
    node::Id expected_destination,
    bool& success,
    unsigned long timeout_ms
) {
    const unsigned long start = millis();

    while ((millis() - start) < timeout_ms) {
        transport::SendStatus status{};

        if (esp_transport.receive_send_status(status)) {
            if (status.destination == expected_destination) {
                success = status.success;
                return true;
            }
        }

        delay(5);
    }

    return false;
}

bool build_and_send_set_request() {
    std::uint8_t payload_buffer[protocol::MAX_PAYLOAD_SIZE]{};
    std::size_t payload_bytes = 0;

    protocol::SetRequestPayload payload{
        {
            TEST_KEY,
            static_cast<std::uint8_t>(sizeof(TEST_KEY))
        },
        {
            TEST_VALUE,
            static_cast<std::uint8_t>(sizeof(TEST_VALUE))
        }
    };

    protocol::Result result = protocol::serialize_set_request(
        payload,
        payload_buffer,
        sizeof(payload_buffer),
        payload_bytes
    );

    print_protocol_result(
        "SET_REQ payload serialization",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    protocol::PacketHeader header{};

    header.protocol_version = protocol::PROTOCOL_VERSION;
    header.msg_type = protocol::MessageType::SET_REQ;
    header.flags = 0;
    header.header_len = protocol::HEADER_SIZE;
    header.request_id = TEST_REQUEST_ID;
    header.origin_node_id = node::A;
    header.sender_node_id = node::A;
    header.destination_node_id = node::B;
    header.ttl = 0;
    header.hlc = TEST_HLC;
    header.payload_len =
        static_cast<std::uint16_t>(payload_bytes);

    std::uint8_t packet_buffer[protocol::MAX_PACKET_SIZE]{};
    std::size_t packet_bytes = 0;

    result = protocol::serialize_packet(
        header,
        {
            payload_buffer,
            static_cast<std::uint8_t>(payload_bytes)
        },
        packet_buffer,
        sizeof(packet_buffer),
        packet_bytes
    );

    print_protocol_result(
        "SET_REQ packet serialization",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    Serial.print("SET_REQ packet length: ");
    Serial.println(packet_bytes);

    transport::Result transport_result =
        esp_transport.send_unicast(
            node::B,
            packet_buffer,
            packet_bytes
        );

    print_transport_result(
        "A -> B SET_REQ transmission",
        transport_result
    );

    if (transport_result != transport::Result::OK) {
        return false;
    }

    bool tx_success = false;

    if (!wait_for_tx_status(
            node::B,
            tx_success,
            1000
        )) {
        Serial.println("SET_REQ TX STATUS: TIMEOUT");
        return false;
    }

    Serial.print("SET_REQ TX STATUS: ");
    Serial.println(tx_success ? "SUCCESS" : "FAIL");

    return tx_success;
}

bool handle_set_request_on_b(
    const transport::ReceivedFrame& frame
) {
    if (!mac_matches(frame.source_mac, NODE_A_MAC)) {
        Serial.println("B: source MAC verification: FAIL");
        return false;
    }

    Serial.println("B: source MAC verification: PASS");

    protocol::PacketView packet{};

    protocol::Result result =
        protocol::deserialize_packet(
            frame.data,
            frame.length,
            packet
        );

    print_protocol_result(
        "B: packet deserialization",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    protocol::DecodedMessage message{};

    result = protocol::decode_message(
        packet,
        message
    );

    print_protocol_result(
        "B: message decoding",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    bool header_ok =
        message.header.protocol_version ==
            protocol::PROTOCOL_VERSION &&
        message.header.msg_type ==
            protocol::MessageType::SET_REQ &&
        message.header.flags == 0 &&
        message.header.header_len ==
            protocol::HEADER_SIZE &&
        message.header.request_id ==
            TEST_REQUEST_ID &&
        message.header.origin_node_id ==
            node::A &&
        message.header.sender_node_id ==
            node::A &&
        message.header.destination_node_id ==
            node::B &&
        message.header.ttl == 0 &&
        message.header.hlc == TEST_HLC;

    if (!header_ok) {
        Serial.println("B: SET_REQ header verification: FAIL");
        return false;
    }

    Serial.println("B: SET_REQ header verification: PASS");

    const protocol::SetRequestPayload& received =
        message.payload.set_request;

    bool payload_ok =
        received.key.len == sizeof(TEST_KEY) &&
        received.value.len == sizeof(TEST_VALUE) &&
        std::memcmp(
            received.key.data,
            TEST_KEY,
            sizeof(TEST_KEY)
        ) == 0 &&
        std::memcmp(
            received.value.data,
            TEST_VALUE,
            sizeof(TEST_VALUE)
        ) == 0;

    if (!payload_ok) {
        Serial.println("B: SET_REQ payload verification: FAIL");
        return false;
    }

    Serial.println("B: SET_REQ payload verification: PASS");

    Serial.print("B: key = ");
    Serial.write(
        received.key.data,
        received.key.len
    );
    Serial.println();

    Serial.print("B: value = ");
    Serial.write(
        received.value.data,
        received.value.len
    );
    Serial.println();

    return true;
}

bool send_set_ack(
    const protocol::PacketHeader& request_header
) {
    std::uint8_t payload_buffer[protocol::MAX_PAYLOAD_SIZE]{};
    std::size_t payload_bytes = 0;

    protocol::SetAckPayload payload{
        protocol::StatusCode::OK,
        {
            TEST_KEY,
            static_cast<std::uint8_t>(sizeof(TEST_KEY))
        }
    };

    protocol::Result result =
        protocol::serialize_set_ack(
            payload,
            payload_buffer,
            sizeof(payload_buffer),
            payload_bytes
        );

    print_protocol_result(
        "SET_ACK payload serialization",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    protocol::PacketHeader header{};

    header.protocol_version = protocol::PROTOCOL_VERSION;
    header.msg_type = protocol::MessageType::SET_ACK;
    header.flags = 0;
    header.header_len = protocol::HEADER_SIZE;

    // Preserve the original request identity so A can
    // associate the ACK with request 1001.
    header.request_id = request_header.request_id;
    header.origin_node_id = request_header.origin_node_id;

    header.sender_node_id = node::B;
    header.destination_node_id = node::A;
    header.ttl = 0;

    // No HLC module yet. Preserve the test HLC for this phase.
    header.hlc = request_header.hlc;

    header.payload_len =
        static_cast<std::uint16_t>(payload_bytes);

    std::uint8_t packet_buffer[protocol::MAX_PACKET_SIZE]{};
    std::size_t packet_bytes = 0;

    result = protocol::serialize_packet(
        header,
        {
            payload_buffer,
            static_cast<std::uint8_t>(payload_bytes)
        },
        packet_buffer,
        sizeof(packet_buffer),
        packet_bytes
    );

    print_protocol_result(
        "SET_ACK packet serialization",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    Serial.print("SET_ACK packet length: ");
    Serial.println(packet_bytes);

    transport::Result transport_result =
        esp_transport.send_unicast(
            node::A,
            packet_buffer,
            packet_bytes
        );

    print_transport_result(
        "B -> A SET_ACK transmission",
        transport_result
    );

    if (transport_result != transport::Result::OK) {
        return false;
    }

    bool tx_success = false;

    if (!wait_for_tx_status(
            node::A,
            tx_success,
            1000
        )) {
        Serial.println("SET_ACK TX STATUS: TIMEOUT");
        return false;
    }

    Serial.print("SET_ACK TX STATUS: ");
    Serial.println(tx_success ? "SUCCESS" : "FAIL");

    return tx_success;
}

bool handle_set_ack_on_a(
    const transport::ReceivedFrame& frame
) {
    if (!mac_matches(frame.source_mac, NODE_B_MAC)) {
        Serial.println("A: source MAC verification: FAIL");
        return false;
    }

    Serial.println("A: source MAC verification: PASS");

    protocol::PacketView packet{};

    protocol::Result result =
        protocol::deserialize_packet(
            frame.data,
            frame.length,
            packet
        );

    print_protocol_result(
        "A: SET_ACK packet deserialization",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    protocol::DecodedMessage message{};

    result = protocol::decode_message(
        packet,
        message
    );

    print_protocol_result(
        "A: SET_ACK message decoding",
        result
    );

    if (result != protocol::Result::OK) {
        return false;
    }

    const protocol::SetAckPayload& ack =
        message.payload.set_ack;

    bool header_ok =
        message.header.protocol_version ==
            protocol::PROTOCOL_VERSION &&
        message.header.msg_type ==
            protocol::MessageType::SET_ACK &&
        message.header.flags == 0 &&
        message.header.header_len ==
            protocol::HEADER_SIZE &&
        message.header.request_id ==
            TEST_REQUEST_ID &&
        message.header.origin_node_id ==
            node::A &&
        message.header.sender_node_id ==
            node::B &&
        message.header.destination_node_id ==
            node::A &&
        message.header.ttl == 0 &&
        message.header.hlc == TEST_HLC;

    if (!header_ok) {
        Serial.println("A: SET_ACK header verification: FAIL");
        return false;
    }

    Serial.println("A: SET_ACK header verification: PASS");

    bool payload_ok =
        ack.status == protocol::StatusCode::OK &&
        ack.key.len == sizeof(TEST_KEY) &&
        std::memcmp(
            ack.key.data,
            TEST_KEY,
            sizeof(TEST_KEY)
        ) == 0;

    if (!payload_ok) {
        Serial.println("A: SET_ACK payload verification: FAIL");
        return false;
    }

    Serial.println("A: SET_ACK payload verification: PASS");

    return true;
}

} // namespace

void setup() {
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("PHASE 1D - PROTOCOL OVER ESP-NOW");
    Serial.println("========================================");

#if NODE_ID == 1

    transport::Result result =
        esp_transport.begin(node::A);

    print_transport_result(
        "Transport begin (Node A)",
        result
    );

#elif NODE_ID == 2

    transport::Result result =
        esp_transport.begin(node::B);

    print_transport_result(
        "Transport begin (Node B)",
        result
    );

#else

    Serial.println(
        "ERROR: Phase 1D test requires NODE_ID=1 or NODE_ID=2."
    );

#endif

#if NODE_ID == 1

    Serial.println("A: protocol integration test ready.");

#elif NODE_ID == 2

    Serial.println("B: protocol integration test ready.");

#endif
}

void loop() {

#if NODE_ID == 1

    static bool request_sent = false;
    static bool completed = false;

    if (!request_sent) {
        delay(2000);

        Serial.println();
        Serial.println("A: building SET_REQ...");
        
        request_sent = build_and_send_set_request();

        if (request_sent) {
            Serial.println("A: SET_REQ transmission complete.");
        }
    }

    if (!completed) {
        transport::ReceivedFrame frame{};

        if (esp_transport.receive(frame)) {
            Serial.println();
            Serial.println("A: received frame.");

            if (handle_set_ack_on_a(frame)) {
                Serial.println();
                Serial.println("========================================");
                Serial.println("PHASE 1D RESULT: PASS");
                Serial.println("A <-> B protocol-over-ESP-NOW verified.");
                Serial.println("========================================");

                completed = true;
            } else {
                Serial.println("A: SET_ACK verification failed.");
            }
        }
    }

#elif NODE_ID == 2

    static bool completed = false;

    if (!completed) {
        transport::ReceivedFrame frame{};

        if (esp_transport.receive(frame)) {
            Serial.println();
            Serial.println("B: received frame.");

            if (handle_set_request_on_b(frame)) {
                Serial.println("B: SET_REQ verification complete.");

                protocol::PacketView request_packet{};

                protocol::Result result =
                    protocol::deserialize_packet(
                        frame.data,
                        frame.length,
                        request_packet
                    );

                if (result != protocol::Result::OK) {
                    Serial.println(
                        "B: could not deserialize request "
                        "again for ACK construction."
                    );
                    return;
                }

                if (send_set_ack(request_packet.header)) {
                    Serial.println("B: SET_ACK transmission complete.");
                    completed = true;
                }
            } else {
                Serial.println("B: SET_REQ verification failed.");
            }
        }
    }

#endif

    delay(5);
}
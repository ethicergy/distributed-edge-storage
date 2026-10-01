
#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstring>

#include "dedup_manager.h"
#include "hlc.h"
#include "platform_time.h"
#include "protocol.h"
#include "reliable_packet.h"
#include "request_identity.h"
#include "request_manager.h"
#include "transport.h"
#include "node_id.h"

#ifndef NODE_ID
#error "NODE_ID must be defined by PlatformIO"
#endif

constexpr node::Id LOCAL_NODE_ID =
    static_cast<node::Id>(NODE_ID);

constexpr node::Id PEER_NODE_ID =
    LOCAL_NODE_ID == node::A ? node::B : node::A;

constexpr std::uint8_t REQUEST_KEY[] = {'r','t','e','s','t'};
constexpr std::uint8_t REQUEST_VALUE[] = {'4','2'};

hlc::Clock hlc_clock(platform_time::now_ms);
transport::EspNowTransport espnow_transport;

request::TimingConfig request_timing{};
request::Manager request_manager(request_timing);
request::DedupManager dedup_manager;
request::Generator request_id_generator;

struct SenderContext {
    transport::EspNowTransport* transport;
};

SenderContext sender_context{&espnow_transport};

bool request_submitted = false;
bool request_finished = false;
bool drop_first_response = true;

request::Identity active_request{};

std::uint32_t receiver_execution_count = 0;

bool reliable_send(
    void* context,
    node::Id destination,
    const std::uint8_t* packet,
    std::size_t packet_length,
    bool retry
)
{
    auto* sender = static_cast<SenderContext*>(context);

    if (sender == nullptr ||
        sender->transport == nullptr ||
        packet == nullptr ||
        packet_length > protocol::MAX_PACKET_SIZE) {
        return false;
    }

    if (!retry) {
        return sender->transport->send_unicast(
            destination,
            packet,
            packet_length
        ) == transport::Result::OK;
    }

    std::uint8_t retry_packet[protocol::MAX_PACKET_SIZE]{};
    std::size_t retry_length = 0;

    if (
        request::prepare_retry(
            packet,
            packet_length,
            retry_packet,
            sizeof(retry_packet),
            retry_length
        ) != request::PacketResult::OK
    ) {
        return false;
    }

    return sender->transport->send_unicast(
        destination,
        retry_packet,
        retry_length
    ) == transport::Result::OK;
}

bool build_set_request(
    std::uint8_t* output,
    std::size_t capacity,
    std::size_t& length
)
{
    const std::uint8_t payload[] = {
        static_cast<std::uint8_t>(sizeof(REQUEST_KEY)),
        REQUEST_KEY[0],
        REQUEST_KEY[1],
        REQUEST_KEY[2],
        REQUEST_KEY[3],
        REQUEST_KEY[4],
        static_cast<std::uint8_t>(sizeof(REQUEST_VALUE)),
        REQUEST_VALUE[0],
        REQUEST_VALUE[1]
    };

    const protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::SET_REQ,
        0,
        protocol::HEADER_SIZE,
        active_request.request_id,
        active_request.origin,
        active_request.origin,
        PEER_NODE_ID,
        0,
        hlc_clock.on_local_event(),
        static_cast<std::uint16_t>(sizeof(payload))
    };

    return protocol::serialize_packet(
        header,
        protocol::ByteView{payload, sizeof(payload)},
        output,
        capacity,
        length
    ) == protocol::Result::OK;
}

bool build_set_ack(
    const protocol::PacketHeader& request_header,
    std::uint8_t* output,
    std::size_t capacity,
    std::size_t& length
)
{
    protocol::SetAckPayload payload{
        protocol::StatusCode::OK,
        protocol::KeyView{
            REQUEST_KEY,
            static_cast<std::uint8_t>(sizeof(REQUEST_KEY))
        }
    };

    std::uint8_t payload_buffer[
        protocol::MAX_PAYLOAD_SIZE
    ]{};

    std::size_t payload_length = 0;

    if (
        protocol::serialize_set_ack(
            payload,
            payload_buffer,
            sizeof(payload_buffer),
            payload_length
        ) != protocol::Result::OK
    ) {
        return false;
    }

    const protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::SET_ACK,
        0,
        protocol::HEADER_SIZE,
        request_header.request_id,
        request_header.origin_node_id,
        LOCAL_NODE_ID,
        request_header.origin_node_id,
        0,
        hlc_clock.on_local_event(),
        static_cast<std::uint16_t>(payload_length)
    };

    return protocol::serialize_packet(
        header,
        protocol::ByteView{
            payload_buffer,
            static_cast<std::uint8_t>(payload_length)
        },
        output,
        capacity,
        length
    ) == protocol::Result::OK;
}

void process_receiver(
    const transport::ReceivedFrame& frame
)
{
    protocol::PacketView packet{};

    if (
        protocol::deserialize_packet(
            frame.data,
            frame.length,
            packet
        ) != protocol::Result::OK
    ) {
        return;
    }

    if (
        packet.header.msg_type !=
        protocol::MessageType::SET_REQ
    ) {
        return;
    }

    protocol::DecodedMessage decoded{};

    if (
        protocol::decode_message(
            packet,
            decoded
        ) != protocol::Result::OK
    ) {
        return;
    }

    request::Identity identity{};

    if (
        request::extract_identity(
            packet,
            identity
        ) != request::PacketResult::OK
    ) {
        return;
    }

    if (hlc_clock.on_receive(packet.header.hlc) == 0) {
        return;
    }

    const std::uint64_t now =
        platform_time::now_ms();

    const request::DedupResult result =
        dedup_manager.begin_request(
            identity,
            now
        );

    if (
        result ==
        request::DedupResult::NEW_REQUEST
    ) {
        ++receiver_execution_count;

        std::uint8_t response[
            protocol::MAX_PACKET_SIZE
        ]{};

        std::size_t response_length = 0;

        if (
            !build_set_ack(
                packet.header,
                response,
                sizeof(response),
                response_length
            )
        ) {
            return;
        }

        if (
            dedup_manager.complete_request(
                identity,
                response,
                response_length,
                now
            ) != request::DedupResult::NEW_REQUEST
        ) {
            return;
        }

        if (drop_first_response) {
            drop_first_response = false;
            Serial.println("SET_ACK: DELIBERATELY DROPPED");
            return;
        }

        espnow_transport.send_unicast(
            packet.header.origin_node_id,
            response,
            response_length
        );

        Serial.println("SET_ACK: SENT");
        return;
    }

    if (
        result ==
        request::DedupResult::DUPLICATE_IN_PROGRESS
    ) {
        Serial.println("DEDUP: DUPLICATE IN_PROGRESS");
        return;
    }

    if (
        result ==
        request::DedupResult::DUPLICATE_COMPLETED
    ) {
        std::uint8_t cached[
            protocol::MAX_PACKET_SIZE
        ]{};

        std::size_t cached_length = 0;

        if (
            dedup_manager.get_cached_response(
                identity,
                cached,
                sizeof(cached),
                cached_length,
                now
            ) != request::DedupResult::DUPLICATE_COMPLETED
        ) {
            Serial.println("DEDUP CACHE: FAIL");
            return;
        }

        espnow_transport.send_unicast(
            packet.header.origin_node_id,
            cached,
            cached_length
        );

        Serial.println("CACHED SET_ACK: REPLAYED");
    }
}

void process_requester(
    const transport::ReceivedFrame& frame
)
{
    protocol::PacketView packet{};

    if (
        protocol::deserialize_packet(
            frame.data,
            frame.length,
            packet
        ) != protocol::Result::OK
    ) {
        return;
    }

    if (
        packet.header.msg_type !=
        protocol::MessageType::SET_ACK
    ) {
        return;
    }

    protocol::DecodedMessage decoded{};

    if (
        protocol::decode_message(
            packet,
            decoded
        ) != protocol::Result::OK
    ) {
        Serial.println("SET_ACK decode: FAIL");
        return;
    }

    const request::Identity identity{
        packet.header.origin_node_id,
        packet.header.request_id
    };

    if (identity != active_request) {
        return;
    }

    if (
        decoded.payload.set_ack.key.len !=
        sizeof(REQUEST_KEY) ||
        std::memcmp(
            decoded.payload.set_ack.key.data,
            REQUEST_KEY,
            sizeof(REQUEST_KEY)
        ) != 0
    ) {
        Serial.println("SET_ACK key: FAIL");
        return;
    }

    const request::Result result =
        request_manager.complete(
            identity,
            frame.data,
            frame.length
        );

    if (result == request::Result::OK) {
        request_finished = true;
        Serial.println("REQUEST: COMPLETED");
    }
}

void process_frames()
{
    transport::ReceivedFrame frame{};

    while (espnow_transport.receive(frame)) {
        if (LOCAL_NODE_ID == node::A) {
            process_requester(frame);
        } else if (LOCAL_NODE_ID == node::B) {
            process_receiver(frame);
        }
    }
}

void submit_request()
{
    if (
        LOCAL_NODE_ID != node::A ||
        request_submitted
    ) {
        return;
    }

    std::uint32_t seed = esp_random();
    if (seed == 0) {
        seed = 1;
    }

    if (!request_id_generator.begin(node::A, seed)) {
        Serial.println("Request ID generator: FAIL");
        return;
    }

    active_request = request_id_generator.next();

    if (!active_request.valid()) {
        Serial.println("Request identity: FAIL");
        return;
    }

    std::uint8_t packet[
        protocol::MAX_PACKET_SIZE
    ]{};

    std::size_t packet_length = 0;

    if (
        !build_set_request(
            packet,
            sizeof(packet),
            packet_length
        )
    ) {
        Serial.println("SET_REQ build: FAIL");
        return;
    }

    bool initial_send_ok = false;

    const request::Result result =
        request_manager.submit(
            active_request,
            node::B,
            packet,
            packet_length,
            platform_time::now_ms(),
            reliable_send,
            &sender_context,
            initial_send_ok
        );

    if (result != request::Result::OK) {
        Serial.println("REQUEST SUBMIT: FAIL");
        return;
    }

    request_submitted = true;

    Serial.print("REQUEST ID: ");
    Serial.println(active_request.request_id);

    Serial.print("INITIAL SEND: ");
    Serial.println(
        initial_send_ok ? "OK" : "TRANSPORT FAILED"
    );
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("PHASE 4R - LOST ACK RELIABILITY TEST");

    if (!hlc_clock.begin(0)) {
        Serial.println("HLC begin: FAIL");
        return;
    }

    Serial.println("HLC begin: OK");

    if (
        espnow_transport.begin(LOCAL_NODE_ID) !=
        transport::Result::OK
    ) {
        Serial.println("Transport begin: FAIL");
        return;
    }

    Serial.println("Transport begin: OK");

    if (
        request_manager.begin() !=
        request::Result::OK
    ) {
        Serial.println("RequestManager begin: FAIL");
        return;
    }

    Serial.println("RequestManager begin: OK");

    dedup_manager.begin();
    Serial.println("DedupManager begin: OK");

    if (LOCAL_NODE_ID == node::A) {
        Serial.println("ROLE: REQUESTER A");
    } else if (LOCAL_NODE_ID == node::B) {
        Serial.println("ROLE: RECEIVER B");
    }
}

void loop()
{
    process_frames();

    if (LOCAL_NODE_ID == node::A) {
        submit_request();

        request_manager.tick(
            platform_time::now_ms()
        );
    }

    if (request_finished) {
        static bool printed = false;

        if (!printed) {
            printed = true;

            request::RequestInfo info{};

            if (
                request_manager.get_info(
                    active_request,
                    info
                ) == request::Result::OK
            ) {
                Serial.print("Retransmissions: ");
                Serial.println(info.retry_count);
            }

            Serial.println(
                "RELIABILITY LOST-ACK TEST: PASS"
            );
        }
    }

    delay(5);
}

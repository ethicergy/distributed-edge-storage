#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "reliable_packet.h"
#include "hlc.h"

static void build_set_request(
    std::uint8_t* buffer,
    std::size_t capacity,
    std::size_t& length
)
{
    const std::uint8_t payload[] = {
        3, 'k', 'e', 'y',
        5, 'v', 'a', 'l', 'u', 'e'
    };

    protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::SET_REQ,
        0,
        protocol::HEADER_SIZE,
        1234,
        node::A,
        node::A,
        node::B,
        0,
        hlc::make(5000, 12),
        static_cast<std::uint16_t>(
            sizeof(payload)
        )
    };

    assert(
        protocol::serialize_packet(
            header,
            protocol::ByteView{
                payload,
                sizeof(payload)
            },
            buffer,
            capacity,
            length
        ) ==
        protocol::Result::OK
    );
}

static void test_extract_identity()
{
    std::uint8_t buffer[
        protocol::MAX_PACKET_SIZE
    ]{};

    std::size_t length = 0;

    build_set_request(
        buffer,
        sizeof(buffer),
        length
    );

    protocol::PacketView packet{};

    assert(
        protocol::deserialize_packet(
            buffer,
            length,
            packet
        ) ==
        protocol::Result::OK
    );

    request::Identity identity{};

    assert(
        request::extract_identity(
            packet,
            identity
        ) ==
        request::PacketResult::OK
    );

    assert(identity.origin == node::A);
    assert(identity.request_id == 1234);
}

static void test_retry_changes_only_retry_flag()
{
    std::uint8_t original[
        protocol::MAX_PACKET_SIZE
    ]{};

    std::uint8_t retry[
        protocol::MAX_PACKET_SIZE
    ]{};

    std::size_t original_length = 0;
    std::size_t retry_length = 0;

    build_set_request(
        original,
        sizeof(original),
        original_length
    );

    assert(
        request::prepare_retry(
            original,
            original_length,
            retry,
            sizeof(retry),
            retry_length
        ) ==
        request::PacketResult::OK
    );

    assert(
        request::same_request_content(
            original,
            original_length,
            retry,
            retry_length
        )
    );

    protocol::PacketView retry_view{};

    assert(
        protocol::deserialize_packet(
            retry,
            retry_length,
            retry_view
        ) ==
        protocol::Result::OK
    );

    assert(
        retry_view.header.flags &
        protocol::FLAG_RETRY
    );

    assert(
        retry_view.header.request_id ==
        1234
    );

    assert(
        retry_view.header.origin_node_id ==
        node::A
    );

    assert(
        retry_view.header.hlc ==
        hlc::make(5000, 12)
    );
}

static void test_get_request()
{
    std::uint8_t packet[
        protocol::MAX_PACKET_SIZE
    ]{};

    std::size_t length = 0;

    const std::uint8_t payload[] = {
        4, 't', 'e', 's', 't'
    };

    protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::GET_REQ,
        0,
        protocol::HEADER_SIZE,
        77,
        node::B,
        node::B,
        node::C,
        0,
        hlc::make(9000, 3),
        static_cast<std::uint16_t>(
            sizeof(payload)
        )
    };

    assert(
        protocol::serialize_packet(
            header,
            protocol::ByteView{
                payload,
                sizeof(payload)
            },
            packet,
            sizeof(packet),
            length
        ) ==
        protocol::Result::OK
    );

    protocol::PacketView view{};

    assert(
        protocol::deserialize_packet(
            packet,
            length,
            view
        ) ==
        protocol::Result::OK
    );

    request::Identity identity{};

    assert(
        request::extract_identity(
            view,
            identity
        ) ==
        request::PacketResult::OK
    );

    assert(identity.origin == node::B);
    assert(identity.request_id == 77);
}

static void test_non_reliable_message()
{
    std::uint8_t buffer[
        protocol::MAX_PACKET_SIZE
    ]{};

    std::size_t length = 0;

    protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::HEARTBEAT,
        0,
        protocol::HEADER_SIZE,
        0,
        node::A,
        node::A,
        node::BROADCAST,
        0,
        hlc::make(100, 0),
        0
    };

    assert(
        protocol::serialize_packet(
            header,
            protocol::ByteView{
                nullptr,
                0
            },
            buffer,
            sizeof(buffer),
            length
        ) ==
        protocol::Result::OK
    );

    protocol::PacketView view{};

    assert(
        protocol::deserialize_packet(
            buffer,
            length,
            view
        ) ==
        protocol::Result::OK
    );

    request::Identity identity{};

    assert(
        request::extract_identity(
            view,
            identity
        ) ==
        request::PacketResult::NOT_RELIABLE
    );
}

int main()
{
    test_extract_identity();
    test_retry_changes_only_retry_flag();
    test_get_request();
    test_non_reliable_message();

    std::cout
        << "Reliable packet tests: PASS\n";

    return 0;
}
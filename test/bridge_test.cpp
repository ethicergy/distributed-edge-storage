/*
 * Raspberry Pi bridge on node A of the simulated cluster (H14):
 * framed SET/GET/DEL/ping from the "Pi", sensor keys readable, and
 * everything keeps working with another node powered off.
 */

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "bridge.h"
#include "cluster.h"
#include "packet_builder.h"
#include "sensor.h"
#include "test_support.h"
#include "uart_frame.h"

namespace {

struct PiSide {
    uart::Decoder decoder;
    std::vector<std::vector<std::uint8_t>> responses;
};

void pi_receive(void* context, const std::uint8_t* data, std::size_t length)
{
    auto* pi = static_cast<PiSide*>(context);
    pi->decoder.push(data, length);

    while (pi->decoder.next()) {
        pi->responses.emplace_back(pi->decoder.envelope(), pi->decoder.envelope() + pi->decoder.envelope_length());
    }
}

struct Harness {
    sim::Cluster cluster{31};
    PiSide pi;
    std::unique_ptr<bridge::Bridge> bridge;

    Harness()
    {
        cluster.enable_sensors(2000);
        REQUIRE(cluster.start_all());
        attach();

        cluster.on_node_tick = [this](node::Id id, std::uint64_t now) {
            if (id == node::A && bridge) {
                bridge->tick(now);
            }
        };
    }

    void attach()
    {
        bridge = std::make_unique<bridge::Bridge>(cluster.runtime(node::A), pi_receive, &pi);
    }

    void send(protocol::MessageType type, std::uint32_t request_id, const std::string& key, const std::string& value = "")
    {
        packet::Buffer packet{};
        const packet::Route route{request_id, node::A, node::A, node::A, 0, 0};

        if (type == protocol::MessageType::HEARTBEAT) {
            REQUIRE(packet::build_heartbeat(node::A, 0, packet));
        } else {
            REQUIRE(packet::build_request(
                type,
                route,
                protocol::KeyView{reinterpret_cast<const std::uint8_t*>(key.data()), static_cast<std::uint8_t>(key.size())},
                protocol::ValueView{reinterpret_cast<const std::uint8_t*>(value.data()), static_cast<std::uint8_t>(value.size())},
                packet
            ));
        }

        std::uint8_t frame[uart::MAX_FRAME];
        std::size_t length = 0;
        REQUIRE(uart::encode(packet.data, packet.length, frame, sizeof(frame), length));

        // Deliver in two pieces to exercise reassembly.
        bridge->on_bytes(frame, length / 2, cluster.now());
        bridge->on_bytes(frame + length / 2, length - length / 2, cluster.now());
    }

    protocol::DecodedMessage wait_response(std::uint32_t request_id, std::vector<std::uint8_t>& storage)
    {
        protocol::DecodedMessage message{};

        const bool got = cluster.run_until([&] {
            for (std::size_t i = 0; i < pi.responses.size(); ++i) {
                protocol::PacketView view{};
                if (protocol::deserialize_packet(pi.responses[i].data(), pi.responses[i].size(), view) == protocol::Result::OK &&
                    view.header.request_id == request_id) {
                    storage = pi.responses[i];
                    pi.responses.erase(pi.responses.begin() + static_cast<std::ptrdiff_t>(i));
                    return true;
                }
            }
            return false;
        }, 30000);

        REQUIRE(got);

        protocol::PacketView view{};
        REQUIRE(protocol::deserialize_packet(storage.data(), storage.size(), view) == protocol::Result::OK);
        REQUIRE(protocol::decode_message(view, message) == protocol::Result::OK);
        return message;
    }
};

void test_set_get_del_ping()
{
    Harness h;
    std::vector<std::uint8_t> raw;

    h.send(protocol::MessageType::HEARTBEAT, 0, "");
    protocol::DecodedMessage ping = h.wait_response(0, raw);
    REQUIRE(ping.header.msg_type == protocol::MessageType::HEARTBEAT);
    REQUIRE(ping.header.sender_node_id == node::A);
    REQUIRE(ping.header.hlc != 0);

    // Keys owned by every node, including one A does not own.
    for (int i = 0; i < 12; ++i) {
        const std::string key = "pi/" + std::to_string(i);
        const std::uint32_t id = 1000 + static_cast<std::uint32_t>(i) * 3;

        h.send(protocol::MessageType::SET_REQ, id, key, "value " + std::to_string(i));
        protocol::DecodedMessage set = h.wait_response(id, raw);
        REQUIRE(set.header.msg_type == protocol::MessageType::SET_ACK);
        REQUIRE(set.payload.set_ack.status == protocol::StatusCode::OK);

        h.send(protocol::MessageType::GET_REQ, id + 1, key);
        protocol::DecodedMessage get = h.wait_response(id + 1, raw);
        REQUIRE(get.header.msg_type == protocol::MessageType::GET_RESP);
        REQUIRE(get.payload.get_response.status == protocol::StatusCode::OK);
        REQUIRE(std::string(reinterpret_cast<const char*>(get.payload.get_response.value.data),
                            get.payload.get_response.value.len) == "value " + std::to_string(i));

        h.send(protocol::MessageType::DEL_REQ, id + 2, key);
        protocol::DecodedMessage del = h.wait_response(id + 2, raw);
        REQUIRE(del.header.msg_type == protocol::MessageType::DEL_ACK);
        REQUIRE(del.payload.delete_ack.status == protocol::StatusCode::OK);
    }

    h.send(protocol::MessageType::GET_REQ, 77, "pi/0");
    protocol::DecodedMessage missing = h.wait_response(77, raw);
    REQUIRE(missing.payload.get_response.status == protocol::StatusCode::NOT_FOUND);

    REQUIRE(h.bridge->stats().requests == 37);
    REQUIRE(h.bridge->stats().pings == 1);
}

void test_sensors_with_node_down()
{
    Harness h;
    std::vector<std::uint8_t> raw;

    // Let every sampler write a few samples.
    h.cluster.run_for(10000);

    // H14: one node (not the bridge) powered off.
    h.cluster.power_off(node::C);
    h.cluster.run_for(config::MEMBERSHIP_TIMEOUT_MS + 500);

    std::uint32_t id = 5000;

    for (node::Id n = node::MIN; n <= node::MAX; ++n) {
        char key[storage::KEY_MAX + 1];
        sensor::latest_key(n, sensor::kind_for_node(n), key, sizeof(key));

        h.send(protocol::MessageType::GET_REQ, ++id, key);
        protocol::DecodedMessage get = h.wait_response(id, raw);

        // C's own sensor value is still served by its replica.
        REQUIRE(get.payload.get_response.status == protocol::StatusCode::OK);

        sensor::Reading reading{};
        REQUIRE(sensor::decode(get.payload.get_response.value.data, get.payload.get_response.value.len, reading));
        REQUIRE(reading.kind == sensor::kind_for_node(n));
        REQUIRE(reading.sequence >= 2);
    }

    h.send(protocol::MessageType::SET_REQ, ++id, "pi/while-down", "ok");
    protocol::DecodedMessage set = h.wait_response(id, raw);
    REQUIRE(set.payload.set_ack.status == protocol::StatusCode::OK);

    h.send(protocol::MessageType::GET_REQ, ++id, "pi/while-down");
    protocol::DecodedMessage get = h.wait_response(id, raw);
    REQUIRE(get.payload.get_response.status == protocol::StatusCode::OK);

    h.send(protocol::MessageType::DEL_REQ, ++id, "pi/while-down");
    protocol::DecodedMessage del = h.wait_response(id, raw);
    REQUIRE(del.payload.delete_ack.status == protocol::StatusCode::OK);
}

void test_garbage_and_unsupported()
{
    Harness h;
    std::vector<std::uint8_t> raw;

    const std::uint8_t noise[] = {0x00, 0xAA, 0x55, 0x02, 0xFF, 0x13, 0xAA};
    h.bridge->on_bytes(noise, sizeof(noise), h.cluster.now());

    // A REPL_REQ from the Pi is not something it may send.
    packet::Buffer packet{};
    const packet::Route route{9, node::A, node::A, node::B, 1, protocol::FLAG_REPLICATED};
    REQUIRE(packet::build_request(protocol::MessageType::REPL_REQ, route,
                                  protocol::KeyView{reinterpret_cast<const std::uint8_t*>("k"), 1},
                                  protocol::ValueView{nullptr, 0}, packet));
    std::uint8_t frame[uart::MAX_FRAME];
    std::size_t length = 0;
    REQUIRE(uart::encode(packet.data, packet.length, frame, sizeof(frame), length));
    h.bridge->on_bytes(frame, length, h.cluster.now());

    REQUIRE(h.bridge->stats().unsupported == 1);

    // Still works afterwards.
    h.send(protocol::MessageType::SET_REQ, 4242, "after-noise", "fine");
    protocol::DecodedMessage set = h.wait_response(4242, raw);
    REQUIRE(set.payload.set_ack.status == protocol::StatusCode::OK);
}

} // namespace

int main()
{
    test_set_get_del_ping();
    test_sensors_with_node_down();
    test_garbage_and_unsupported();

    std::cout << "Bridge tests: PASS\n";
    return 0;
}

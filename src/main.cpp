#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#include "consistent_hash.h"
#include "hlc.h"
#include "membership.h"
#include "node_id.h"
#include "placement.h"
#include "platform_time.h"
#include "protocol.h"
#include "transport.h"

#ifndef NODE_ID
#error "NODE_ID must be defined by PlatformIO"
#endif

constexpr node::Id LOCAL_NODE_ID =
    static_cast<node::Id>(NODE_ID);

constexpr std::uint64_t HEARTBEAT_INTERVAL_MS = 500;
constexpr std::uint64_t MEMBERSHIP_TIMEOUT_MS = 2000;

constexpr std::uint8_t TEST_KEY[] = {
    'p', 'h', 'a', 's', 'e', '5', '_', 'f', 'a', 'i', 'l'
};

hlc::Clock hlc_clock(
    platform_time::now_ms
);

transport::EspNowTransport espnow_transport;

membership::Manager membership_manager(
    LOCAL_NODE_ID,
    MEMBERSHIP_TIMEOUT_MS
);

placement::Resolver placement_resolver;

bool initialized = false;
bool initial_placement_printed = false;
bool failure_reported = false;
bool recovery_reported = false;

std::uint64_t last_heartbeat_send_ms = 0;

placement::Assignment initial_assignment{};

static bool send_heartbeat()
{
    std::uint8_t buffer[
        protocol::MAX_PACKET_SIZE
    ]{};

    const protocol::PacketHeader header{
        protocol::PROTOCOL_VERSION,
        protocol::MessageType::HEARTBEAT,
        0,
        protocol::HEADER_SIZE,
        0,
        LOCAL_NODE_ID,
        LOCAL_NODE_ID,
        node::BROADCAST,
        0,
        hlc_clock.now(),
        0
    };

    std::size_t length = 0;

    const protocol::Result result =
        protocol::serialize_packet(
            header,
            protocol::ByteView{
                nullptr,
                0
            },
            buffer,
            sizeof(buffer),
            length
        );

    if (result != protocol::Result::OK) {
        return false;
    }

    return espnow_transport.send_broadcast(
        buffer,
        length
    ) == transport::Result::OK;
}

static void process_received_frames()
{
    transport::ReceivedFrame frame{};

    while (espnow_transport.receive(frame)) {
        protocol::PacketView packet{};

        if (
            protocol::deserialize_packet(
                frame.data,
                frame.length,
                packet
            ) != protocol::Result::OK
        ) {
            continue;
        }

        if (
            packet.header.msg_type !=
            protocol::MessageType::HEARTBEAT
        ) {
            continue;
        }

        if (
            packet.header.origin_node_id <
                node::MIN ||
            packet.header.origin_node_id >
                node::MAX
        ) {
            continue;
        }

        if (
            packet.header.sender_node_id !=
            packet.header.origin_node_id
        ) {
            continue;
        }

        membership_manager.heartbeat_received(
            packet.header.sender_node_id,
            platform_time::now_ms()
        );
    }
}

static void print_membership()
{
    const std::uint64_t now =
        platform_time::now_ms();

    Serial.print("ALIVE: ");

    for (
        node::Id id = node::MIN;
        id <= node::MAX;
        ++id
    ) {
        if (
            membership_manager.is_alive(
                id,
                now
            )
        ) {
            Serial.print(
                static_cast<unsigned>(id)
            );
            Serial.print(' ');
        }
    }

    Serial.println();
}

static bool all_nodes_alive(std::uint64_t now)
{
    for (
        node::Id id = node::MIN;
        id <= node::MAX;
        ++id
    ) {
        if (!membership_manager.is_alive(id, now)) {
            return false;
        }
    }

    return true;
}

static void check_placement()
{
    const std::uint64_t now =
        platform_time::now_ms();

    if (!initial_placement_printed) {
        if (!all_nodes_alive(now)) {
            return;
        }
    }

    placement::Assignment assignment{};

    if (
        !placement_resolver.locate(
            TEST_KEY,
            sizeof(TEST_KEY),
            membership_manager,
            now,
            assignment
        )
    ) {
        return;
    }

    if (!initial_placement_printed) {
        initial_assignment = assignment;
        initial_placement_printed = true;

        Serial.print(
            "INITIAL PLACEMENT: PRIMARY="
        );
        Serial.print(
            static_cast<unsigned>(
                assignment.primary
            )
        );
        Serial.print(
            " REPLICA="
        );
        Serial.println(
            static_cast<unsigned>(
                assignment.replica
            )
        );

        Serial.println(
            "ALL 4 NODES ALIVE: PASS"
        );

        Serial.println(
            "UNPLUG THE PRIMARY NODE NOW."
        );

        return;
    }

    if (
        !failure_reported &&
        initial_assignment.primary !=
            assignment.primary
    ) {
        failure_reported = true;

        Serial.println();
        Serial.println(
            "PRIMARY FAILURE DETECTED"
        );

        Serial.print(
            "OLD PRIMARY="
        );
        Serial.println(
            static_cast<unsigned>(
                initial_assignment.primary
            )
        );

        Serial.print(
            "NEW PRIMARY="
        );
        Serial.println(
            static_cast<unsigned>(
                assignment.primary
            )
        );

        Serial.print(
            "NEW REPLICA="
        );
        Serial.println(
            static_cast<unsigned>(
                assignment.replica
            )
        );

        if (
            assignment.primary ==
                initial_assignment.replica
        ) {
            Serial.println(
                "OLD REPLICA PROMOTION: PASS"
            );
        } else {
            Serial.println(
                "OLD REPLICA PROMOTION: FAIL"
            );
        }
    }

    if (
        failure_reported &&
        !recovery_reported &&
        assignment.primary ==
            initial_assignment.primary &&
        assignment.replica ==
            initial_assignment.replica
    ) {
        recovery_reported = true;

        Serial.println();
        Serial.println(
            "NODE REJOINED"
        );

        Serial.println(
            "ORIGINAL PLACEMENT RESTORED: PASS"
        );

        Serial.println();
        Serial.println(
            "PHASE 5 MEMBERSHIP + "
            "PLACEMENT TEST: PASS"
        );
    }
}
void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println(
        "========================================"
    );
    Serial.println(
        "PHASE 5 - MEMBERSHIP + PLACEMENT"
    );
    Serial.println(
        "HARDWARE FAILURE TEST"
    );
    Serial.println(
        "========================================"
    );

    if (!hlc_clock.begin(0)) {
        Serial.println(
            "HLC begin: FAIL"
        );
        return;
    }

    if (
        espnow_transport.begin(LOCAL_NODE_ID) !=
        transport::Result::OK
    ) {
        Serial.println(
            "Transport begin: FAIL"
        );
        return;
    }

    if (
        !membership_manager.begin(
            platform_time::now_ms()
        )
    ) {
        Serial.println(
            "Membership begin: FAIL"
        );
        return;
    }

    if (!placement_resolver.begin()) {
        Serial.println(
            "Placement begin: FAIL"
        );
        return;
    }

    initialized = true;

    Serial.print("LOCAL NODE: ");
    Serial.println(
        static_cast<unsigned>(
            LOCAL_NODE_ID
        )
    );

    Serial.print(
        "Heartbeat interval: "
    );
    Serial.print(
        static_cast<unsigned long>(
            HEARTBEAT_INTERVAL_MS
        )
    );
    Serial.println(" ms");

    Serial.print(
        "Membership timeout: "
    );
    Serial.print(
        static_cast<unsigned long>(
            MEMBERSHIP_TIMEOUT_MS
        )
    );
    Serial.println(" ms");

    Serial.println(
        "Waiting for all nodes..."
    );
}

void loop()
{
    if (!initialized) {
        delay(1000);
        return;
    }

    const std::uint64_t now =
        platform_time::now_ms();

    if (
        last_heartbeat_send_ms == 0 ||
        now - last_heartbeat_send_ms >=
            HEARTBEAT_INTERVAL_MS
    ) {
        if (!send_heartbeat()) {
            Serial.println(
                "Heartbeat send: FAIL"
            );
        }

        last_heartbeat_send_ms = now;
    }

    process_received_frames();

    check_placement();

    delay(5);
}
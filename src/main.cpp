#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#include "consistent_hash.h"
#include "hash_ring.h"
#include "membership.h"
#include "placement.h"
#include "platform_time.h"
#include "node_id.h"

#ifndef NODE_ID
#error "NODE_ID must be defined by PlatformIO"
#endif

constexpr node::Id LOCAL_NODE_ID =
    static_cast<node::Id>(NODE_ID);

constexpr std::uint64_t MEMBERSHIP_TIMEOUT_MS = 60000;

constexpr std::uint32_t TEST_KEY_COUNT = 1000;

membership::Manager membership_manager(
    LOCAL_NODE_ID,
    MEMBERSHIP_TIMEOUT_MS
);

placement::Resolver placement_resolver;

static bool test_complete = false;

static bool setup_membership()
{
    const std::uint64_t now =
        platform_time::now_ms();

    if (!membership_manager.begin(now)) {
        return false;
    }

    for (
        node::Id id = node::MIN;
        id <= node::MAX;
        ++id
    ) {
        if (id == LOCAL_NODE_ID) {
            continue;
        }

        if (!membership_manager.heartbeat_received(id, now)) {
            return false;
        }
    }

    return true;
}

static std::uint32_t ring_fingerprint(
    const hash_ring::Ring& ring
)
{
    std::uint32_t fingerprint =
        consistent_hash::FNV1A_OFFSET_BASIS;

    for (
        std::size_t i = 0;
        i < ring.point_count();
        ++i
    ) {
        const hash_ring::VirtualPoint& point =
            ring.point(i);

        const std::uint8_t bytes[] = {
            static_cast<std::uint8_t>(
                point.position >> 24
            ),
            static_cast<std::uint8_t>(
                point.position >> 16
            ),
            static_cast<std::uint8_t>(
                point.position >> 8
            ),
            static_cast<std::uint8_t>(
                point.position
            ),
            point.owner,
            point.virtual_index
        };

        fingerprint =
            consistent_hash::fnv1a(
                bytes,
                sizeof(bytes)
            ) ^ fingerprint;
    }

    return fingerprint;
}

static void run_test()
{
    Serial.println();
    Serial.println(
        "========================================"
    );
    Serial.println(
        "PHASE 5 - HARDWARE PLACEMENT TEST"
    );
    Serial.println(
        "========================================"
    );

    Serial.print("LOCAL NODE: ");
    Serial.println(LOCAL_NODE_ID);

    if (!setup_membership()) {
        Serial.println(
            "Membership setup: FAIL"
        );
        return;
    }

    Serial.println(
        "Membership setup: OK"
    );

    if (!placement_resolver.begin()) {
        Serial.println(
            "Placement begin: FAIL"
        );
        return;
    }

    Serial.println(
        "Placement begin: OK"
    );

    hash_ring::Ring ring;

    if (!ring.begin()) {
        Serial.println(
            "Ring begin: FAIL"
        );
        return;
    }

    Serial.print(
        "Ring points: "
    );
    Serial.println(
        static_cast<unsigned>(
            ring.point_count()
        )
    );

    Serial.print(
        "Ring fingerprint: 0x"
    );
    Serial.printf(
        "%08lX\n",
        static_cast<unsigned long>(
            ring_fingerprint(ring)
        )
    );

    std::size_t primary_counts[5]{};

    bool placement_ok = true;

    for (
        std::uint32_t i = 0;
        i < TEST_KEY_COUNT;
        ++i
    ) {
        const std::uint8_t key[] = {
            'p',
            'l',
            'a',
            'c',
            static_cast<std::uint8_t>(
                i & 0xFF
            ),
            static_cast<std::uint8_t>(
                (i >> 8) & 0xFF
            ),
            static_cast<std::uint8_t>(
                (i >> 16) & 0xFF
            ),
            static_cast<std::uint8_t>(
                (i >> 24) & 0xFF
            )
        };

        placement::Assignment assignment{};

        if (
            !placement_resolver.locate(
                key,
                sizeof(key),
                membership_manager,
                platform_time::now_ms(),
                assignment
            )
        ) {
            placement_ok = false;
            break;
        }

        if (
            assignment.primary == node::INVALID ||
            assignment.replica == node::INVALID ||
            assignment.primary == assignment.replica
        ) {
            placement_ok = false;
            break;
        }

        ++primary_counts[
            assignment.primary
        ];

        if (i < 5) {
            Serial.printf(
                "KEY %lu -> PRIMARY=%u REPLICA=%u\n",
                static_cast<unsigned long>(i),
                assignment.primary,
                assignment.replica
            );
        }
    }

    if (!placement_ok) {
        Serial.println(
            "Placement validation: FAIL"
        );
        return;
    }

    Serial.println(
        "Placement validation: OK"
    );

    Serial.printf(
        "PRIMARY COUNTS: A=%u B=%u C=%u D=%u\n",
        static_cast<unsigned>(
            primary_counts[node::A]
        ),
        static_cast<unsigned>(
            primary_counts[node::B]
        ),
        static_cast<unsigned>(
            primary_counts[node::C]
        ),
        static_cast<unsigned>(
            primary_counts[node::D]
        )
    );

    Serial.println(
        "PHASE 5 HARDWARE TEST: PASS"
    );
}

void setup()
{
    Serial.begin(115200);

    delay(1000);

    Serial.println();
    Serial.println(
        "Booting placement test..."
    );
}

void loop()
{
    if (!test_complete) {
        test_complete = true;
        run_test();
    }

    delay(1000);
}

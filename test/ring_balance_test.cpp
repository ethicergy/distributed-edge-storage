/*
 * Ring balance with VIRTUAL_NODES_PER_NODE points per node.
 * Work plan P2: no node may be primary for more than ~35 % of keys.
 * Measured (10k keys): A 25.9 %, B 29.0 %, C 25.5 %, D 19.5 %.
 */

#include <cstdio>
#include <iostream>

#include "membership.h"
#include "placement.h"
#include "test_support.h"

int main()
{
    placement::Resolver resolver;
    REQUIRE(resolver.begin());

    const placement::AliveMask all = 0x0F;

    int primary[node::MAX + 1] = {};
    int owner[node::MAX + 1] = {};

    constexpr int KEYS = 10000;
    char key[40];

    for (int i = 0; i < KEYS; ++i) {
        const int length = std::snprintf(key, sizeof(key), "key-%08x-%d", i * 2654435761u, i);

        placement::Assignment assignment{};
        REQUIRE(resolver.locate(reinterpret_cast<const std::uint8_t*>(key), static_cast<std::size_t>(length), all, assignment));
        REQUIRE(assignment.primary != assignment.replica);

        ++primary[assignment.primary];
        ++owner[assignment.primary];
        ++owner[assignment.replica];
    }

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        const double primary_share = 100.0 * primary[id] / KEYS;
        const double owner_share = 100.0 * owner[id] / (2.0 * KEYS);

        std::printf("node %u: primary %.1f %%, owner %.1f %%\n", id, primary_share, owner_share);

        REQUIRE(primary_share > 15.0 && primary_share < 35.0);
        REQUIRE(owner_share > 12.0 && owner_share < 38.0);
    }

    // The membership-based overload agrees with the mask overload.
    membership::Manager view(node::A, 2000);
    REQUIRE(view.begin(0));
    view.heartbeat_received(node::C, 0);

    for (int i = 0; i < 200; ++i) {
        const int length = std::snprintf(key, sizeof(key), "k%d", i);
        placement::Assignment by_view{};
        placement::Assignment by_mask{};

        REQUIRE(resolver.locate(reinterpret_cast<const std::uint8_t*>(key), static_cast<std::size_t>(length), view, 100, by_view));
        REQUIRE(resolver.locate(reinterpret_cast<const std::uint8_t*>(key), static_cast<std::size_t>(length),
                                placement::AliveMask(placement::mask_of(node::A) | placement::mask_of(node::C)), by_mask));

        REQUIRE(by_view.primary == by_mask.primary);
        REQUIRE(by_view.replica == by_mask.replica);
        REQUIRE(placement::is_owner(by_view, node::A) && placement::is_owner(by_view, node::C));
    }

    // Single live node: primary only, no replica.
    placement::Assignment alone{};
    REQUIRE(resolver.locate(reinterpret_cast<const std::uint8_t*>("x"), 1, placement::mask_of(node::B), alone));
    REQUIRE(alone.primary == node::B && alone.replica == node::INVALID);

    std::cout << "Ring balance tests: PASS\n";
    return 0;
}

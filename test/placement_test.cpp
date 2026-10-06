#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "membership.h"
#include "placement.h"

static membership::Manager make_membership(
    node::Id local,
    std::uint8_t live_mask,
    std::uint64_t now_ms
)
{
    membership::Manager manager(
        local,
        1000
    );

    assert(manager.begin(now_ms));

    for (
        node::Id id = node::MIN;
        id <= node::MAX;
        ++id
    ) {
        const std::uint8_t bit =
            static_cast<std::uint8_t>(1u << (id - 1));

        if (
            id != local &&
            (live_mask & bit) != 0
        ) {
            assert(
                manager.heartbeat_received(
                    id,
                    now_ms
                )
            );
        }
    }

    return manager;
}

static std::size_t count_live(
    std::uint8_t live_mask
)
{
    std::size_t count = 0;

    for (
        node::Id id = node::MIN;
        id <= node::MAX;
        ++id
    ) {
        if (
            live_mask &
            static_cast<std::uint8_t>(
                1u << (id - 1)
            )
        ) {
            ++count;
        }
    }

    return count;
}

static bool is_live(
    node::Id id,
    std::uint8_t live_mask
)
{
    return (
        live_mask &
        static_cast<std::uint8_t>(
            1u << (id - 1)
        )
    ) != 0;
}

int main()
{
    placement::Resolver resolver;

    assert(resolver.begin());
    assert(resolver.ready());

    constexpr std::uint64_t NOW = 1000;

    /*
     * Test every possible non-empty membership view.
     *
     * 1  = A alive
     * 2  = B alive
     * 4  = C alive
     * 8  = D alive
     *
     * Therefore masks 1..15 cover every possible
     * non-empty live-node combination.
     */
    for (
        std::uint8_t live_mask = 1;
        live_mask <= 15;
        ++live_mask
    ) {
        const std::size_t live_count =
            count_live(live_mask);

        for (
            std::uint32_t key_index = 0;
            key_index < 1000;
            ++key_index
        ) {
            const std::uint8_t key[] = {
                'k',
                static_cast<std::uint8_t>(
                    key_index & 0xFF
                ),
                static_cast<std::uint8_t>(
                    (key_index >> 8) & 0xFF
                ),
                static_cast<std::uint8_t>(
                    live_mask
                )
            };

            bool expected_set = false;
            placement::Assignment expected{};

            /*
             * Every alive node sees the same membership
             * view and must calculate the same placement
             * for this exact key.
             */
            for (
                node::Id local = node::MIN;
                local <= node::MAX;
                ++local
            ) {
                if (!is_live(local, live_mask)) {
                    continue;
                }

                membership::Manager membership =
                    make_membership(
                        local,
                        live_mask,
                        NOW
                    );

                placement::Assignment assignment{};

                assert(
                    resolver.locate(
                        key,
                        sizeof(key),
                        membership,
                        NOW,
                        assignment
                    )
                );

                assert(
                    is_live(
                        assignment.primary,
                        live_mask
                    )
                );

                if (live_count == 1) {
                    assert(
                        assignment.replica ==
                        node::INVALID
                    );
                } else {
                    assert(
                        assignment.replica !=
                        node::INVALID
                    );

                    assert(
                        assignment.replica !=
                        assignment.primary
                    );

                    assert(
                        is_live(
                            assignment.replica,
                            live_mask
                        )
                    );
                }

                if (!expected_set) {
                    expected = assignment;
                    expected_set = true;
                } else {
                    assert(
                        assignment.primary ==
                        expected.primary
                    );

                    assert(
                        assignment.replica ==
                        expected.replica
                    );
                }
            }
        }
    }

    /*
     * Failure transition tests.
     *
     * Start with all four nodes alive.
     */
    {
        membership::Manager all_alive =
            make_membership(
                node::A,
                0x0F,
                NOW
            );

        const std::uint8_t key[] = {
            'f', 'a', 'i', 'l'
        };

        placement::Assignment before{};

        assert(
            resolver.locate(
                key,
                sizeof(key),
                all_alive,
                NOW,
                before
            )
        );

        assert(before.primary != node::INVALID);
        assert(before.replica != node::INVALID);
        assert(before.primary != before.replica);

        /*
         * Kill the primary.
         *
         * With RF=2, the old replica is the first
         * different live node clockwise, so it must
         * be promoted to primary.
         */
        {
            const std::uint8_t live_mask =
                static_cast<std::uint8_t>(
                    0x0F &
                    ~(
                        1u <<
                        (before.primary - 1)
                    )
                );

            assert(count_live(live_mask) == 3);

            const node::Id local =
                before.primary == node::A
                    ? node::B
                    : node::A;

            assert(is_live(local, live_mask));

            membership::Manager membership =
                make_membership(
                    local,
                    live_mask,
                    NOW
                );

            placement::Assignment after{};

            assert(
                resolver.locate(
                    key,
                    sizeof(key),
                    membership,
                    NOW,
                    after
                )
            );

            assert(
                after.primary ==
                before.replica
            );

            assert(
                after.primary !=
                before.primary
            );

            assert(
                after.replica !=
                node::INVALID
            );

            assert(
                after.replica !=
                after.primary
            );

            assert(
                is_live(
                    after.replica,
                    live_mask
                )
            );
        }

        /*
         * Kill the replica.
         *
         * The primary must remain unchanged.
         * A different live node becomes replica.
         */
        {
            const std::uint8_t live_mask =
                static_cast<std::uint8_t>(
                    0x0F &
                    ~(
                        1u <<
                        (before.replica - 1)
                    )
                );

            assert(count_live(live_mask) == 3);

            node::Id local = node::INVALID;

            for (
                node::Id candidate = node::MIN;
                candidate <= node::MAX;
                ++candidate
            ) {
                if (is_live(candidate, live_mask)) {
                    local = candidate;
                    break;
                }
            }

            assert(local != node::INVALID);

            membership::Manager membership =
                make_membership(
                    local,
                    live_mask,
                    NOW
                );

            placement::Assignment after{};

            assert(
                resolver.locate(
                    key,
                    sizeof(key),
                    membership,
                    NOW,
                    after
                )
            );

            assert(
                after.primary ==
                before.primary
            );

            assert(
                after.replica !=
                node::INVALID
            );

            assert(
                after.replica !=
                before.replica
            );

            assert(
                after.replica !=
                after.primary
            );

            assert(
                is_live(
                    after.replica,
                    live_mask
                )
            );
        }
    }

    /*
     * One-node case.
     *
     * Placement still succeeds, but RF=2 cannot
     * be satisfied, so replica is INVALID.
     */
    {
        membership::Manager only_a(
            node::A,
            1000
        );

        assert(only_a.begin(NOW));

        const std::uint8_t key[] = {
            'o', 'n', 'e'
        };

        placement::Assignment result{};

        assert(
            resolver.locate(
                key,
                sizeof(key),
                only_a,
                NOW,
                result
            )
        );

        assert(result.primary == node::A);
        assert(result.replica == node::INVALID);
    }


    /*
     * Distribution sanity test.
     *
     * With all nodes alive, sample many keys and count
     * primary assignments. This does not require exact
     * percentages; it detects pathological skew.
     */
    {
        membership::Manager membership =
            make_membership(
                node::A,
                0x0F,
                NOW
            );

        std::size_t primary_counts[5]{};

        constexpr std::uint32_t KEY_COUNT = 100000;

        for (
            std::uint32_t i = 0;
            i < KEY_COUNT;
            ++i
        ) {
            const std::uint8_t key[] = {
                'd',
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

            assert(
                resolver.locate(
                    key,
                    sizeof(key),
                    membership,
                    NOW,
                    assignment
                )
            );

            ++primary_counts[assignment.primary];
        }

        std::size_t minimum = KEY_COUNT;
        std::size_t maximum = 0;

        for (
            node::Id id = node::MIN;
            id <= node::MAX;
            ++id
        ) {
            assert(primary_counts[id] > 0);

            if (primary_counts[id] < minimum) {
                minimum = primary_counts[id];
            }

            if (primary_counts[id] > maximum) {
                maximum = primary_counts[id];
            }
        }

        assert(maximum <= minimum * 3);

        std::printf(
            "Distribution: A=%zu B=%zu C=%zu D=%zu\n",
            primary_counts[node::A],
            primary_counts[node::B],
            primary_counts[node::C],
            primary_counts[node::D]
        );
    }

    return 0;
}

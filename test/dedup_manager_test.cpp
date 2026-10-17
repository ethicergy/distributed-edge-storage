#include <cassert>
#include <cstdint>
#include <iostream>

#include "dedup_manager.h"

static request::Identity identity(
    node::Id origin,
    std::uint32_t request_id
)
{
    return request::Identity{
        origin,
        request_id
    };
}

static void test_new_request()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::A, 100);

    assert(
        manager.begin_request(
            id,
            0
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    assert(manager.active_count() == 1);

    request::DedupState state{};

    assert(
        manager.get_state(
            id,
            state,
            0
        ) ==
        request::DedupResult::
            DUPLICATE_IN_PROGRESS
    );

    assert(
        state ==
        request::DedupState::IN_PROGRESS
    );
}

static void test_duplicate_in_progress()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::A, 200);

    assert(
        manager.begin_request(
            id,
            100
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    assert(
        manager.begin_request(
            id,
            200
        ) ==
        request::DedupResult::
            DUPLICATE_IN_PROGRESS
    );

    assert(manager.active_count() == 1);
}

static void test_completion_and_replay()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::A, 300);

    assert(
        manager.begin_request(
            id,
            1000
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    const std::uint8_t response[] = {
        0x10,
        0x20,
        0x30
    };

    assert(
        manager.complete_request(
            id,
            response,
            sizeof(response),
            1100
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    /*
     * Same request after completion.
     */
    assert(
        manager.begin_request(
            id,
            1200
        ) ==
        request::DedupResult::
            DUPLICATE_COMPLETED
    );

    std::uint8_t output[8]{};
    std::size_t output_length = 0;

    assert(
        manager.get_cached_response(
            id,
            output,
            sizeof(output),
            output_length,
            1200
        ) ==
        request::DedupResult::
            DUPLICATE_COMPLETED
    );

    assert(output_length == 3);

    assert(output[0] == 0x10);
    assert(output[1] == 0x20);
    assert(output[2] == 0x30);
}

static void test_retention_expiry()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::B, 400);

    assert(
        manager.begin_request(
            id,
            1000
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    const std::uint8_t response[] = {
        0x42
    };

    assert(
        manager.complete_request(
            id,
            response,
            sizeof(response),
            2000
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    /*
     * Nine seconds after completion:
     * still retained.
     */
    assert(
        manager.begin_request(
            id,
            10999
        ) ==
        request::DedupResult::
            DUPLICATE_COMPLETED
    );

    /*
     * Ten seconds after completion:
     * expired.
     */
    assert(
        manager.begin_request(
            id,
            12000
        ) ==
        request::DedupResult::NEW_REQUEST
    );
}

static void test_in_progress_does_not_expire()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::C, 500);

    assert(
        manager.begin_request(
            id,
            0
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    /*
     * IN_PROGRESS has no retention timer.
     * It remains present.
     */
    assert(
        manager.begin_request(
            id,
            100000
        ) ==
        request::DedupResult::
            DUPLICATE_IN_PROGRESS
    );
}

static void test_unknown_completion()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::D, 600);

    const std::uint8_t response[] = {
        0xAA
    };

    assert(
        manager.complete_request(
            id,
            response,
            sizeof(response),
            0
        ) ==
        request::DedupResult::
            UNKNOWN_REQUEST
    );
}

static void test_buffer_too_small()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const auto id =
        identity(node::A, 700);

    assert(
        manager.begin_request(
            id,
            0
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    const std::uint8_t response[] = {
        1, 2, 3, 4
    };

    assert(
        manager.complete_request(
            id,
            response,
            sizeof(response),
            10
        ) ==
        request::DedupResult::NEW_REQUEST
    );

    std::uint8_t output[2]{};
    std::size_t output_length = 0;

    assert(
        manager.get_cached_response(
            id,
            output,
            sizeof(output),
            output_length,
            10
        ) ==
        request::DedupResult::
            BUFFER_TOO_SMALL
    );
}

static void test_table_limit()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    for (
        std::uint32_t i = 1;
        i <= request::MAX_DEDUP_ENTRIES;
        ++i
    ) {

        assert(
            manager.begin_request(
                identity(
                    node::A,
                    i
                ),
                0
            ) ==
            request::DedupResult::NEW_REQUEST
        );
    }

    assert(
        manager.active_count() ==
        request::MAX_DEDUP_ENTRIES
    );

    assert(
        manager.begin_request(
            identity(
                node::B,
                1
            ),
            0
        ) ==
        request::DedupResult::TABLE_FULL
    );
}

static void test_invalid_identity()
{
    request::DedupManager manager(10000);

    assert(manager.begin());

    const request::Identity invalid{
        node::INVALID,
        0
    };

    assert(
        manager.begin_request(
            invalid,
            0
        ) ==
        request::DedupResult::
            INVALID_ARGUMENT
    );
}

static void test_in_progress_reclaimed_after_timeout()
{
    request::DedupManager manager(10000, 3000);

    assert(manager.begin());

    const auto id =
        identity(node::B, 7);

    assert(
        manager.begin_request(id, 100) ==
        request::DedupResult::NEW_REQUEST
    );

    // Still protected just before the timeout.
    assert(
        manager.begin_request(id, 3099) ==
        request::DedupResult::DUPLICATE_IN_PROGRESS
    );

    // Leaked entry is reclaimed; the slot is free again.
    manager.expire(3100);

    assert(manager.active_count() == 0);

    assert(
        manager.begin_request(id, 3100) ==
        request::DedupResult::NEW_REQUEST
    );

    // A completed entry still follows the retention timer.
    const std::uint8_t response[] = {1, 2, 3};

    assert(
        manager.complete_request(
            id,
            response,
            sizeof(response),
            3200
        ) == request::DedupResult::NEW_REQUEST
    );

    manager.expire(3200 + 9999);
    assert(manager.active_count() == 1);

    manager.expire(3200 + 10000);
    assert(manager.active_count() == 0);
}

static void test_eviction_under_pressure()
{
    request::DedupManager manager(10000, 0, 3000);

    assert(manager.begin());

    const std::uint8_t response[] = {9};

    // Fill the table with completed entries at t = 0..N-1.
    for (std::uint32_t i = 0;
         i < request::MAX_DEDUP_ENTRIES;
         ++i) {

        const auto id = identity(node::A, 1000 + i);

        assert(manager.begin_request(id, i) ==
               request::DedupResult::NEW_REQUEST);

        assert(manager.complete_request(id, response, 1, i) ==
               request::DedupResult::NEW_REQUEST);
    }

    // Too young to evict: a retry could still arrive.
    assert(manager.begin_request(identity(node::B, 1), 2999) ==
           request::DedupResult::TABLE_FULL);

    // The oldest entry (completed at t=0) is now evictable.
    assert(manager.begin_request(identity(node::B, 1), 3000) ==
           request::DedupResult::NEW_REQUEST);

    request::DedupState state{};

    assert(manager.get_state(identity(node::A, 1000), state, 3000) ==
           request::DedupResult::UNKNOWN_REQUEST);

    // The next-oldest one is still cached.
    assert(manager.get_state(identity(node::A, 1001), state, 3000) ==
           request::DedupResult::DUPLICATE_COMPLETED);

    // IN_PROGRESS entries are never evicted.
    request::DedupManager busy(10000, 0, 1);
    assert(busy.begin());

    for (std::uint32_t i = 0;
         i < request::MAX_DEDUP_ENTRIES;
         ++i) {
        assert(busy.begin_request(identity(node::C, 1 + i), 0) ==
               request::DedupResult::NEW_REQUEST);
    }

    assert(busy.begin_request(identity(node::D, 1), 100000) ==
           request::DedupResult::TABLE_FULL);
}

int main()
{
    test_eviction_under_pressure();
    test_in_progress_reclaimed_after_timeout();
    test_new_request();
    test_duplicate_in_progress();
    test_completion_and_replay();
    test_retention_expiry();
    test_in_progress_does_not_expire();
    test_unknown_completion();
    test_buffer_too_small();
    test_table_limit();
    test_invalid_identity();

    std::cout
        << "Dedup manager tests: PASS\n";

    return 0;
}
#include <cassert>
#include <cstdint>
#include <iostream>

#include "request_manager.h"

static int send_count = 0;
static int retry_count = 0;


static node::Id last_destination = node::INVALID;

static bool fake_send(
    void*,
    node::Id destination,
    const std::uint8_t*,
    std::size_t,
    bool retry
)
{
    ++send_count;

    if (retry) {
        ++retry_count;
    }


    last_destination = destination;

    return true;
}

static request::Identity make_identity(
    node::Id origin,
    std::uint32_t id
)
{
    return request::Identity{
        origin,
        id
    };
}

static void test_basic_completion()
{
    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    const std::uint8_t packet[] = {
        0xAA,
        0xBB,
        0xCC
    };

    bool initial_send_ok = false;

    const request::Identity identity =
        make_identity(
            node::A,
            100
        );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::OK
    );

    assert(initial_send_ok);
    assert(send_count == 1);
    assert(manager.pending_count() == 1);

    request::State state{};

    assert(
        manager.get_state(
            identity,
            state
        ) ==
        request::Result::OK
    );

    assert(
        state == request::State::PENDING
    );

    const std::uint8_t response[] = {
        0x42
    };

    assert(
        manager.complete(
            identity,
            response,
            sizeof(response)
        ) ==
        request::Result::OK
    );

    assert(manager.pending_count() == 0);

    assert(
        manager.get_state(
            identity,
            state
        ) ==
        request::Result::OK
    );

    assert(
        state == request::State::COMPLETED
    );

    std::uint8_t output[8]{};
    std::size_t output_length = 0;

    assert(
        manager.get_completion(
            identity,
            output,
            sizeof(output),
            output_length
        ) ==
        request::Result::OK
    );

    assert(output_length == 1);
    assert(output[0] == 0x42);
}

static void test_retry_schedule()
{
    send_count = 0;
    retry_count = 0;

    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    const std::uint8_t packet[] = {
        0x01
    };

    bool initial_send_ok = false;

    const request::Identity identity =
        make_identity(
            node::A,
            200
        );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::OK
    );

    assert(initial_send_ok);
    assert(send_count == 1);

    /*
     * Before 500 ms: nothing.
     */
    manager.tick(499);

    assert(send_count == 1);
    assert(retry_count == 0);

    /*
     * Retry #1 at 500 ms.
     */
    manager.tick(500);

    assert(send_count == 2);
    assert(retry_count == 1);

    /*
     * Backoff:
     *
     * 500 + 1000 = 1500
     */
    manager.tick(1499);

    assert(send_count == 2);

    manager.tick(1500);

    assert(send_count == 3);
    assert(retry_count == 2);

    /*
     * Next retry:
     *
     * 1500 + 2000 = 3500
     */
    manager.tick(3499);

    assert(send_count == 3);

    manager.tick(3500);

    assert(send_count == 4);
    assert(retry_count == 3);
}

static void test_deadline()
{
    send_count = 0;
    retry_count = 0;

    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    const std::uint8_t packet[] = {
        0x02
    };

    bool initial_send_ok = false;

    const request::Identity identity =
        make_identity(
            node::A,
            300
        );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::OK
    );

    manager.tick(500);
    manager.tick(1500);
    manager.tick(3500);

    assert(retry_count == 3);

    request::State state{};

    assert(
        manager.get_state(
            identity,
            state
        ) ==
        request::Result::OK
    );

    assert(
        state == request::State::PENDING
    );

    /*
     * Overall deadline = 5000 ms.
     */
    manager.tick(4999);

    assert(
        manager.get_state(
            identity,
            state
        ) ==
        request::Result::OK
    );

    assert(
        state == request::State::PENDING
    );

    manager.tick(5000);

    assert(
        manager.get_state(
            identity,
            state
        ) ==
        request::Result::OK
    );

    assert(
        state == request::State::FAILED
    );
}

static void test_duplicate_identity()
{
    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    const std::uint8_t packet[] = {
        0x03
    };

    bool initial_send_ok = false;

    const request::Identity identity =
        make_identity(
            node::A,
            400
        );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::OK
    );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::DUPLICATE
    );
}

static void test_late_response()
{
    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    const std::uint8_t packet[] = {
        0x04
    };

    bool initial_send_ok = false;

    const request::Identity identity =
        make_identity(
            node::A,
            500
        );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::OK
    );

    /*
     * Terminal timeout.
     */
    manager.tick(500);
    manager.tick(1500);
    manager.tick(3500);
    manager.tick(5000);

    const std::uint8_t response[] = {
        0x55
    };

    assert(
        manager.complete(
            identity,
            response,
            sizeof(response)
        ) ==
        request::Result::ALREADY_TERMINAL
    );
}

static void test_immediate_send_failure_still_retries()
{
    send_count = 0;
    retry_count = 0;

    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    bool initial_send_ok = false;

    const std::uint8_t packet[] = {
        0x05
    };

    /*
     * Callback deliberately reports transport failure.
     */
    auto failing_send =
        [](void*,
           node::Id,
           const std::uint8_t*,
           std::size_t,
           bool retry) -> bool {

            if (retry) {
                ++retry_count;
            }

            ++send_count;

            return false;
        };

    const request::Identity identity =
        make_identity(
            node::A,
            600
        );

    assert(
        manager.submit(
            identity,
            node::B,
            packet,
            sizeof(packet),
            0,
            failing_send,
            nullptr,
            initial_send_ok
        ) ==
        request::Result::OK
    );

    assert(!initial_send_ok);
    assert(send_count == 1);

    manager.tick(500);

    assert(retry_count == 1);

    request::State state{};

    assert(
        manager.get_state(
            identity,
            state
        ) ==
        request::Result::OK
    );

    assert(
        state == request::State::PENDING
    );
}

/*
 * A primary replicates to a replica that may also be the client of
 * the original request, so destination == identity.origin is valid.
 */
static void test_destination_may_equal_origin()
{
    request::Manager manager;

    assert(
        manager.begin() ==
        request::Result::OK
    );

    const std::uint8_t packet[] = {0x01};
    bool initial_send_ok = false;

    assert(
        manager.submit(
            make_identity(node::C, 42),
            node::C,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) == request::Result::OK
    );

    assert(initial_send_ok);
    assert(last_destination == node::C);

    // Invalid destinations are still rejected.
    assert(
        manager.submit(
            make_identity(node::C, 43),
            node::INVALID,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) == request::Result::INVALID_ARGUMENT
    );

    assert(
        manager.submit(
            make_identity(node::C, 44),
            node::BROADCAST,
            packet,
            sizeof(packet),
            0,
            fake_send,
            nullptr,
            initial_send_ok
        ) == request::Result::INVALID_ARGUMENT
    );

    send_count = 0;
    retry_count = 0;
    last_destination = node::INVALID;
}

int main()
{
    test_destination_may_equal_origin();
    test_basic_completion();
    test_retry_schedule();
    test_deadline();
    test_duplicate_identity();
    test_late_response();
    test_immediate_send_failure_still_retries();

    std::cout
        << "Request manager tests: PASS\n";

    return 0;
}
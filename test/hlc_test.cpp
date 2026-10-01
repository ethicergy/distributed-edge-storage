#include <cassert>
#include <cstdint>
#include <iostream>

#include "hlc.h"

using namespace hlc;

static std::uint64_t fake_now_ms = 0;
static bool advance_before_next_return = false;
static std::uint64_t advance_to_ms = 0;

static std::uint64_t fake_physical_now()
{
    if (advance_before_next_return) {
        fake_now_ms = advance_to_ms;
        advance_before_next_return = false;
    }

    return fake_now_ms;
}

static void test_encoding()
{
    const Hlc value =
        make(
            0x123456789ABCuLL,
            0xBEEF
        );

    assert(
        physical(value) ==
        0x123456789ABCuLL
    );

    assert(
        logical(value) ==
        0xBEEF
    );
}

static void test_comparison()
{
    assert(
        compare(
            make(100, 1),
            make(100, 2)
        ) < 0
    );

    assert(
        compare(
            make(101, 0),
            make(100, 0)
        ) > 0
    );

    assert(
        compare(
            make(500, 9),
            make(500, 9)
        ) == 0
    );
}

static void test_reboot_anchor()
{
    fake_now_ms = 100;

    Clock clock(fake_physical_now);

    assert(
        clock.begin(
            make(1000, 7)
        )
    );

    assert(clock.ready());

    Hlc value = clock.now();

    /*
     * Raw timer = 100 ms.
     * Persisted physical = 1000 ms.
     * Runtime offset must reconstruct physical = 1000.
     */
    assert(physical(value) == 1000);
    assert(logical(value) == 7);

    /*
     * Same physical millisecond:
     * logical counter increments.
     */
    value = clock.on_local_event();

    assert(physical(value) == 1000);
    assert(logical(value) == 8);

    /*
     * Raw clock advances by 1 ms.
     * Reconstructed physical clock advances too.
     */
    fake_now_ms = 101;

    value = clock.on_local_event();

    assert(physical(value) == 1001);
    assert(logical(value) == 0);
}

static void test_receive_rules()
{
    fake_now_ms = 1000;

    Clock clock(fake_physical_now);

    assert(
        clock.begin(
            make(1000, 4)
        )
    );

    /*
     * p == local == remote
     */
    Hlc value =
        clock.on_receive(
            make(1000, 9)
        );

    assert(physical(value) == 1000);
    assert(logical(value) == 10);

    /*
     * physical_now is larger than both.
     */
    fake_now_ms = 1001;

    value =
        clock.on_receive(
            make(900, 50)
        );

    assert(physical(value) == 1001);
    assert(logical(value) == 0);

    /*
     * p == remote
     */
    value =
        clock.on_receive(
            make(1100, 50)
        );

    assert(physical(value) == 1100);
    assert(logical(value) == 51);
}

static void test_logical_overflow()
{
    fake_now_ms = 100;

    Clock clock(fake_physical_now);

    assert(
        clock.begin(
            make(500, LOGICAL_MAX)
        )
    );

    /*
     * First call observes the same physical millisecond,
     * so logical counter is already at maximum.
     *
     * The next physical-time read advances to raw 101,
     * giving reconstructed physical 501.
     */
    advance_before_next_return = true;
    advance_to_ms = 101;

    Hlc value =
        clock.on_local_event();

    assert(physical(value) == 501);
    assert(logical(value) == 0);
}

int main()
{
    test_encoding();
    test_comparison();
    test_reboot_anchor();
    test_receive_rules();
    test_logical_overflow();

    std::cout << "HLC tests: PASS\n";

    return 0;
}
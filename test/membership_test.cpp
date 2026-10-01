#include <cassert>
#include <cstdint>
#include <iostream>

#include "membership.h"

using namespace membership;

static void test_initial_state()
{
    Manager manager(
        node::A,
        1000
    );

    assert(manager.begin(500));

    assert(manager.ready());

    assert(manager.local_node_id() == node::A);

    /*
     * Self is always alive.
     */
    assert(
        manager.is_alive(
            node::A,
            500
        )
    );

    /*
     * Other nodes are unknown until observed.
     */
    assert(
        !manager.is_alive(
            node::B,
            500
        )
    );

    assert(!manager.seen(node::B));
}

static void test_heartbeat_and_timeout()
{
    Manager manager(
        node::A,
        1000
    );

    assert(manager.begin(1000));

    assert(
        manager.heartbeat_received(
            node::B,
            1200
        )
    );

    assert(manager.seen(node::B));

    assert(
        manager.last_heard(node::B) ==
        1200
    );

    /*
     * 500 ms since last heartbeat.
     */
    assert(
        manager.is_alive(
            node::B,
            1700
        )
    );

    /*
     * Exactly at timeout is still alive.
     */
    assert(
        manager.is_alive(
            node::B,
            2200
        )
    );

    /*
     * One millisecond beyond timeout.
     */
    assert(
        !manager.is_alive(
            node::B,
            2201
        )
    );
}

static void test_recovery_after_new_heartbeat()
{
    Manager manager(
        node::A,
        1000
    );

    assert(manager.begin(0));

    assert(
        manager.heartbeat_received(
            node::C,
            100
        )
    );

    assert(
        !manager.is_alive(
            node::C,
            1101
        )
    );

    /*
     * A new heartbeat makes the node alive again.
     */
    assert(
        manager.heartbeat_received(
            node::C,
            1200
        )
    );

    assert(
        manager.is_alive(
            node::C,
            1200
        )
    );
}

static void test_all_nodes_are_independent()
{
    Manager manager(
        node::B,
        1000
    );

    assert(manager.begin(1000));

    assert(
        manager.heartbeat_received(
            node::A,
            1000
        )
    );

    assert(
        manager.heartbeat_received(
            node::C,
            1500
        )
    );

    /*
     * A's observation expires while C remains alive.
     */
    assert(
        !manager.is_alive(
            node::A,
            2101
        )
    );

    assert(
        manager.is_alive(
            node::C,
            2101
        )
    );

    /*
     * D has never been seen.
     */
    assert(
        !manager.is_alive(
            node::D,
            2101
        )
    );

    /*
     * B itself remains alive.
     */
    assert(
        manager.is_alive(
            node::B,
            2101
        )
    );
}

static void test_invalid_nodes()
{
    Manager manager(
        node::A,
        1000
    );

    assert(manager.begin(0));

    assert(
        !manager.heartbeat_received(
            node::INVALID,
            100
        )
    );

    assert(
        !manager.is_alive(
            node::INVALID,
            100
        )
    );

    assert(
        !manager.seen(
            node::INVALID
        )
    );
}

static void test_self_heartbeat()
{
    Manager manager(
        node::A,
        1000
    );

    assert(manager.begin(500));

    /*
     * Self heartbeat is harmless but must not
     * turn into a remote observation.
     */
    assert(
        manager.heartbeat_received(
            node::A,
            900
        )
    );

    assert(
        manager.is_alive(
            node::A,
            5000
        )
    );
}

int main()
{
    test_initial_state();
    test_heartbeat_and_timeout();
    test_recovery_after_new_heartbeat();
    test_all_nodes_are_independent();
    test_invalid_nodes();
    test_self_heartbeat();

    std::cout << "Membership tests: PASS\n";

    return 0;
}
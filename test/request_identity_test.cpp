#include <cassert>
#include <cstdint>
#include <iostream>

#include "request_identity.h"

static void test_identity_validity()
{
    request::Identity valid{
        node::A,
        123
    };

    assert(valid.valid());

    request::Identity invalid_id{
        node::A,
        0
    };

    assert(!invalid_id.valid());

    request::Identity invalid_node{
        node::INVALID,
        123
    };

    assert(!invalid_node.valid());
}

static void test_equality()
{
    request::Identity a{
        node::A,
        100
    };

    request::Identity b{
        node::A,
        100
    };

    request::Identity c{
        node::A,
        101
    };

    request::Identity d{
        node::B,
        100
    };

    assert(a == b);
    assert(!(a != b));

    assert(a != c);
    assert(a != d);
}

static void test_generator()
{
    request::Generator generator;

    assert(!generator.ready());

    assert(
        generator.begin(
            node::A,
            100
        )
    );

    assert(generator.ready());

    request::Identity first =
        generator.next();

    request::Identity second =
        generator.next();

    request::Identity third =
        generator.next();

    assert(first.origin == node::A);
    assert(first.request_id == 101);

    assert(second.origin == node::A);
    assert(second.request_id == 102);

    assert(third.origin == node::A);
    assert(third.request_id == 103);

    assert(first != second);
    assert(second != third);
}

static void test_zero_is_never_generated()
{
    request::Generator generator;

    assert(
        generator.begin(
            node::B,
            0xFFFFFFFFu
        )
    );

    request::Identity identity =
        generator.next();

    assert(identity.origin == node::B);
    assert(identity.request_id == 1);
    assert(identity.valid());

    identity =
        generator.next();

    assert(identity.request_id == 2);
}

static void test_invalid_origin()
{
    request::Generator generator;

    assert(
        !generator.begin(
            node::INVALID,
            100
        )
    );

    assert(!generator.ready());

    request::Identity identity =
        generator.next();

    assert(!identity.valid());
}

static void test_node_is_preserved()
{
    request::Generator generator;

    assert(
        generator.begin(
            node::D,
            9000
        )
    );

    for (int i = 0; i < 10; ++i) {
        const request::Identity identity =
            generator.next();

        assert(identity.origin == node::D);
        assert(identity.request_id != 0);
    }
}

int main()
{
    test_identity_validity();
    test_equality();
    test_generator();
    test_zero_is_never_generated();
    test_invalid_origin();
    test_node_is_preserved();

    std::cout
        << "Request identity tests: PASS\n";

    return 0;
}
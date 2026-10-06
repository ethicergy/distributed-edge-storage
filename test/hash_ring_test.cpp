#include <cassert>
#include <cstddef>
#include <cstdint>

#include "hash_ring.h"

int main()
{
    hash_ring::Ring ring;

    assert(ring.begin());
    assert(ring.ready());

    assert(
        ring.point_count() ==
        hash_ring::RING_POINT_COUNT
    );

    std::size_t owner_counts[5]{};

    for (
        std::size_t i = 0;
        i < ring.point_count();
        ++i
    ) {
        const hash_ring::VirtualPoint& point =
            ring.point(i);

        assert(point.owner >= node::MIN);
        assert(point.owner <= node::MAX);

        assert(
            point.virtual_index <
            hash_ring::VIRTUAL_NODES_PER_NODE
        );

        ++owner_counts[point.owner];
    }

    assert(owner_counts[node::A] == 4);
    assert(owner_counts[node::B] == 4);
    assert(owner_counts[node::C] == 4);
    assert(owner_counts[node::D] == 4);

    for (
        std::size_t i = 1;
        i < ring.point_count();
        ++i
    ) {
        assert(
            ring.point(i - 1).position <
            ring.point(i).position
        );
    }

    /*
     * Exact point must return itself.
     */
    for (
        std::size_t i = 0;
        i < ring.point_count();
        ++i
    ) {
        hash_ring::VirtualPoint result{};

        assert(
            ring.find_successor(
                ring.point(i).position,
                result
            )
        );

        assert(
            result.position ==
            ring.point(i).position
        );

        assert(
            result.owner ==
            ring.point(i).owner
        );

        assert(
            result.virtual_index ==
            ring.point(i).virtual_index
        );
    }

    /*
     * A hash before the first point returns
     * the first point.
     */
    {
        hash_ring::VirtualPoint result{};

        assert(
            ring.find_successor(
                0,
                result
            )
        );

        assert(
            result.position ==
            ring.point(0).position
        );
    }

    /*
     * A hash after the final point wraps to
     * the first point.
     */
    {
        hash_ring::VirtualPoint result{};

        assert(
            ring.find_successor(
                0xFFFFFFFFu,
                result
            )
        );

        assert(
            result.position ==
            ring.point(0).position
        );
    }

    /*
     * Two separately constructed rings must
     * produce identical lookup results.
     */
    hash_ring::Ring second_ring;

    assert(second_ring.begin());

    for (std::uint32_t i = 0; i < 1000; ++i) {
        const std::uint32_t hash =
            i * 123456789u;
        hash_ring::VirtualPoint a{};
        hash_ring::VirtualPoint b{};

        assert(ring.find_successor(hash, a));
        assert(second_ring.find_successor(hash, b));

        assert(a.position == b.position);
        assert(a.owner == b.owner);
        assert(a.virtual_index == b.virtual_index);
    }

    return 0;
}

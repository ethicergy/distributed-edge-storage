#include "hash_ring.h"

#include "consistent_hash.h"

namespace hash_ring {

namespace {

std::uint32_t mix32(std::uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;

    return value;
}

std::uint32_t virtual_point_hash(
    node::Id id,
    std::uint8_t virtual_index
)
{
    const std::uint8_t seed[] = {
        id,
        virtual_index,
        0xC3,
        0x28
    };

    return mix32(
        consistent_hash::fnv1a(
            seed,
            sizeof(seed)
        )
    );
}

} // namespace

bool Ring::less(
    const VirtualPoint& lhs,
    const VirtualPoint& rhs
)
{
    if (lhs.position != rhs.position) {
        return lhs.position < rhs.position;
    }

    if (lhs.owner != rhs.owner) {
        return lhs.owner < rhs.owner;
    }

    return lhs.virtual_index < rhs.virtual_index;
}

bool Ring::begin()
{
    std::size_t position = 0;

    for (
        node::Id id = node::MIN;
        id <= node::MAX;
        ++id
    ) {
        for (
            std::uint8_t virtual_index = 0;
            virtual_index < VIRTUAL_NODES_PER_NODE;
            ++virtual_index
        ) {
            points_[position++] = VirtualPoint{
                virtual_point_hash(
                    id,
                    virtual_index
                ),
                id,
                virtual_index
            };
        }
    }

    for (
        std::size_t i = 1;
        i < RING_POINT_COUNT;
        ++i
    ) {
        const VirtualPoint current =
            points_[i];

        std::size_t j = i;

        while (
            j > 0 &&
            less(
                current,
                points_[j - 1]
            )
        ) {
            points_[j] =
                points_[j - 1];

            --j;
        }

        points_[j] = current;
    }

    initialized_ = true;

    return true;
}

bool Ring::ready() const
{
    return initialized_;
}

std::size_t Ring::point_count() const
{
    return RING_POINT_COUNT;
}

const VirtualPoint& Ring::point(
    std::size_t index
) const
{
    return points_[index];
}

bool Ring::find_successor(
    std::uint32_t hash,
    VirtualPoint& result
) const
{
    std::size_t index = 0;

    if (!find_successor_index(hash, index)) {
        return false;
    }

    result = points_[index];

    return true;
}

bool Ring::find_successor_index(
    std::uint32_t hash,
    std::size_t& result_index
) const
{
    if (!initialized_) {
        return false;
    }

    std::size_t left = 0;
    std::size_t right = RING_POINT_COUNT;

    while (left < right) {
        const std::size_t middle =
            left + (right - left) / 2;

        if (points_[middle].position < hash) {
            left = middle + 1;
        } else {
            right = middle;
        }
    }

    if (left == RING_POINT_COUNT) {
        result_index = 0;
    } else {
        result_index = left;
    }

    return true;
}

} // namespace hash_ring

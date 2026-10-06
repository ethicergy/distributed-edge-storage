#pragma once

#include <cstddef>
#include <cstdint>

#include "node_id.h"

namespace hash_ring {

constexpr std::size_t VIRTUAL_NODES_PER_NODE = 4;

constexpr std::size_t RING_POINT_COUNT =
    static_cast<std::size_t>(
        node::MAX - node::MIN + 1
    ) * VIRTUAL_NODES_PER_NODE;

struct VirtualPoint {
    std::uint32_t position;
    node::Id owner;
    std::uint8_t virtual_index;
};

class Ring {
public:
    bool begin();

    bool ready() const;

    std::size_t point_count() const;

    const VirtualPoint& point(
        std::size_t index
    ) const;

    bool find_successor(
        std::uint32_t hash,
        VirtualPoint& result
    ) const;

    bool find_successor_index(
        std::uint32_t hash,
        std::size_t& result_index
    ) const;

private:
    static bool less(
        const VirtualPoint& lhs,
        const VirtualPoint& rhs
    );

    VirtualPoint points_[RING_POINT_COUNT]{};
    bool initialized_ = false;
};

} // namespace hash_ring

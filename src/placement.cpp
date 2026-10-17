#include "placement.h"

#include "consistent_hash.h"

namespace placement {

bool Resolver::begin()
{
    if (!ring_.begin()) {
        return false;
    }

    initialized_ = true;

    return true;
}

bool Resolver::ready() const
{
    return initialized_;
}

AliveMask alive_mask(
    const membership::Manager& membership,
    std::uint64_t now_ms
)
{
    AliveMask mask = 0;

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        if (membership.is_alive(id, now_ms)) {
            mask = static_cast<AliveMask>(mask | mask_of(id));
        }
    }

    return mask;
}

bool Resolver::locate(
    const std::uint8_t* key,
    std::size_t key_length,
    const membership::Manager& membership,
    std::uint64_t now_ms,
    Assignment& result
) const
{
    return locate(
        key,
        key_length,
        alive_mask(membership, now_ms),
        result
    );
}

bool Resolver::locate(
    const std::uint8_t* key,
    std::size_t key_length,
    AliveMask alive,
    Assignment& result
) const
{
    auto is_alive = [alive](node::Id id) {
        return id >= node::MIN &&
               id <= node::MAX &&
               (alive & mask_of(id)) != 0;
    };

    if (!initialized_) {
        return false;
    }

    if (key == nullptr && key_length != 0) {
        return false;
    }

    result = Assignment{
        node::INVALID,
        node::INVALID
    };

    const std::uint32_t key_hash =
        consistent_hash::fnv1a(
            key,
            key_length
        );

    std::size_t start_index = 0;

    if (
        !ring_.find_successor_index(
            key_hash,
            start_index
        )
    ) {
        return false;
    }

    /*
     * First live physical node clockwise
     * from the key position becomes primary.
     */
    for (
        std::size_t offset = 0;
        offset < ring_.point_count();
        ++offset
    ) {
        const std::size_t index =
            (start_index + offset) %
            ring_.point_count();

        const hash_ring::VirtualPoint& point =
            ring_.point(index);

        if (is_alive(point.owner)) {
            result.primary = point.owner;
            break;
        }
    }

    /*
     * No live node exists.
     */
    if (result.primary == node::INVALID) {
        return false;
    }

    /*
     * Continue clockwise until we find a live node
     * belonging to a different physical machine.
     */
    for (
        std::size_t offset = 1;
        offset <= ring_.point_count();
        ++offset
    ) {
        const std::size_t index =
            (start_index + offset) %
            ring_.point_count();

        const hash_ring::VirtualPoint& point =
            ring_.point(index);

        if (
            point.owner != result.primary &&
            is_alive(point.owner)
        ) {
            result.replica = point.owner;
            break;
        }
    }

    return true;
}

} // namespace placement

#pragma once

#include <cstddef>
#include <cstdint>

#include "hash_ring.h"
#include "membership.h"
#include "node_id.h"

namespace placement {

struct Assignment {
    node::Id primary;
    node::Id replica;
};

// Bit (id - node::MIN) set = node considered alive.
using AliveMask = std::uint8_t;

constexpr AliveMask mask_of(node::Id id)
{
    return static_cast<AliveMask>(1u << (id - node::MIN));
}

AliveMask alive_mask(
    const membership::Manager& membership,
    std::uint64_t now_ms
);

inline bool is_owner(const Assignment& assignment, node::Id id)
{
    return id != node::INVALID &&
           (assignment.primary == id || assignment.replica == id);
}

class Resolver {
public:
    bool begin();

    bool ready() const;

    bool locate(
        const std::uint8_t* key,
        std::size_t key_length,
        const membership::Manager& membership,
        std::uint64_t now_ms,
        Assignment& result
    ) const;

    // Same placement rule for an explicit set of live nodes.
    bool locate(
        const std::uint8_t* key,
        std::size_t key_length,
        AliveMask alive,
        Assignment& result
    ) const;

private:
    hash_ring::Ring ring_;
    bool initialized_ = false;
};

} // namespace placement

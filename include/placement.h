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

private:
    hash_ring::Ring ring_;
    bool initialized_ = false;
};

} // namespace placement

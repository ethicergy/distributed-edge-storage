#pragma once

#include <cstdint>

#include "node_id.h"

namespace request {

struct Identity {
    node::Id origin;
    std::uint32_t request_id;

    bool valid() const;

    bool operator==(const Identity& other) const;
    bool operator!=(const Identity& other) const;
};

class Generator {
public:
    Generator();

    bool begin(
        node::Id origin_node_id,
        std::uint32_t seed
    );

    Identity next();

    bool ready() const;

    node::Id origin_node_id() const;

private:
    node::Id origin_node_id_ = node::INVALID;
    std::uint32_t next_request_id_ = 1;
    bool initialized_ = false;
};

} // namespace request
#pragma once

#include <cstddef>
#include <cstdint>

#include "node_id.h"

namespace membership {

constexpr std::size_t NODE_COUNT =
    static_cast<std::size_t>(node::MAX - node::MIN + 1);

struct NeighborState {
    std::uint64_t last_heard_ms;
    bool seen;
};

class Manager {
public:
    explicit Manager(
        node::Id local_node_id,
        std::uint64_t timeout_ms
    );

    bool begin(std::uint64_t now_ms);

    bool heartbeat_received(
        node::Id sender,
        std::uint64_t now_ms
    );

    bool is_alive(
        node::Id id,
        std::uint64_t now_ms
    ) const;

    std::uint64_t last_heard(
        node::Id id
    ) const;

    bool seen(node::Id id) const;

    node::Id local_node_id() const;

    std::uint64_t timeout_ms() const;

    bool ready() const;

private:
    static bool valid_node(node::Id id);

    static std::size_t index(node::Id id);

    node::Id local_node_id_;
    std::uint64_t timeout_ms_;
    NeighborState neighbors_[NODE_COUNT]{};
    bool initialized_ = false;
};

} // namespace membership
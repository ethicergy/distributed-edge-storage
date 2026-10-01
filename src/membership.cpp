#include "membership.h"

namespace membership {

Manager::Manager(
    node::Id local_node_id,
    std::uint64_t timeout_ms
)
    : local_node_id_(local_node_id),
      timeout_ms_(timeout_ms)
{
}

bool Manager::valid_node(node::Id id)
{
    return id >= node::MIN &&
           id <= node::MAX;
}

std::size_t Manager::index(node::Id id)
{
    return static_cast<std::size_t>(
        id - node::MIN
    );
}

bool Manager::begin(std::uint64_t now_ms)
{
    if (!valid_node(local_node_id_)) {
        return false;
    }

    if (timeout_ms_ == 0) {
        return false;
    }

    for (std::size_t i = 0; i < NODE_COUNT; ++i) {
        neighbors_[i] = NeighborState{
            0,
            false
        };
    }

    /*
     * A node considers itself alive without requiring
     * a heartbeat from itself.
     */
    neighbors_[index(local_node_id_)] = NeighborState{
        now_ms,
        true
    };

    initialized_ = true;

    return true;
}

bool Manager::heartbeat_received(
    node::Id sender,
    std::uint64_t now_ms
)
{
    if (!initialized_ ||
        !valid_node(sender)) {

        return false;
    }

    /*
     * Heartbeats from ourselves are not meaningful
     * membership observations.
     */
    if (sender == local_node_id_) {
        return true;
    }

    const std::size_t i = index(sender);

    neighbors_[i].last_heard_ms = now_ms;
    neighbors_[i].seen = true;

    return true;
}

bool Manager::is_alive(
    node::Id id,
    std::uint64_t now_ms
) const
{
    if (!initialized_ ||
        !valid_node(id)) {

        return false;
    }

    if (id == local_node_id_) {
        return true;
    }

    const NeighborState& state =
        neighbors_[index(id)];

    if (!state.seen) {
        return false;
    }

    /*
     * Monotonic time means now_ms should never be
     * earlier than last_heard_ms.
     *
     * Treat an anomalous backwards clock reading
     * as not alive rather than underflowing.
     */
    if (now_ms < state.last_heard_ms) {
        return false;
    }

    const std::uint64_t elapsed =
        now_ms - state.last_heard_ms;

    return elapsed <= timeout_ms_;
}

std::uint64_t Manager::last_heard(
    node::Id id
) const
{
    if (!initialized_ ||
        !valid_node(id)) {

        return 0;
    }

    return neighbors_[index(id)].last_heard_ms;
}

bool Manager::seen(node::Id id) const
{
    if (!initialized_ ||
        !valid_node(id)) {

        return false;
    }

    return neighbors_[index(id)].seen;
}

node::Id Manager::local_node_id() const
{
    return local_node_id_;
}

std::uint64_t Manager::timeout_ms() const
{
    return timeout_ms_;
}

bool Manager::ready() const
{
    return initialized_;
}

} // namespace membership
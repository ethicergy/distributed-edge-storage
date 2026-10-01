#include "request_identity.h"

namespace request {

bool Identity::valid() const
{
    return origin != node::INVALID &&
           origin >= node::MIN &&
           origin <= node::MAX &&
           request_id != 0;
}

bool Identity::operator==(
    const Identity& other
) const
{
    return origin == other.origin &&
           request_id == other.request_id;
}

bool Identity::operator!=(
    const Identity& other
) const
{
    return !(*this == other);
}

Generator::Generator()
{
}

bool Generator::begin(
    node::Id origin_node_id,
    std::uint32_t seed
)
{
    if (origin_node_id < node::MIN ||
        origin_node_id > node::MAX) {

        return false;
    }

    origin_node_id_ = origin_node_id;

    /*
     * seed represents the last already-reserved ID.
     *
     * next() increments before returning.
     *
     * Zero is reserved and will never be returned.
     */
    next_request_id_ = seed;

    if (next_request_id_ == 0) {
        next_request_id_ = 0;
    }

    initialized_ = true;

    return true;
}

Identity Generator::next()
{
    if (!initialized_) {
        return Identity{
            node::INVALID,
            0
        };
    }

    ++next_request_id_;

    /*
     * request_id = 0 is reserved.
     * Wrap directly to 1.
     */
    if (next_request_id_ == 0) {
        next_request_id_ = 1;
    }

    return Identity{
        origin_node_id_,
        next_request_id_
    };
}

bool Generator::ready() const
{
    return initialized_;
}

node::Id Generator::origin_node_id() const
{
    return origin_node_id_;
}

} // namespace request
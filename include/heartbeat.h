#pragma once

#include <cstddef>
#include <cstdint>

#include "hlc.h"
#include "node_id.h"
#include "protocol.h"

namespace heartbeat {

protocol::Result build_packet(
    node::Id local_node_id,
    hlc::Hlc timestamp,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

} // namespace heartbeat
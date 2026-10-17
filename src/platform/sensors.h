#pragma once

#include "node_id.h"
#include "sensor.h"

namespace platform_sensors {

// The real sensor of node `id` (DHT11, AM312, LM393, BH1750), created
// on first call; nullptr for an unknown id. Wrap it in a
// sensor::FallbackSource.
sensor::Source* real_source(node::Id id);

} // namespace platform_sensors

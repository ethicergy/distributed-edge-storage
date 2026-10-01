#pragma once

#include <cstdint>

namespace platform_time {

/**
 * Monotonic elapsed time in milliseconds.
 *
 * This is intentionally not wall-clock time.
 */
std::uint64_t now_ms();

} // namespace platform_time
#include "platform_time.h"

#include <cstdint>

#include "esp_timer.h"

namespace platform_time {

std::uint64_t now_ms()
{
    const std::int64_t microseconds = esp_timer_get_time();

    if (microseconds <= 0) {
        return 0;
    }

    return static_cast<std::uint64_t>(
        microseconds / 1000
    );
}

} // namespace platform_time
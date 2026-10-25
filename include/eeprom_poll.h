#pragma once

#include <cstdint>

#include "eeprom.h"

namespace eeprom {

/*
 * Wait for the AT24C256's internal write cycle: the chip NACKs its
 * address until the page is written. Platform-free so the host tests
 * can drive it with a fake clock.
 *
 *   now_ms()  monotonic milliseconds
 *   ready()   one address poll; true when the chip ACKs
 *   pause()   give up the CPU for about 1 ms
 *
 * The chip gets one more poll after the deadline: the task can be off
 * the CPU for longer than the whole timeout (on node D the DHT11 read
 * runs with interrupts off), and a timeout must mean the chip did not
 * answer, not that we did not ask (BRINGUP_LOG problem 15).
 */
template <typename NowMs, typename Ready, typename Pause>
Result wait_for_write_cycle(NowMs now_ms, Ready ready, Pause pause,
                            std::uint32_t timeout_ms = WRITE_CYCLE_TIMEOUT_MS)
{
    const std::uint32_t start = now_ms();

    while (static_cast<std::uint32_t>(now_ms() - start) < timeout_ms) {
        if (ready()) {
            return Result::OK;
        }

        pause();
    }

    return ready() ? Result::OK : Result::WRITE_TIMEOUT;
}

} // namespace eeprom

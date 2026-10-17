#pragma once

#include <cstdint>

namespace hlc {

using Hlc = std::uint64_t;
using PhysicalNowFn = std::uint64_t (*)();

constexpr std::uint8_t LOGICAL_BITS = 16;
constexpr std::uint16_t LOGICAL_MAX = 0xFFFFu;
constexpr std::uint64_t PHYSICAL_MASK = (1ULL << 48) - 1ULL;

/*
 * No remote-drift rejection: the physical part is uptime based
 * (millis + offset), so legitimate clocks of different boards can
 * differ by hours. Rejecting "far ahead" clocks would permanently
 * split a cluster whose halves booted at different times.
 * See docs/DESIGN.md, "Clocks".
 */

constexpr Hlc make(
    std::uint64_t physical_ms,
    std::uint16_t logical_counter
) {
    return ((physical_ms & PHYSICAL_MASK) << LOGICAL_BITS) |
           static_cast<Hlc>(logical_counter);
}

constexpr std::uint64_t physical(Hlc value) {
    return (value >> LOGICAL_BITS) & PHYSICAL_MASK;
}

constexpr std::uint16_t logical(Hlc value) {
    return static_cast<std::uint16_t>(value & LOGICAL_MAX);
}

int compare(Hlc lhs, Hlc rhs);

class Clock {
public:
    explicit Clock(PhysicalNowFn physical_now);

    bool begin(Hlc persisted_hlc);

    Hlc now() const;
    Hlc on_local_event();
    Hlc on_receive(Hlc remote_hlc);

    /*
     * Pull the local physical clock forward to a remote clock that
     * is ahead of it (never backwards). After this, the physical
     * parts of all nodes track the most advanced clock in the
     * cluster, so versions created on different boards are ordered
     * by real time instead of by uptime. Call before on_receive().
     */
    void adopt_physical(Hlc remote_hlc);

    // Physical milliseconds of this clock right now (with offset).
    std::uint64_t physical_now() const;

    bool ready() const;

private:
    std::uint64_t physical_now_ms() const;
    Hlc wait_for_physical_advance(
        std::uint64_t previous_physical
    );

    PhysicalNowFn physical_now_;
    std::uint64_t physical_offset_ms_ = 0;
    Hlc value_ = 0;
    bool initialized_ = false;
};

} // namespace hlc
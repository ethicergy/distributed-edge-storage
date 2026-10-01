#include "hlc.h"

namespace hlc {

int compare(Hlc lhs, Hlc rhs)
{
    if (lhs < rhs) {
        return -1;
    }

    if (lhs > rhs) {
        return 1;
    }

    return 0;
}

Clock::Clock(PhysicalNowFn physical_now)
    : physical_now_(physical_now)
{
}

bool Clock::begin(Hlc persisted_hlc)
{
    if (physical_now_ == nullptr) {
        return false;
    }

    const std::uint64_t raw_now =
        physical_now_() & PHYSICAL_MASK;

    const std::uint64_t persisted_physical =
        physical(persisted_hlc);

    /*
     * The ESP32 monotonic timer restarts after reboot.
     *
     * If the persisted physical component is ahead of the
     * new boot's raw timer, create a runtime offset so that
     * the reconstructed physical clock starts at the
     * persisted value and continues from there.
     */
    if (raw_now <= persisted_physical) {
        physical_offset_ms_ =
            persisted_physical - raw_now;

        value_ = persisted_hlc;
    } else {
        /*
         * The new boot's monotonic time is already ahead of
         * the persisted physical component.
         */
        physical_offset_ms_ = 0;
        value_ = make(raw_now, 0);
    }

    initialized_ = true;
    return true;
}

std::uint64_t Clock::physical_now_ms() const
{
    const std::uint64_t raw_now =
        physical_now_() & PHYSICAL_MASK;

    if (raw_now > PHYSICAL_MASK - physical_offset_ms_) {
        return PHYSICAL_MASK;
    }

    return raw_now + physical_offset_ms_;
}

Hlc Clock::now() const
{
    if (!initialized_) {
        return 0;
    }

    const std::uint64_t current_physical =
        physical_now_ms();

    const std::uint64_t stored_physical =
        physical(value_);

    /*
     * now() is a read-only snapshot.
     * It does not itself count as a local event.
     */
    if (current_physical > stored_physical) {
        return make(current_physical, 0);
    }

    return value_;
}

Hlc Clock::wait_for_physical_advance(
    std::uint64_t previous_physical
)
{
    // Logical counter overflowed: don't block, just move
    // physical forward by 1 ms and reset the counter.
    value_ = make(previous_physical + 1, 0);
    return value_;
}

Hlc Clock::on_local_event()
{
    if (!initialized_) {
        return 0;
    }

    const std::uint64_t previous_physical =
        physical(value_);

    const std::uint16_t previous_logical =
        logical(value_);

    const std::uint64_t current_physical =
        physical_now_ms();


    const std::uint64_t next_physical =
        current_physical > previous_physical
            ? current_physical
            : previous_physical;

    /*
     * Physical time advanced.
     */
    if (next_physical > previous_physical) {
        value_ = make(next_physical, 0);
        return value_;
    }

    /*
     * Same physical millisecond:
     * increment logical component.
     */
    if (previous_logical == LOGICAL_MAX) {
        return wait_for_physical_advance(
            previous_physical
        );
    }

    value_ = make(
        previous_physical,
        static_cast<std::uint16_t>(
            previous_logical + 1u
        )
    );

    return value_;
}

Hlc Clock::on_receive(Hlc remote_hlc)
{
    if (!initialized_) {
        return 0;
    }

    const std::uint64_t local_physical =
        physical(value_);

    const std::uint16_t local_logical =
        logical(value_);

    const std::uint64_t remote_physical =
        physical(remote_hlc);

    const std::uint16_t remote_logical =
        logical(remote_hlc);

    const std::uint64_t current_physical =
        physical_now_ms();

    

if (remote_physical > current_physical &&
    remote_physical - current_physical >
        MAX_REMOTE_DRIFT_MS) {

    return 0;
}
    /*
     * p = max(local physical, remote physical, physical_now)
     */
    std::uint64_t next_physical = local_physical;

    if (remote_physical > next_physical) {
        next_physical = remote_physical;
    }

    if (current_physical > next_physical) {
        next_physical = current_physical;
    }

    std::uint16_t next_logical = 0;

    /*
     * p == local == remote
     */
    if (next_physical == local_physical &&
        next_physical == remote_physical) {

        const std::uint16_t highest_logical =
            local_logical > remote_logical
                ? local_logical
                : remote_logical;

        if (highest_logical == LOGICAL_MAX) {
            return wait_for_physical_advance(
                next_physical
            );
        }

        next_logical = static_cast<std::uint16_t>(
            highest_logical + 1u
        );

    /*
     * p == local
     */
    } else if (next_physical == local_physical) {

        if (local_logical == LOGICAL_MAX) {
            return wait_for_physical_advance(
                next_physical
            );
        }

        next_logical = static_cast<std::uint16_t>(
            local_logical + 1u
        );

    /*
     * p == remote
     */
    } else if (next_physical == remote_physical) {

        if (remote_logical == LOGICAL_MAX) {
            return wait_for_physical_advance(
                next_physical
            );
        }

        next_logical = static_cast<std::uint16_t>(
            remote_logical + 1u
        );

    /*
     * p == physical_now and is greater than both.
     */
    } else {
        next_logical = 0;
    }

    value_ = make(
        next_physical,
        next_logical
    );

    return value_;
}

bool Clock::ready() const
{
    return initialized_;
}

} // namespace hlc
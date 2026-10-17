#include "sensor_sources.h"

namespace sensor {

// ------------------------------------------------------------------
// MotionSource
// ------------------------------------------------------------------

MotionSource::MotionSource(DigitalInput& input, std::uint32_t settle_ms, std::uint32_t poll_interval_ms)
    : input_(input), settle_ms_(settle_ms), poll_interval_ms_(poll_interval_ms)
{
}

bool MotionSource::settling(std::uint64_t now_ms) const
{
    return !started_ || now_ms - start_ms_ < settle_ms_;
}

void MotionSource::poll(std::uint64_t now_ms)
{
    if (!started_) {
        started_ = true;
        start_ms_ = now_ms;
        next_poll_ms_ = now_ms;
    }

    if (now_ms < next_poll_ms_) {
        return;
    }

    next_poll_ms_ = now_ms + poll_interval_ms_;

    const bool high = input_.level();

    if (settling(now_ms)) {
        // The PIR may toggle while it settles; that is not motion.
        high_ = high;
        return;
    }

    if (high && !high_) {
        ++detections_;
    }

    if (high) {
        have_motion_ = true;
        last_motion_ms_ = now_ms;
    }

    high_ = high;
}

bool MotionSource::read(std::uint64_t now_ms, Sample& sample)
{
    poll(now_ms);

    sample = Sample{};

    if (settling(now_ms)) {
        sample.flags = READING_FLAG_SETTLING;
        detections_ = 0;
        return true;
    }

    sample.value_milli = static_cast<std::int32_t>(detections_ * 1000u);
    sample.aux = have_motion_
        ? static_cast<std::int32_t>((now_ms - last_motion_ms_) / 1000u)
        : AUX_NONE;

    detections_ = 0;
    return true;
}

// ------------------------------------------------------------------
// SoundSource
// ------------------------------------------------------------------

SoundSource::SoundSource(DigitalInput& input, bool active_high, std::uint32_t gap_ms)
    : input_(input), active_high_(active_high), gap_ms_(gap_ms)
{
}

bool SoundSource::begin()
{
    if (!input_.begin()) {
        return false;
    }

    edges_seen_ = input_.edges();
    return true;
}

void SoundSource::poll(std::uint64_t now_ms)
{
    const std::uint32_t edges = input_.edges();
    const std::uint32_t new_edges = edges - edges_seen_; // wraps correctly
    edges_seen_ = edges;

    if (new_edges > 0) {
        total_edges_ += new_edges;

        if (!have_edge_ || now_ms - last_edge_ms_ >= gap_ms_) {
            ++events_;
        }

        have_edge_ = true;
        last_edge_ms_ = now_ms;
    }

    const bool active = input_.level() == active_high_;

    if (!started_) {
        started_ = true;
        window_start_ms_ = now_ms;
    } else if (active_) {
        active_ms_ += now_ms - last_poll_ms_;
    }

    active_ = active;
    last_poll_ms_ = now_ms;
}

bool SoundSource::read(std::uint64_t now_ms, Sample& sample)
{
    poll(now_ms);

    sample = Sample{};

    const std::uint64_t window_ms = now_ms - window_start_ms_;
    const std::uint64_t active_ms = active_ms_ < window_ms ? active_ms_ : window_ms;

    sample.value_milli = static_cast<std::int32_t>(events_ * 1000u);
    sample.aux = window_ms == 0
        ? 0
        : static_cast<std::int32_t>(active_ms * 100000u / window_ms);

    events_ = 0;
    active_ms_ = 0;
    window_start_ms_ = now_ms;
    return true;
}

// ------------------------------------------------------------------
// LatestValueSource
// ------------------------------------------------------------------

LatestValueSource::LatestValueSource(Kind kind, ValueInput& input)
    : kind_(kind), input_(input)
{
}

bool LatestValueSource::read(std::uint64_t now_ms, Sample& sample)
{
    (void)now_ms;

    const ValueAttempts attempts = input_.take();
    failures_ += attempts.failed;

    if (attempts.ok > 0) {
        have_value_ = true;
        last_good_milli_ = attempts.last_good_milli;
    }

    if (!have_value_) {
        return false;
    }

    sample = Sample{};
    sample.value_milli = last_good_milli_;
    return true;
}

// ------------------------------------------------------------------
// FallbackSource
// ------------------------------------------------------------------

FallbackSource::FallbackSource(Source* real, Source& simulated, std::uint8_t fallback_after)
    : real_(real), simulated_(simulated), fallback_after_(fallback_after)
{
}

bool FallbackSource::begin()
{
    simulated_.begin();

    real_ok_ = real_ != nullptr &&
               real_->kind() == simulated_.kind() &&
               real_->begin();

    using_simulated_ = !real_ok_;
    return true;
}

void FallbackSource::poll(std::uint64_t now_ms)
{
    if (real_ok_) {
        real_->poll(now_ms);
    }

    simulated_.poll(now_ms);
}

bool FallbackSource::read(std::uint64_t now_ms, Sample& sample)
{
    if (!real_ok_) {
        using_simulated_ = true;
        return simulated_.read(now_ms, sample);
    }

    if (real_->read(now_ms, sample)) {
        consecutive_failures_ = 0;
        using_simulated_ = false;
        return true;
    }

    ++failures_;

    if (consecutive_failures_ < fallback_after_) {
        ++consecutive_failures_;
    }

    if (consecutive_failures_ < fallback_after_) {
        return false;
    }

    using_simulated_ = true;
    return simulated_.read(now_ms, sample);
}

std::uint32_t FallbackSource::failures() const
{
    return failures_ + (real_ != nullptr ? real_->failures() : 0);
}

} // namespace sensor

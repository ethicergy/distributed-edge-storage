#include "sensor_sources.h"

#include <cstdio>

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
// LM393 AO: block summary, detector, SoundSource
// ------------------------------------------------------------------

SoundBlock summarize_sound_block(
    const std::uint16_t* samples_mv,
    std::size_t count,
    std::uint16_t clip_low_mv,
    std::uint16_t clip_high_mv,
    std::uint64_t end_ms
)
{
    SoundBlock block{};
    block.end_ms = end_ms;

    if (count == 0) {
        return block;
    }

    std::uint16_t lo = samples_mv[0];
    std::uint16_t hi = samples_mv[0];
    std::uint32_t sum = 0;

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint16_t mv = samples_mv[i];
        lo = mv < lo ? mv : lo;
        hi = mv > hi ? mv : hi;
        sum += mv;
    }

    block.min_mv = lo;
    block.max_mv = hi;
    block.mean_mv = static_cast<std::uint16_t>((sum + count / 2) / count);
    block.clipped = lo <= clip_low_mv || hi >= clip_high_mv;
    return block;
}

SoundDetector::SoundDetector(std::uint16_t threshold_mv, std::uint32_t gap_ms)
    : threshold_mv_(threshold_mv), gap_ms_(gap_ms)
{
}

void SoundDetector::add(const SoundBlock& block)
{
    const std::uint16_t swing = swing_mv(block);
    const bool active = swing > threshold_mv_;

    if (active && !active_) {
        if (!have_crossing_ || block.end_ms - last_crossing_ms_ >= gap_ms_) {
            ++window_.events;
        }

        have_crossing_ = true;
        last_crossing_ms_ = block.end_ms;
    }

    active_ = active;

    ++window_.blocks;
    window_.active_blocks += active ? 1u : 0u;
    window_.clipped_blocks += block.clipped ? 1u : 0u;
    window_.max_swing_mv = swing > window_.max_swing_mv ? swing : window_.max_swing_mv;
    mean_sum_ += block.mean_mv;
}

SoundWindow SoundDetector::take()
{
    SoundWindow out = window_;

    if (out.blocks > 0) {
        out.mean_mv = static_cast<std::uint16_t>((mean_sum_ + out.blocks / 2) / out.blocks);
    }

    window_ = SoundWindow{};
    mean_sum_ = 0;
    return out;
}

SoundSource::SoundSource(SoundBlockInput& input, std::uint16_t threshold_mv, std::uint32_t gap_ms, std::uint32_t block_ms)
    : input_(input), detector_(threshold_mv, gap_ms), threshold_mv_(threshold_mv), block_ms_(block_ms)
{
}

void SoundSource::poll(std::uint64_t now_ms)
{
    if (!started_) {
        started_ = true;
        window_start_ms_ = now_ms;
    }

    SoundBlock blocks[16];
    std::size_t n = 0;

    while ((n = input_.take(blocks, sizeof(blocks) / sizeof(blocks[0]))) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            detector_.add(blocks[i]);
        }
    }
}

bool SoundSource::read(std::uint64_t now_ms, Sample& sample)
{
    poll(now_ms);

    const SoundWindow window = detector_.take();
    const std::uint64_t window_ms = now_ms - window_start_ms_;

    last_window_ = window;
    last_window_ms_ = window_ms;
    window_start_ms_ = now_ms;

    sample = Sample{};

    if (window.blocks == 0) {
        ++failures_;
        return false;
    }

    const std::uint64_t active_ms = static_cast<std::uint64_t>(window.active_blocks) * block_ms_;

    sample.value_milli = static_cast<std::int32_t>(window.events * 1000u);
    sample.aux = window_ms == 0
        ? 0
        : static_cast<std::int32_t>((active_ms < window_ms ? active_ms : window_ms) * 100000u / window_ms);
    return true;
}

std::size_t SoundSource::describe(char* out, std::size_t capacity) const
{
    if (capacity == 0) {
        return 0;
    }

    out[0] = '\0';

    if (last_window_ms_ == 0) {
        return 0;
    }

    const SoundWindow& w = last_window_;
    const std::uint64_t sampled_pct =
        static_cast<std::uint64_t>(w.blocks) * block_ms_ * 100u / last_window_ms_;

    const int n = std::snprintf(
        out,
        capacity,
        "AO rest %u mV, swing max %u mV (threshold %u mV), %lu/%lu blocks active, "
        "%lu clipped, sampled %lu %%, %lu dropped, %lu us/sample",
        static_cast<unsigned>(w.mean_mv),
        static_cast<unsigned>(w.max_swing_mv),
        static_cast<unsigned>(threshold_mv_),
        static_cast<unsigned long>(w.active_blocks),
        static_cast<unsigned long>(w.blocks),
        static_cast<unsigned long>(w.clipped_blocks),
        static_cast<unsigned long>(sampled_pct),
        static_cast<unsigned long>(input_.dropped()),
        static_cast<unsigned long>(input_.sample_cost_us())
    );

    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }

    return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1;
}

// ------------------------------------------------------------------
// LatestValueSource
// ------------------------------------------------------------------

LatestValueSource::LatestValueSource(Kind kind, ValueInput& input, std::uint32_t stale_ms)
    : kind_(kind), input_(input), stale_ms_(stale_ms)
{
}

void LatestValueSource::poll(std::uint64_t now_ms)
{
    if (!started_) {
        started_ = true;
        start_ms_ = now_ms;
    }
}

bool LatestValueSource::read(std::uint64_t now_ms, Sample& sample)
{
    poll(now_ms);

    const ValueAttempts attempts = input_.take();
    failures_ += attempts.failed;

    if (attempts.ok > 0) {
        have_value_ = true;
        last_good_milli_ = attempts.last_good_milli;
        last_good_ms_ = attempts.last_good_ms;
    }

    sample = Sample{};

    if (have_value_) {
        sample.value_milli = last_good_milli_;

        if (now_ms > last_good_ms_ && now_ms - last_good_ms_ > stale_ms_) {
            sample.flags = READING_FLAG_STALE;
        }

        return true;
    }

    if (now_ms - start_ms_ <= stale_ms_) {
        return false; // no read yet; not an error so soon after boot
    }

    sample.flags = READING_FLAG_ERROR;
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

    // A source that must never be replaced (DHT11) stays in charge even
    // if begin() failed; it then reports errors itself.
    real_ok_ = real_ != nullptr &&
               real_->kind() == simulated_.kind() &&
               (real_->begin() || !real_->fallback_allowed());

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

    if (!real_->fallback_allowed()) {
        return false;
    }

    if (consecutive_failures_ < fallback_after_) {
        ++consecutive_failures_;
    }

    if (consecutive_failures_ < fallback_after_) {
        return false;
    }

    using_simulated_ = true;
    return simulated_.read(now_ms, sample);
}

std::size_t FallbackSource::describe(char* out, std::size_t capacity) const
{
    if (real_ok_) {
        return real_->describe(out, capacity);
    }

    if (capacity > 0) {
        out[0] = '\0';
    }

    return 0;
}

std::uint32_t FallbackSource::failures() const
{
    return failures_ + (real_ != nullptr ? real_->failures() : 0);
}

} // namespace sensor

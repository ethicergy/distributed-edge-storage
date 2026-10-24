#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "sensor.h"

/*
 * Platform-independent logic of the real sensors (D1). The hardware
 * access is behind three small interfaces, implemented by the drivers
 * in src/platform/sensors.cpp and by fakes in the host tests:
 *
 *   DigitalInput     a GPIO level (AM312)
 *   SoundBlockInput  ADC sample blocks from another task (LM393 AO)
 *   ValueInput       attempts of a sensor read in another task (DHT11)
 *
 * and a Source for the BH1750 (plain synchronous I2C read).
 */
namespace sensor {

class DigitalInput {
public:
    virtual ~DigitalInput() = default;

    virtual bool begin() = 0;

    // Raw pin level, true = high.
    virtual bool level() = 0;
};

/*
 * AM312 PIR (node B). OUT goes high for ~2 s per detection; polled
 * every config::PIR_POLL_INTERVAL_MS. Value: detections (rising edges)
 * per window. Aux: seconds from the end of the last high period seen
 * to the sample (AUX_NONE if none). For config::PIR_SETTLE_MS after the
 * first poll (about power-on) every reading is flagged settling with
 * value 0, and input is ignored.
 */
class MotionSource final : public Source {
public:
    explicit MotionSource(
        DigitalInput& input,
        std::uint32_t settle_ms = config::PIR_SETTLE_MS,
        std::uint32_t poll_interval_ms = config::PIR_POLL_INTERVAL_MS
    );

    bool begin() override { return input_.begin(); }
    Kind kind() const override { return Kind::MOTION; }
    bool simulated() const override { return false; }
    void poll(std::uint64_t now_ms) override;
    bool read(std::uint64_t now_ms, Sample& sample) override;

    bool settling(std::uint64_t now_ms) const;

private:
    DigitalInput& input_;
    std::uint32_t settle_ms_;
    std::uint32_t poll_interval_ms_;

    bool started_ = false;
    std::uint64_t start_ms_ = 0;
    std::uint64_t next_poll_ms_ = 0;
    bool high_ = false;
    std::uint32_t detections_ = 0;
    bool have_motion_ = false;
    std::uint64_t last_motion_ms_ = 0;
};

/*
 * LM393 sound module, AO only (node C). A task samples AO on an ADC1
 * pin every config::SOUND_SAMPLE_INTERVAL_MS and summarises each
 * config::SOUND_BLOCK_SAMPLES samples into a SoundBlock. The logic
 * below is pure (host tests feed it synthetic sample arrays):
 *
 *   summarize_sound_block  min / max / mean of one block, and whether a
 *                          sample sat at an ADC limit (clipping)
 *   SoundDetector          a block is active when its peak-to-peak
 *                          swing exceeds the threshold; events are
 *                          rising crossings (inactive -> active), and
 *                          crossings less than gap_ms apart are one event
 *
 * The reading keeps the DO-era format: value = events per window,
 * aux = milli-percent of the window active.
 */
struct SoundBlock {
    std::uint16_t min_mv = 0;
    std::uint16_t max_mv = 0;
    std::uint16_t mean_mv = 0;
    bool clipped = false;     // a sample at clip_low_mv or clip_high_mv
    std::uint64_t end_ms = 0; // node clock at the last sample
};

// clip_low_mv / clip_high_mv: what raw 0 and full scale convert to.
SoundBlock summarize_sound_block(
    const std::uint16_t* samples_mv,
    std::size_t count,
    std::uint16_t clip_low_mv,
    std::uint16_t clip_high_mv,
    std::uint64_t end_ms
);

constexpr std::uint16_t swing_mv(const SoundBlock& block)
{
    return static_cast<std::uint16_t>(block.max_mv - block.min_mv);
}

// One reading window of blocks.
struct SoundWindow {
    std::uint32_t events = 0;
    std::uint32_t blocks = 0;
    std::uint32_t active_blocks = 0;
    std::uint32_t clipped_blocks = 0;
    std::uint16_t max_swing_mv = 0;
    std::uint16_t mean_mv = 0; // mean of the block means (resting level)
};

class SoundDetector {
public:
    SoundDetector(std::uint16_t threshold_mv, std::uint32_t gap_ms);

    void add(const SoundBlock& block);

    // The window so far; starts a new one. Crossing state carries over.
    SoundWindow take();

private:
    std::uint16_t threshold_mv_;
    std::uint32_t gap_ms_;
    SoundWindow window_{};
    std::uint64_t mean_sum_ = 0;
    bool active_ = false;
    bool have_crossing_ = false;
    std::uint64_t last_crossing_ms_ = 0;
};

// Blocks completed by the sampling task, handed to the node task.
class SoundBlockInput {
public:
    virtual ~SoundBlockInput() = default;

    virtual bool begin() = 0;

    // Moves up to `capacity` completed blocks into `out`, oldest first.
    virtual std::size_t take(SoundBlock* out, std::size_t capacity) = 0;

    // Running count of blocks lost because the buffer was full.
    virtual std::uint32_t dropped() const { return 0; }

    // Mean time of one ADC sample in microseconds (0 if unknown).
    virtual std::uint32_t sample_cost_us() const { return 0; }
};

/*
 * value: events per window; aux: milli-percent of the window active
 * (active blocks x block_ms). A window with no block at all (sampling
 * task stalled or dead) is a failed read, so the fallback shows it.
 * describe(): the last window's level, swing and sampling figures.
 */
class SoundSource final : public Source {
public:
    explicit SoundSource(
        SoundBlockInput& input,
        std::uint16_t threshold_mv = config::SOUND_ACTIVE_SWING_MV,
        std::uint32_t gap_ms = config::SOUND_EVENT_GAP_MS,
        std::uint32_t block_ms = config::SOUND_BLOCK_MS
    );

    bool begin() override { return input_.begin(); }
    Kind kind() const override { return Kind::SOUND; }
    bool simulated() const override { return false; }
    void poll(std::uint64_t now_ms) override;
    bool read(std::uint64_t now_ms, Sample& sample) override;
    std::uint32_t failures() const override { return failures_; }
    std::size_t describe(char* out, std::size_t capacity) const override;

    const SoundWindow& last_window() const { return last_window_; }

private:
    SoundBlockInput& input_;
    SoundDetector detector_;
    std::uint16_t threshold_mv_;
    std::uint32_t block_ms_;

    bool started_ = false;
    std::uint64_t window_start_ms_ = 0;
    SoundWindow last_window_{};
    std::uint64_t last_window_ms_ = 0;
    std::uint32_t failures_ = 0;
};

// Results of read attempts made elsewhere (the DHT11 task) since the
// last take().
struct ValueAttempts {
    std::uint32_t ok = 0;
    std::uint32_t failed = 0;
    std::int32_t last_good_milli = 0; // valid when ok > 0
    std::uint64_t last_good_ms = 0;   // when it was read (node clock), valid when ok > 0
};

class ValueInput {
public:
    virtual ~ValueInput() = default;

    virtual bool begin() = 0;
    virtual ValueAttempts take() = 0;
};

/*
 * A level sensor read outside the node task (DHT11). Failed attempts
 * are counted and keep the last good value, for stale_ms. After that,
 * readings carry READING_FLAG_STALE (still the last good value), or
 * READING_FLAG_ERROR with value 0 if there was no good read since the
 * first poll. read() fails (no reading) only during the first stale_ms
 * while no value exists yet. Never replaced by simulated values
 * (fallback_allowed() is false).
 */
class LatestValueSource final : public Source {
public:
    LatestValueSource(Kind kind, ValueInput& input, std::uint32_t stale_ms = config::DHT_STALE_MS);

    bool begin() override { return input_.begin(); }
    Kind kind() const override { return kind_; }
    bool simulated() const override { return false; }
    void poll(std::uint64_t now_ms) override;
    bool read(std::uint64_t now_ms, Sample& sample) override;
    std::uint32_t failures() const override { return failures_; }
    bool fallback_allowed() const override { return false; }

private:
    Kind kind_;
    ValueInput& input_;
    std::uint32_t stale_ms_;
    bool started_ = false;
    std::uint64_t start_ms_ = 0;
    bool have_value_ = false;
    std::int32_t last_good_milli_ = 0;
    std::uint64_t last_good_ms_ = 0;
    std::uint32_t failures_ = 0;
};

/*
 * The per-node switch with a safety net: uses `real` when given and
 * its begin() succeeded; after config::SENSOR_FALLBACK_AFTER_FAILURES
 * failed reads in a row it serves the simulated source (flagged
 * simulated) until the real one reads again, unless the real source
 * forbids that (fallback_allowed(): the DHT11 reports stale / error
 * instead). real == nullptr: always simulated.
 */
class FallbackSource final : public Source {
public:
    FallbackSource(
        Source* real,
        Source& simulated,
        std::uint8_t fallback_after = config::SENSOR_FALLBACK_AFTER_FAILURES
    );

    bool begin() override;
    Kind kind() const override { return simulated_.kind(); }
    bool simulated() const override { return using_simulated_; }
    void poll(std::uint64_t now_ms) override;
    bool read(std::uint64_t now_ms, Sample& sample) override;
    std::uint32_t failures() const override;
    std::size_t describe(char* out, std::size_t capacity) const override;

    // Real sensor present and initialised.
    bool real_enabled() const { return real_ok_; }

private:
    Source* real_;
    Source& simulated_;
    std::uint8_t fallback_after_;
    bool real_ok_ = false;
    bool using_simulated_ = true;
    std::uint8_t consecutive_failures_ = 0;
    std::uint32_t failures_ = 0;
};

// Whether config::REAL_SENSOR_NODES enables node `id`'s real sensor.
constexpr bool real_sensor_enabled(node::Id id, std::uint8_t mask = config::REAL_SENSOR_NODES)
{
    return id >= node::MIN && id <= node::MAX && ((mask >> (id - node::MIN)) & 1u) != 0;
}

} // namespace sensor

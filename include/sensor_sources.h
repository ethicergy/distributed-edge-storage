#pragma once

#include <cstdint>

#include "config.h"
#include "sensor.h"

/*
 * Platform-independent logic of the real sensors (D1). The hardware
 * access is behind three small interfaces, implemented by the drivers
 * in src/platform/sensors.cpp and by fakes in the host tests:
 *
 *   DigitalInput  a GPIO level, optionally with an ISR edge counter
 *   ValueInput    attempts of a sensor read in another task (DHT11)
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

    // Running count of edges seen by the ISR (wraps). Only written by
    // the ISR; readers take differences.
    virtual std::uint32_t edges() { return 0; }
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
 * LM393 sound module, DO only (node C). The ISR counts edges into the
 * active level; edges less than gap_ms apart are one event. Value:
 * events per window. Aux: milli-percent of the window DO was active,
 * from the level sampled on every poll (each node-task iteration).
 */
class SoundSource final : public Source {
public:
    explicit SoundSource(
        DigitalInput& input,
        bool active_high = config::SOUND_ACTIVE_HIGH,
        std::uint32_t gap_ms = config::SOUND_EVENT_GAP_MS
    );

    bool begin() override;
    Kind kind() const override { return Kind::SOUND; }
    bool simulated() const override { return false; }
    void poll(std::uint64_t now_ms) override;
    bool read(std::uint64_t now_ms, Sample& sample) override;

    std::uint32_t total_edges() const { return total_edges_; }

private:
    DigitalInput& input_;
    bool active_high_;
    std::uint32_t gap_ms_;

    std::uint32_t edges_seen_ = 0;
    std::uint32_t total_edges_ = 0;
    std::uint32_t events_ = 0;
    bool have_edge_ = false;
    std::uint64_t last_edge_ms_ = 0;

    bool started_ = false;
    std::uint64_t window_start_ms_ = 0;
    std::uint64_t last_poll_ms_ = 0;
    bool active_ = false;
    std::uint64_t active_ms_ = 0;
};

// Results of read attempts made elsewhere (the DHT11 task) since the
// last take().
struct ValueAttempts {
    std::uint32_t ok = 0;
    std::uint32_t failed = 0;
    std::int32_t last_good_milli = 0; // valid when ok > 0
};

class ValueInput {
public:
    virtual ~ValueInput() = default;

    virtual bool begin() = 0;
    virtual ValueAttempts take() = 0;
};

/*
 * A level sensor read outside the node task (DHT11, node A). Keeps the
 * last good value through failed attempts and counts them; read()
 * fails only until the first good value.
 */
class LatestValueSource final : public Source {
public:
    LatestValueSource(Kind kind, ValueInput& input);

    bool begin() override { return input_.begin(); }
    Kind kind() const override { return kind_; }
    bool simulated() const override { return false; }
    bool read(std::uint64_t now_ms, Sample& sample) override;
    std::uint32_t failures() const override { return failures_; }

private:
    Kind kind_;
    ValueInput& input_;
    bool have_value_ = false;
    std::int32_t last_good_milli_ = 0;
    std::uint32_t failures_ = 0;
};

/*
 * The per-node switch with a safety net: uses `real` when given and
 * its begin() succeeded; after config::SENSOR_FALLBACK_AFTER_FAILURES
 * failed reads in a row it serves the simulated source (flagged
 * simulated) until the real one reads again. real == nullptr: always
 * simulated.
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

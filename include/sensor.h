#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "node_id.h"
#include "node_runtime.h"

/*
 * Sensor acquisition path (work plan P7, decisions D1/D3/D4).
 *
 * Every node runs a SimulatedSource unless config::REAL_SENSOR_NODES
 * enables its real sensor (sensor_sources.h, behind a FallbackSource
 * that reverts to the simulated one if the hardware fails). A real driver only has to implement
 * sensor::Source; nothing else changes.
 *
 * Data model (D3, option a + b):
 *   latest value  : "s/<node>/<kind>"            e.g. "s/A/temp"
 *   history ring  : "h/<node>/<kind>/<seq % N>"  e.g. "h/A/temp/2"
 * N = config::SENSOR_HISTORY_SLOTS. Keys per node: 1 + N, so the
 * whole cluster uses 4 * (1 + N) keys, far below the key budget.
 *
 * Every sample is written with the normal distributed SET (rule R6):
 * placement, replication, versioning and retries all apply.
 */
namespace sensor {

enum class Kind : std::uint8_t {
    TEMPERATURE = 1, // milli-degC
    HUMIDITY = 2,    // milli-%RH
    PRESSURE = 3,    // milli-hPa
    LIGHT = 4,       // milli-lux
    MOTION = 5,      // milli-events per window; aux: s since last motion
    SOUND = 6        // milli-events per window; aux: milli-% of window active
};

const char* kind_key(Kind kind);   // "temp", "humidity", ...
const char* kind_unit(Kind kind);  // "degC", "%RH", "hPa", "lux", "events"

// Event kinds count occurrences per sampling window and carry an aux
// value; level kinds report one measurement.
bool has_aux(Kind kind);

// Node -> sensor (D1): A DHT11, B AM312 PIR, C LM393 sound, D BH1750.
Kind kind_for_node(node::Id id);

/*
 * Reading wire format (big-endian like the protocol):
 *
 *   0      format version: 0x01 (level kinds, 20 bytes) or
 *          0x02 (event kinds, 24 bytes)
 *   1      kind
 *   2      flags (bit 0: simulated source, bit 1: sensor settling)
 *   3      reserved (0)
 *   4..7   sequence number (u32, per node, survives reboots)
 *   8..11  value in thousandths of the unit (i32)
 *   12..19 HLC of the sample (u64; physical part = cluster ms, see
 *          docs/DESIGN.md "Clocks" for wall-clock conversion)
 *   20..23 v2 only: aux (i32)
 *            MOTION: seconds from the last detection to the sample,
 *                    AUX_NONE if none since boot
 *            SOUND:  milli-percent of the window the output was active
 */
constexpr std::uint8_t READING_FORMAT = 0x01;
constexpr std::uint8_t READING_FORMAT_AUX = 0x02;
constexpr std::size_t READING_SIZE = 20;
constexpr std::size_t READING_SIZE_AUX = 24;
constexpr std::size_t READING_MAX_SIZE = READING_SIZE_AUX;
constexpr std::uint8_t READING_FLAG_SIMULATED = 0x01;
// MOTION: the PIR is still in its post-power-on settle time; value 0.
constexpr std::uint8_t READING_FLAG_SETTLING = 0x02;
constexpr std::int32_t AUX_NONE = -1;

struct Reading {
    Kind kind;
    std::uint8_t flags;
    std::uint32_t sequence;
    std::int32_t value_milli;
    std::uint64_t hlc;
    std::int32_t aux = 0; // event kinds only
};

// Returns the encoded length (READING_SIZE or READING_SIZE_AUX), 0 on error.
std::size_t encode(const Reading& reading, std::uint8_t (&out)[READING_MAX_SIZE]);
bool decode(const std::uint8_t* data, std::size_t length, Reading& reading);

// Human-readable value: "23.202 degC", "2 events, last motion 12 s
// before the sample", "settling", "5 events, 37.5 % active".
std::size_t format_value(
    Kind kind,
    std::int32_t value_milli,
    std::int32_t aux,
    std::uint8_t flags,
    char* out,
    std::size_t capacity
);

// "s/A/temp"
std::size_t latest_key(node::Id id, Kind kind, char* out, std::size_t capacity);

// "h/A/temp/3"
std::size_t history_key(node::Id id, Kind kind, std::uint32_t sequence, char* out, std::size_t capacity);

// One sampling window's result.
struct Sample {
    std::int32_t value_milli = 0; // thousandths of the unit
    std::int32_t aux = 0;         // event kinds, see the wire format
    std::uint8_t flags = 0;       // READING_FLAG_SETTLING; SIMULATED comes from simulated()
};

class Source {
public:
    virtual ~Source() = default;

    virtual bool begin() = 0;
    virtual Kind kind() const = 0;
    virtual bool simulated() const = 0;

    // Called on every node-task iteration, also while the sampler is
    // off or the node is not ACTIVE, so event sources never miss input.
    virtual void poll(std::uint64_t now_ms) { (void)now_ms; }

    // The window since the previous read. False: no value this time.
    virtual bool read(std::uint64_t now_ms, Sample& sample) = 0;

    // Failed hardware reads so far, including ones hidden by keeping
    // the last good value (shown by `status`).
    virtual std::uint32_t failures() const { return 0; }
};

/*
 * Deterministic synthetic signal. Level kinds: a slow daily-like wave,
 * a faster ripple and bounded noise around a realistic base value.
 * Event kinds: a few random events per window, more during the "day".
 * Also the per-node fallback when a real sensor is off or broken.
 */
class SimulatedSource final : public Source {
public:
    SimulatedSource(Kind kind, std::uint32_t seed);

    bool begin() override;
    Kind kind() const override { return kind_; }
    bool simulated() const override { return true; }
    bool read(std::uint64_t now_ms, Sample& sample) override;

private:
    std::uint32_t next_random();
    void read_level(std::uint64_t now_ms, Sample& sample);
    void read_events(std::uint64_t now_ms, Sample& sample);

    Kind kind_;
    std::uint32_t state_;
    std::int32_t noise_ = 0;

    // Event kinds.
    bool have_window_ = false;
    std::uint64_t window_start_ms_ = 0;
    bool have_event_ = false;
    std::uint64_t last_event_ms_ = 0;
};

struct SamplerStats {
    std::uint32_t samples;
    std::uint32_t writes_ok;
    std::uint32_t writes_under_replicated;
    std::uint32_t writes_failed;
    std::uint32_t skipped_busy;
    std::uint32_t read_errors;
    std::int32_t last_value_milli;
    std::int32_t last_aux;
    std::uint8_t last_flags;
    std::uint32_t last_sequence;
};

class Sampler {
public:
    Sampler(edge::Runtime& runtime, Source& source);

    bool begin();

    void tick(std::uint64_t now_ms);

    void set_period(std::uint32_t period_ms) { period_ms_ = period_ms; }
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    const SamplerStats& stats() const { return stats_; }
    const Source& source() const { return source_; }

private:
    enum Tag : std::uint32_t {
        TAG_RESUME = 1,
        TAG_LATEST = 2,
        TAG_HISTORY = 3
    };

    static void on_complete(void* context, std::uint32_t tag, const edge::ClientResult& result);

    void handle(std::uint32_t tag, const edge::ClientResult& result);

    edge::Runtime& runtime_;
    Source& source_;

    std::uint32_t period_ms_ = config::SENSOR_PERIOD_MS;
    std::uint64_t next_sample_ms_ = 0;
    std::uint32_t sequence_ = 0;
    std::uint8_t in_flight_ = 0;
    bool resumed_ = false;
    bool resume_in_flight_ = false;
    bool enabled_ = true;

    SamplerStats stats_{};
};

} // namespace sensor

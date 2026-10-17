#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "node_id.h"
#include "node_runtime.h"

/*
 * Sensor acquisition path (work plan P7, decisions D1/D3/D4).
 *
 * Until the real sensors are wired, every node runs a
 * SimulatedSource that produces a smooth, plausible signal. A real
 * driver only has to implement sensor::Source; nothing else changes.
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
    LIGHT = 4        // milli-lux
};

const char* kind_key(Kind kind);   // "temp", "humidity", ...
const char* kind_unit(Kind kind);  // "degC", "%RH", "hPa", "lux"

// Default assignment until D1 names the real parts.
Kind kind_for_node(node::Id id);

/*
 * Reading wire format v1 (20 bytes, big-endian like the protocol):
 *
 *   0      format version (0x01)
 *   1      kind
 *   2      flags (bit 0: simulated source)
 *   3      reserved (0)
 *   4..7   sequence number (u32, per node, survives reboots)
 *   8..11  value in thousandths of the unit (i32)
 *   12..19 HLC of the sample (u64; physical part = cluster ms, see
 *          docs/DESIGN.md "Clocks" for wall-clock conversion)
 */
constexpr std::uint8_t READING_FORMAT = 0x01;
constexpr std::size_t READING_SIZE = 20;
constexpr std::uint8_t READING_FLAG_SIMULATED = 0x01;

struct Reading {
    Kind kind;
    std::uint8_t flags;
    std::uint32_t sequence;
    std::int32_t value_milli;
    std::uint64_t hlc;
};

bool encode(const Reading& reading, std::uint8_t (&out)[READING_SIZE]);
bool decode(const std::uint8_t* data, std::size_t length, Reading& reading);

// "s/A/temp"
std::size_t latest_key(node::Id id, Kind kind, char* out, std::size_t capacity);

// "h/A/temp/3"
std::size_t history_key(node::Id id, Kind kind, std::uint32_t sequence, char* out, std::size_t capacity);

class Source {
public:
    virtual ~Source() = default;

    virtual bool begin() = 0;
    virtual Kind kind() const = 0;
    virtual bool simulated() const = 0;

    // value in thousandths of the unit
    virtual bool read(std::uint64_t now_ms, std::int32_t& value_milli) = 0;
};

/*
 * Deterministic synthetic signal: a slow daily-like wave, a faster
 * ripple and bounded noise around a realistic base value.
 */
class SimulatedSource final : public Source {
public:
    SimulatedSource(Kind kind, std::uint32_t seed);

    bool begin() override;
    Kind kind() const override { return kind_; }
    bool simulated() const override { return true; }
    bool read(std::uint64_t now_ms, std::int32_t& value_milli) override;

private:
    std::uint32_t next_random();

    Kind kind_;
    std::uint32_t state_;
    std::int32_t noise_ = 0;
};

struct SamplerStats {
    std::uint32_t samples;
    std::uint32_t writes_ok;
    std::uint32_t writes_under_replicated;
    std::uint32_t writes_failed;
    std::uint32_t skipped_busy;
    std::uint32_t read_errors;
    std::int32_t last_value_milli;
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

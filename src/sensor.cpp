#include "sensor.h"

#include <cmath>
#include <cstdio>

namespace sensor {

namespace {

void put_u32(std::uint8_t* out, std::uint32_t value)
{
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t get_u32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

char letter(node::Id id)
{
    return (id >= node::MIN && id <= node::MAX)
        ? static_cast<char>('A' + (id - node::MIN))
        : '?';
}

bool valid_kind(std::uint8_t kind)
{
    return kind >= static_cast<std::uint8_t>(Kind::TEMPERATURE) &&
           kind <= static_cast<std::uint8_t>(Kind::LIGHT);
}

constexpr double PI = 3.14159265358979323846;

} // namespace

const char* kind_key(Kind kind)
{
    switch (kind) {
        case Kind::TEMPERATURE: return "temp";
        case Kind::HUMIDITY: return "humidity";
        case Kind::PRESSURE: return "pressure";
        case Kind::LIGHT: return "light";
    }

    return "unknown";
}

const char* kind_unit(Kind kind)
{
    switch (kind) {
        case Kind::TEMPERATURE: return "degC";
        case Kind::HUMIDITY: return "%RH";
        case Kind::PRESSURE: return "hPa";
        case Kind::LIGHT: return "lux";
    }

    return "";
}

Kind kind_for_node(node::Id id)
{
    switch (id) {
        case node::A: return Kind::TEMPERATURE;
        case node::B: return Kind::HUMIDITY;
        case node::C: return Kind::PRESSURE;
        default: return Kind::LIGHT;
    }
}

bool encode(const Reading& reading, std::uint8_t (&out)[READING_SIZE])
{
    if (!valid_kind(static_cast<std::uint8_t>(reading.kind))) {
        return false;
    }

    out[0] = READING_FORMAT;
    out[1] = static_cast<std::uint8_t>(reading.kind);
    out[2] = reading.flags;
    out[3] = 0;
    put_u32(out + 4, reading.sequence);
    put_u32(out + 8, static_cast<std::uint32_t>(reading.value_milli));
    put_u32(out + 12, static_cast<std::uint32_t>(reading.hlc >> 32));
    put_u32(out + 16, static_cast<std::uint32_t>(reading.hlc));

    return true;
}

bool decode(const std::uint8_t* data, std::size_t length, Reading& reading)
{
    if (data == nullptr ||
        length != READING_SIZE ||
        data[0] != READING_FORMAT ||
        !valid_kind(data[1]) ||
        data[3] != 0) {
        return false;
    }

    reading.kind = static_cast<Kind>(data[1]);
    reading.flags = data[2];
    reading.sequence = get_u32(data + 4);
    reading.value_milli = static_cast<std::int32_t>(get_u32(data + 8));
    reading.hlc =
        (static_cast<std::uint64_t>(get_u32(data + 12)) << 32) |
        get_u32(data + 16);

    return true;
}

std::size_t latest_key(node::Id id, Kind kind, char* out, std::size_t capacity)
{
    const int written = std::snprintf(out, capacity, "s/%c/%s", letter(id), kind_key(kind));
    return written > 0 && static_cast<std::size_t>(written) < capacity
        ? static_cast<std::size_t>(written)
        : 0;
}

std::size_t history_key(node::Id id, Kind kind, std::uint32_t sequence, char* out, std::size_t capacity)
{
    const int written = std::snprintf(
        out,
        capacity,
        "h/%c/%s/%u",
        letter(id),
        kind_key(kind),
        static_cast<unsigned>(sequence % config::SENSOR_HISTORY_SLOTS)
    );

    return written > 0 && static_cast<std::size_t>(written) < capacity
        ? static_cast<std::size_t>(written)
        : 0;
}

// ------------------------------------------------------------------
// Simulated source
// ------------------------------------------------------------------

SimulatedSource::SimulatedSource(Kind kind, std::uint32_t seed)
    : kind_(kind), state_(seed == 0 ? 0x9E3779B9u : seed)
{
}

bool SimulatedSource::begin()
{
    return true;
}

std::uint32_t SimulatedSource::next_random()
{
    // xorshift32
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
}

bool SimulatedSource::read(std::uint64_t now_ms, std::int32_t& value_milli)
{
    struct Profile {
        double base;
        double daily_amplitude;
        double ripple_amplitude;
        double noise_step;
        double min;
        double max;
    };

    Profile profile{};

    switch (kind_) {
        case Kind::TEMPERATURE: profile = {22.5, 3.0, 0.4, 0.05, -40.0, 85.0}; break;
        case Kind::HUMIDITY: profile = {48.0, 12.0, 2.0, 0.3, 0.0, 100.0}; break;
        case Kind::PRESSURE: profile = {1013.0, 4.0, 0.6, 0.05, 870.0, 1085.0}; break;
        case Kind::LIGHT: profile = {320.0, 280.0, 25.0, 4.0, 0.0, 100000.0}; break;
    }

    // A compressed "day" of 20 minutes so changes are visible in demos.
    const double t = static_cast<double>(now_ms) / 1000.0;
    const double daily = std::sin(2.0 * PI * t / 1200.0);
    const double ripple = std::sin(2.0 * PI * t / 97.0);

    // Bounded random walk.
    const std::int32_t step =
        static_cast<std::int32_t>(next_random() % 2001u) - 1000;

    noise_ += step;

    if (noise_ > 3000) noise_ = 3000;
    if (noise_ < -3000) noise_ = -3000;

    double value =
        profile.base +
        profile.daily_amplitude * daily +
        profile.ripple_amplitude * ripple +
        profile.noise_step * (static_cast<double>(noise_) / 1000.0);

    if (value < profile.min) value = profile.min;
    if (value > profile.max) value = profile.max;

    value_milli = static_cast<std::int32_t>(std::lround(value * 1000.0));
    return true;
}

// ------------------------------------------------------------------
// Sampler
// ------------------------------------------------------------------

Sampler::Sampler(edge::Runtime& runtime, Source& source)
    : runtime_(runtime), source_(source)
{
}

bool Sampler::begin()
{
    return source_.begin();
}

void Sampler::on_complete(void* context, std::uint32_t tag, const edge::ClientResult& result)
{
    static_cast<Sampler*>(context)->handle(tag, result);
}

void Sampler::handle(std::uint32_t tag, const edge::ClientResult& result)
{
    if (tag == TAG_RESUME) {
        resume_in_flight_ = false;

        Reading last{};

        if (result.status == edge::ClientStatus::OK &&
            decode(result.value, result.value_length, last)) {
            sequence_ = last.sequence + 1;
            resumed_ = true;
        } else if (result.status == edge::ClientStatus::NOT_FOUND) {
            sequence_ = 0;
            resumed_ = true;
        }

        // Anything else: try the resume read again on the next tick.
        return;
    }

    if (in_flight_ > 0) {
        --in_flight_;
    }

    if (result.status == edge::ClientStatus::OK) {
        ++stats_.writes_ok;
    } else if (result.status == edge::ClientStatus::UNDER_REPLICATED) {
        ++stats_.writes_under_replicated;
    } else {
        ++stats_.writes_failed;
    }
}

void Sampler::tick(std::uint64_t now_ms)
{
    if (!enabled_ || runtime_.state() != edge::NodeState::ACTIVE) {
        return;
    }

    const node::Id self = runtime_.local_node_id();
    char key[storage::KEY_MAX + 1];

    // Continue the sequence across reboots: read our latest sample
    // back from the cluster first.
    if (!resumed_) {
        if (resume_in_flight_) {
            return;
        }

        const std::size_t length = latest_key(self, source_.kind(), key, sizeof(key));

        if (runtime_.submit(
                edge::OpType::GET,
                reinterpret_cast<const std::uint8_t*>(key),
                length,
                nullptr,
                0,
                on_complete,
                this,
                TAG_RESUME,
                now_ms
            ) == edge::ClientStatus::OK) {

            resume_in_flight_ = true;

            // Stagger the nodes' sampling instants.
            next_sample_ms_ =
                now_ms + 1000u +
                static_cast<std::uint64_t>(self - node::MIN) * (period_ms_ / 4u);
        }

        return;
    }

    if (now_ms < next_sample_ms_) {
        return;
    }

    next_sample_ms_ = now_ms + period_ms_;

    if (in_flight_ > 0 || runtime_.free_client_slots() < 2) {
        ++stats_.skipped_busy;
        return;
    }

    std::int32_t value = 0;

    if (!source_.read(now_ms, value)) {
        ++stats_.read_errors;
        return;
    }

    const Reading reading{
        source_.kind(),
        static_cast<std::uint8_t>(source_.simulated() ? READING_FLAG_SIMULATED : 0),
        sequence_,
        value,
        runtime_.clock().now()
    };

    std::uint8_t encoded[READING_SIZE];

    if (!encode(reading, encoded)) {
        ++stats_.read_errors;
        return;
    }

    ++stats_.samples;
    stats_.last_value_milli = value;
    stats_.last_sequence = sequence_;

    const std::size_t latest_length = latest_key(self, source_.kind(), key, sizeof(key));

    if (runtime_.submit(
            edge::OpType::SET,
            reinterpret_cast<const std::uint8_t*>(key),
            latest_length,
            encoded,
            sizeof(encoded),
            on_complete,
            this,
            TAG_LATEST,
            now_ms
        ) == edge::ClientStatus::OK) {
        ++in_flight_;
    } else {
        ++stats_.writes_failed;
    }

    const std::size_t history_length =
        history_key(self, source_.kind(), sequence_, key, sizeof(key));

    if (runtime_.submit(
            edge::OpType::SET,
            reinterpret_cast<const std::uint8_t*>(key),
            history_length,
            encoded,
            sizeof(encoded),
            on_complete,
            this,
            TAG_HISTORY,
            now_ms
        ) == edge::ClientStatus::OK) {
        ++in_flight_;
    } else {
        ++stats_.writes_failed;
    }

    ++sequence_;
}

} // namespace sensor

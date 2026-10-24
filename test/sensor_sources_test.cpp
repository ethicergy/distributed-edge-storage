/*
 * Real-sensor logic (sensor_sources.h) over fake inputs: AM312 motion,
 * LM393 sound (AO sample arrays), DHT11-style latest value, and the
 * fallback switch.
 */

#include <iostream>
#include <string>
#include <vector>

#include "sensor_sources.h"
#include "test_support.h"

namespace {

// A GPIO whose level the test sets.
class FakeInput final : public sensor::DigitalInput {
public:
    bool begin() override
    {
        ++begins;
        return begin_ok;
    }

    bool level() override
    {
        ++reads;
        return high;
    }

    bool begin_ok = true;
    bool high = false;
    int begins = 0;
    int reads = 0;
};

class FakeValueInput final : public sensor::ValueInput {
public:
    bool begin() override { return true; }

    sensor::ValueAttempts take() override
    {
        sensor::ValueAttempts out = pending;
        pending = sensor::ValueAttempts{};
        return out;
    }

    void attempt(bool ok, std::int32_t value = 0, std::uint64_t at_ms = 0)
    {
        if (ok) {
            ++pending.ok;
            pending.last_good_milli = value;
            pending.last_good_ms = at_ms;
        } else {
            ++pending.failed;
        }
    }

    sensor::ValueAttempts pending;
};

// A synchronous real source (like the BH1750) that can be made to fail.
class FakeLightSource final : public sensor::Source {
public:
    bool begin() override { return begin_ok; }
    sensor::Kind kind() const override { return sensor::Kind::LIGHT; }
    bool simulated() const override { return false; }

    bool read(std::uint64_t, sensor::Sample& sample) override
    {
        ++reads;

        if (!read_ok) {
            ++failed;
            return false;
        }

        sample = sensor::Sample{};
        sample.value_milli = 123000;
        return true;
    }

    std::uint32_t failures() const override { return failed; }

    bool begin_ok = true;
    bool read_ok = true;
    int reads = 0;
    std::uint32_t failed = 0;
};

// Poll every `step` ms from `from` to `to` (exclusive), like the node task.
void run(sensor::Source& source, std::uint64_t from, std::uint64_t to, std::uint64_t step = 1)
{
    for (std::uint64_t t = from; t < to; t += step) {
        source.poll(t);
    }
}

void test_motion_settling_then_detections()
{
    FakeInput pir;
    sensor::MotionSource motion(pir, 5000, 100);
    REQUIRE(motion.begin());
    REQUIRE(motion.kind() == sensor::Kind::MOTION);
    REQUIRE(!motion.simulated());

    // Toggling while settling is ignored and reported as settling.
    pir.high = true;
    run(motion, 1000, 3000);
    pir.high = false;
    run(motion, 3000, 5000);

    sensor::Sample sample{};
    REQUIRE(motion.read(5000, sample));
    REQUIRE(sample.flags == sensor::READING_FLAG_SETTLING);
    REQUIRE(sample.value_milli == 0);
    REQUIRE(motion.settling(5999));

    // Settled at 6000 (first poll at 1000 + 5000). No motion yet.
    run(motion, 5000, 8000);
    REQUIRE(!motion.settling(6000));
    REQUIRE(motion.read(8000, sample));
    REQUIRE(sample.flags == 0);
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == sensor::AUX_NONE);

    // Two ~2 s detections in the window.
    pir.high = true;
    run(motion, 8000, 10000);
    pir.high = false;
    run(motion, 10000, 12000);
    pir.high = true;
    run(motion, 12000, 14000);
    pir.high = false;
    run(motion, 14000, 20000);

    REQUIRE(motion.read(20000, sample));
    REQUIRE(sample.value_milli == 2000);
    // Last seen high at the poll just before 14000.
    REQUIRE(sample.aux == 6);

    // Next window: no new detection, age keeps growing.
    run(motion, 20000, 30000);
    REQUIRE(motion.read(30000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 16);

    // Polled every 100 ms, not on every node-task iteration.
    run(motion, 30000, 31000);
    const int reads_before = pir.reads;
    run(motion, 31000, 32000);
    REQUIRE(pir.reads - reads_before == 10);
}

void test_motion_high_across_window_counts_once()
{
    FakeInput pir;
    sensor::MotionSource motion(pir, 0, 100);
    REQUIRE(motion.begin());

    pir.high = true;
    run(motion, 0, 1000);

    sensor::Sample sample{};
    REQUIRE(motion.read(1000, sample));
    REQUIRE(sample.value_milli == 1000);
    REQUIRE(sample.aux == 0);

    // Still the same detection in the next window.
    run(motion, 1000, 1500);
    pir.high = false;
    run(motion, 1500, 2000);
    REQUIRE(motion.read(2000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 0);
}

// ------------------------------------------------------------------
// LM393 AO: synthetic 1 kHz sample arrays (mV), cut into blocks the way
// the sampling task does. 0 dB attenuation: the ADC reads 0..750 mV.
// ------------------------------------------------------------------

constexpr std::uint16_t CLIP_LOW_MV = 0;
constexpr std::uint16_t CLIP_HIGH_MV = 750;
constexpr std::uint16_t REST_MV = 400;
constexpr std::uint16_t THRESHOLD_MV = 40;

using Samples = std::vector<std::uint16_t>;

// Deterministic noise in [-amplitude, +amplitude].
std::int32_t noise(std::uint32_t& state, std::int32_t amplitude)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<std::int32_t>((state >> 16) % static_cast<std::uint32_t>(2 * amplitude + 1)) - amplitude;
}

std::uint16_t adc(std::int32_t mv)
{
    if (mv < CLIP_LOW_MV) {
        return CLIP_LOW_MV;
    }
    return static_cast<std::uint16_t>(mv > CLIP_HIGH_MV ? CLIP_HIGH_MV : mv);
}

// Quiet room: rest +- 4 mV of ADC noise.
void append_silence(Samples& out, std::size_t ms, std::uint16_t rest = REST_MV)
{
    static std::uint32_t state = 1;
    for (std::size_t i = 0; i < ms; ++i) {
        out.push_back(adc(rest + noise(state, 4)));
    }
}

// A sound around rest: alternating sign (kHz content sampled at 1 kHz),
// amplitude between `low` and `high` mV.
void append_sound(Samples& out, std::size_t ms, std::int32_t low, std::int32_t high, std::uint16_t rest = REST_MV)
{
    static std::uint32_t state = 7;
    for (std::size_t i = 0; i < ms; ++i) {
        const std::int32_t amplitude = low + (noise(state, (high - low) / 2) + (high - low) / 2);
        out.push_back(adc(rest + ((i % 2) != 0 ? amplitude : -amplitude)));
    }
}

// A clap: 40 ms loud, 20 ms below the threshold, 30 ms of ringing.
void append_clap(Samples& out)
{
    append_sound(out, 40, 100, 200);
    append_silence(out, 20);
    append_sound(out, 30, 30, 60);
}

class FakeBlocks final : public sensor::SoundBlockInput {
public:
    bool begin() override { return begin_ok; }

    std::size_t take(sensor::SoundBlock* out, std::size_t capacity) override
    {
        std::size_t n = 0;
        while (n < capacity && next < blocks.size()) {
            out[n++] = blocks[next++];
        }
        return n;
    }

    std::uint32_t dropped() const override { return dropped_blocks; }
    std::uint32_t sample_cost_us() const override { return 23; }

    // Sample k of `samples` is taken at start_ms + k.
    void feed(const Samples& samples, std::uint64_t start_ms)
    {
        for (std::size_t i = 0; i + config::SOUND_BLOCK_SAMPLES <= samples.size(); i += config::SOUND_BLOCK_SAMPLES) {
            blocks.push_back(sensor::summarize_sound_block(
                &samples[i], config::SOUND_BLOCK_SAMPLES, CLIP_LOW_MV, CLIP_HIGH_MV,
                start_ms + i + config::SOUND_BLOCK_SAMPLES - 1
            ));
        }
    }

    bool begin_ok = true;
    std::vector<sensor::SoundBlock> blocks;
    std::size_t next = 0;
    std::uint32_t dropped_blocks = 0;
};

sensor::SoundBlock block(std::uint16_t min_mv, std::uint16_t max_mv, std::uint64_t end_ms)
{
    sensor::SoundBlock b{};
    b.min_mv = min_mv;
    b.max_mv = max_mv;
    b.mean_mv = static_cast<std::uint16_t>((min_mv + max_mv) / 2);
    b.end_ms = end_ms;
    return b;
}

void test_sound_block_summary()
{
    const std::uint16_t samples[] = {400, 410, 395, 420, 405};
    sensor::SoundBlock b = sensor::summarize_sound_block(samples, 5, CLIP_LOW_MV, CLIP_HIGH_MV, 99);
    REQUIRE(b.min_mv == 395);
    REQUIRE(b.max_mv == 420);
    REQUIRE(sensor::swing_mv(b) == 25);
    REQUIRE(b.mean_mv == 406);
    REQUIRE(!b.clipped);
    REQUIRE(b.end_ms == 99);

    const std::uint16_t top[] = {700, 750, 720};
    REQUIRE(sensor::summarize_sound_block(top, 3, CLIP_LOW_MV, CLIP_HIGH_MV, 0).clipped);
    const std::uint16_t bottom[] = {0, 30, 10};
    REQUIRE(sensor::summarize_sound_block(bottom, 3, CLIP_LOW_MV, CLIP_HIGH_MV, 0).clipped);

    // Empty block: no swing, never active.
    b = sensor::summarize_sound_block(samples, 0, CLIP_LOW_MV, CLIP_HIGH_MV, 5);
    REQUIRE(sensor::swing_mv(b) == 0);
    REQUIRE(b.end_ms == 5);
}

void test_sound_silence()
{
    FakeBlocks mic;
    sensor::SoundSource sound(mic, THRESHOLD_MV, 150, config::SOUND_BLOCK_MS);
    REQUIRE(sound.begin());
    REQUIRE(sound.kind() == sensor::Kind::SOUND);

    Samples samples;
    append_silence(samples, 10000);
    mic.feed(samples, 0);

    sound.poll(0);
    sensor::Sample sample{};
    REQUIRE(sound.read(10000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 0);

    const sensor::SoundWindow& w = sound.last_window();
    REQUIRE(w.blocks == 1000);
    REQUIRE(w.active_blocks == 0);
    REQUIRE(w.clipped_blocks == 0);
    REQUIRE(w.max_swing_mv <= 8);
    REQUIRE(w.mean_mv >= REST_MV - 2 && w.mean_mv <= REST_MV + 2);
}

void test_sound_clap()
{
    FakeBlocks mic;
    sensor::SoundSource sound(mic, THRESHOLD_MV, 150, config::SOUND_BLOCK_MS);
    REQUIRE(sound.begin());

    // One clap at 1 s (in and out of the threshold: one event), two
    // claps 1 s apart from 5 s.
    Samples samples;
    append_silence(samples, 1000);
    append_clap(samples);           // 1000-1089
    append_silence(samples, 3910);  // to 5000
    append_clap(samples);           // 5000-5089
    append_silence(samples, 910);   // to 6000
    append_clap(samples);           // 6000-6089
    append_silence(samples, 3910);  // to 10000
    REQUIRE(samples.size() == 10000);
    mic.feed(samples, 0);

    sound.poll(0);
    sensor::Sample sample{};
    REQUIRE(sound.read(10000, sample));
    REQUIRE(sample.value_milli == 3000);
    // 7 active blocks per clap (4 loud + 3 ringing) = 210 ms of 10 s.
    REQUIRE(sound.last_window().active_blocks == 21);
    REQUIRE(sample.aux == 2100); // 2.100 %
    REQUIRE(sound.last_window().max_swing_mv >= 200);
    REQUIRE(sound.last_window().clipped_blocks == 0);

    // Next window quiet again.
    samples.clear();
    append_silence(samples, 10000);
    mic.feed(samples, 10000);
    REQUIRE(sound.read(20000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 0);
}

void test_sound_steady_noise()
{
    FakeBlocks mic;
    sensor::SoundSource sound(mic, THRESHOLD_MV, 150, config::SOUND_BLOCK_MS);
    REQUIRE(sound.begin());

    // Loud steady noise (fan, music) for two windows: one event when it
    // starts, active the whole time, no new event while it lasts.
    Samples samples;
    append_sound(samples, 3000, 30, 60);
    mic.feed(samples, 0);

    sound.poll(0);
    sensor::Sample sample{};
    REQUIRE(sound.read(3000, sample));
    REQUIRE(sample.value_milli == 1000);
    REQUIRE(sample.aux == 100000);
    mic.feed(samples, 3000);
    REQUIRE(sound.read(6000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 100000);

    // Noise below the threshold is silence.
    samples.clear();
    append_sound(samples, 3000, 5, 15);
    mic.feed(samples, 6000);
    REQUIRE(sound.read(9000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 0);
}

void test_sound_clipping()
{
    FakeBlocks mic;
    sensor::SoundSource sound(mic, THRESHOLD_MV, 150, config::SOUND_BLOCK_MS);
    REQUIRE(sound.begin());

    // A clap loud enough to hit both ADC limits: still one event, the
    // swing is the full range, and the clipping is counted.
    Samples samples;
    append_silence(samples, 1000);
    append_sound(samples, 40, 500, 600);
    append_silence(samples, 1960);
    mic.feed(samples, 0);

    sound.poll(0);
    sensor::Sample sample{};
    REQUIRE(sound.read(3000, sample));
    REQUIRE(sample.value_milli == 1000);
    REQUIRE(sound.last_window().max_swing_mv == CLIP_HIGH_MV - CLIP_LOW_MV);
    REQUIRE(sound.last_window().clipped_blocks == 4);

    // AO resting above the ADC range (wrong attenuation): every sample
    // reads full scale. No swing, so no events, but every block clipped.
    samples.clear();
    append_silence(samples, 3000, 900);
    mic.feed(samples, 3000);
    REQUIRE(sound.read(6000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 0);
    REQUIRE(sound.last_window().clipped_blocks == sound.last_window().blocks);
    REQUIRE(sound.last_window().mean_mv == CLIP_HIGH_MV);
}

void test_sound_gap_and_threshold()
{
    sensor::SoundDetector detector(THRESHOLD_MV, 150);

    // Exactly the threshold is not active; one above is.
    detector.add(block(400, 440, 10));
    REQUIRE(detector.take().active_blocks == 0);

    detector.add(block(400, 441, 20));  // crossing at 20: event 1
    detector.add(block(400, 401, 30));
    detector.add(block(380, 500, 160)); // 140 ms later: same event
    detector.add(block(400, 401, 170));
    detector.add(block(380, 500, 310)); // 150 ms after the last crossing: event 2
    detector.add(block(380, 500, 320)); // still active, no crossing

    const sensor::SoundWindow w = detector.take();
    REQUIRE(w.events == 2);
    REQUIRE(w.blocks == 6);
    REQUIRE(w.active_blocks == 4);
    REQUIRE(w.max_swing_mv == 120);

    // Active across the window boundary: no new event in the next one.
    detector.add(block(380, 500, 330));
    REQUIRE(detector.take().events == 0);
}

void test_sound_no_blocks_is_a_failed_read()
{
    FakeBlocks mic;
    sensor::SoundSource sound(mic, THRESHOLD_MV, 150, config::SOUND_BLOCK_MS);
    REQUIRE(sound.begin());

    sound.poll(0);
    sensor::Sample sample{};
    REQUIRE(!sound.read(5000, sample)); // sampling task stalled
    REQUIRE(sound.failures() == 1);

    Samples samples;
    append_silence(samples, 5000);
    mic.feed(samples, 5000);
    REQUIRE(sound.read(10000, sample));
    REQUIRE(sound.failures() == 1);

    FakeBlocks dead;
    dead.begin_ok = false;
    sensor::SoundSource no_task(dead);
    REQUIRE(!no_task.begin());
}

void test_sound_describe()
{
    FakeBlocks mic;
    mic.dropped_blocks = 3;
    sensor::SoundSource sound(mic, THRESHOLD_MV, 150, config::SOUND_BLOCK_MS);
    REQUIRE(sound.begin());

    char text[160];
    REQUIRE(sound.describe(text, sizeof(text)) == 0); // no window yet

    Samples samples;
    append_silence(samples, 1000);
    append_clap(samples);
    append_silence(samples, 3910);
    mic.feed(samples, 0);
    sound.poll(0);
    sensor::Sample sample{};
    REQUIRE(sound.read(5000, sample));

    const std::size_t n = sound.describe(text, sizeof(text));
    REQUIRE(n > 0 && n == std::string(text).size());
    std::cout << "  describe: " << text << "\n";
    const std::string rest = "rest " + std::to_string(sound.last_window().mean_mv) + " mV";
    REQUIRE(std::string(text).find(rest) != std::string::npos);
    REQUIRE(std::string(text).find("threshold 40 mV") != std::string::npos);
    REQUIRE(std::string(text).find("7/500 blocks active") != std::string::npos);
    REQUIRE(std::string(text).find("sampled 100 %") != std::string::npos);
    REQUIRE(std::string(text).find("3 dropped") != std::string::npos);
    REQUIRE(std::string(text).find("23 us/sample") != std::string::npos);

    // Truncates safely.
    char small[8];
    REQUIRE(sound.describe(small, sizeof(small)) == 7);
    REQUIRE(std::string(small).size() == 7);

    // The fallback switch shows the real source's detail.
    sensor::SimulatedSource sim(sensor::Kind::SOUND, 1);
    sensor::FallbackSource top(&sound, sim);
    REQUIRE(top.begin());
    REQUIRE(top.describe(text, sizeof(text)) == n);

    sensor::FallbackSource none(nullptr, sim);
    REQUIRE(none.begin());
    REQUIRE(none.describe(text, sizeof(text)) == 0);
}

void test_latest_value_keeps_last_good()
{
    FakeValueInput dht;
    sensor::LatestValueSource source(sensor::Kind::TEMPERATURE, dht);
    REQUIRE(source.begin());

    sensor::Sample sample{};

    // Nothing good yet: no value.
    dht.attempt(false);
    REQUIRE(!source.read(0, sample));
    REQUIRE(source.failures() == 1);

    dht.attempt(true, 21500, 1000);
    dht.attempt(true, 21700, 2000);
    REQUIRE(source.read(2000, sample));
    REQUIRE(sample.value_milli == 21700);
    REQUIRE(sample.flags == 0);

    // Failures keep the last good value and are counted.
    dht.attempt(false);
    dht.attempt(false);
    REQUIRE(source.read(4000, sample));
    REQUIRE(sample.value_milli == 21700);
    REQUIRE(source.failures() == 3);

    // No attempt finished in this window: still the last good value.
    REQUIRE(source.read(5000, sample));
    REQUIRE(sample.value_milli == 21700);
    REQUIRE(sample.flags == 0);
}

void test_latest_value_stale_and_error()
{
    sensor::Sample sample{};

    // Last good read at 2000; DHT_STALE_MS (10 s) later the reading is
    // stale but keeps the last good value.
    {
        FakeValueInput dht;
        sensor::LatestValueSource source(sensor::Kind::TEMPERATURE, dht, 10000);
        REQUIRE(source.begin());
        source.poll(0);

        dht.attempt(true, 22100, 2000);
        REQUIRE(source.read(2000, sample));
        REQUIRE(sample.flags == 0);

        dht.attempt(false);
        dht.attempt(false);
        REQUIRE(source.read(12000, sample));
        REQUIRE(sample.flags == 0); // exactly 10 s: not yet

        dht.attempt(false);
        REQUIRE(source.read(12001, sample));
        REQUIRE(sample.flags == sensor::READING_FLAG_STALE);
        REQUIRE(sample.value_milli == 22100);
        REQUIRE(source.failures() == 3);

        // A good read clears it.
        dht.attempt(true, 22300, 14000);
        REQUIRE(source.read(14000, sample));
        REQUIRE(sample.flags == 0);
        REQUIRE(sample.value_milli == 22300);
    }

    // Never a good read: no reading for the first 10 s, then error.
    {
        FakeValueInput dht;
        sensor::LatestValueSource source(sensor::Kind::TEMPERATURE, dht, 10000);
        REQUIRE(source.begin());
        source.poll(500);

        dht.attempt(false);
        REQUIRE(!source.read(5000, sample));
        REQUIRE(!source.read(10500, sample));

        REQUIRE(source.read(10501, sample));
        REQUIRE(sample.flags == sensor::READING_FLAG_ERROR);
        REQUIRE(sample.value_milli == 0);
    }
}

void test_fallback_never_replaces_dht()
{
    // Even with many failed reads in a row, a source that forbids
    // fallback is never replaced by simulated values.
    FakeValueInput dht;
    sensor::LatestValueSource real(sensor::Kind::TEMPERATURE, dht, 10000);
    sensor::SimulatedSource simulated(sensor::Kind::TEMPERATURE, 1);
    sensor::FallbackSource source(&real, simulated, 3);
    REQUIRE(source.begin());
    REQUIRE(source.real_enabled());

    sensor::Sample sample{};

    for (std::uint64_t t = 0; t <= 10000; t += 1000) {
        REQUIRE(!source.read(t, sample));
        REQUIRE(!source.simulated());
    }

    REQUIRE(source.read(11000, sample));
    REQUIRE(!source.simulated());
    REQUIRE(sample.flags == sensor::READING_FLAG_ERROR);

    // begin() failing does not hand it to the simulated source either.
    class FailingInput final : public sensor::ValueInput {
    public:
        bool begin() override { return false; }
        sensor::ValueAttempts take() override { return {}; }
    } broken;

    sensor::LatestValueSource dead(sensor::Kind::TEMPERATURE, broken, 10000);
    sensor::FallbackSource guarded(&dead, simulated, 3);
    REQUIRE(guarded.begin());
    REQUIRE(guarded.real_enabled());
    REQUIRE(!guarded.read(0, sample));
    REQUIRE(guarded.read(20000, sample));
    REQUIRE(!guarded.simulated());
    REQUIRE(sample.flags == sensor::READING_FLAG_ERROR);
}

void test_fallback_switch()
{
    sensor::Sample sample{};

    // No real source: simulated.
    {
        sensor::SimulatedSource simulated(sensor::Kind::LIGHT, 1);
        sensor::FallbackSource source(nullptr, simulated);
        REQUIRE(source.begin());
        REQUIRE(!source.real_enabled());
        REQUIRE(source.read(1000, sample));
        REQUIRE(source.simulated());
    }

    // Real source whose begin() fails: simulated, real never read.
    {
        FakeLightSource real;
        real.begin_ok = false;
        sensor::SimulatedSource simulated(sensor::Kind::LIGHT, 1);
        sensor::FallbackSource source(&real, simulated, 3);
        REQUIRE(source.begin());
        REQUIRE(!source.real_enabled());
        REQUIRE(source.read(1000, sample));
        REQUIRE(source.simulated());
        REQUIRE(real.reads == 0);
    }

    // Kind mismatch: the real source is not used.
    {
        FakeLightSource real;
        sensor::SimulatedSource simulated(sensor::Kind::TEMPERATURE, 1);
        sensor::FallbackSource source(&real, simulated, 3);
        REQUIRE(source.begin());
        REQUIRE(!source.real_enabled());
    }

    // Working real source; after 3 failures in a row the simulated one
    // stands in; a good read switches back.
    {
        FakeLightSource real;
        sensor::SimulatedSource simulated(sensor::Kind::LIGHT, 1);
        sensor::FallbackSource source(&real, simulated, 3);
        REQUIRE(source.begin());
        REQUIRE(source.real_enabled());

        REQUIRE(source.read(1000, sample));
        REQUIRE(!source.simulated());
        REQUIRE(sample.value_milli == 123000);

        real.read_ok = false;
        REQUIRE(!source.read(2000, sample));
        REQUIRE(!source.read(3000, sample));
        REQUIRE(source.read(4000, sample));
        REQUIRE(source.simulated());
        REQUIRE(sample.value_milli != 123000);
        REQUIRE(source.read(5000, sample));
        REQUIRE(source.simulated());

        real.read_ok = true;
        REQUIRE(source.read(6000, sample));
        REQUIRE(!source.simulated());
        REQUIRE(sample.value_milli == 123000);

        // Own count (4) plus the real source's (4).
        REQUIRE(source.failures() == 8);
    }

    // poll() reaches the real source.
    {
        FakeInput pir;
        sensor::MotionSource real(pir, 0, 100);
        sensor::SimulatedSource simulated(sensor::Kind::MOTION, 1);
        sensor::FallbackSource source(&real, simulated);
        REQUIRE(source.begin());
        REQUIRE(pir.begins == 1);

        pir.high = true;
        run(source, 0, 500);
        REQUIRE(source.read(500, sample));
        REQUIRE(!source.simulated());
        REQUIRE(sample.value_milli == 1000);
    }
}

void test_real_sensor_mask()
{
    REQUIRE(!sensor::real_sensor_enabled(node::A, 0x00));
    REQUIRE(sensor::real_sensor_enabled(node::A, 0x01));
    REQUIRE(!sensor::real_sensor_enabled(node::B, 0x01));
    REQUIRE(sensor::real_sensor_enabled(node::D, 0x08));
    REQUIRE(sensor::real_sensor_enabled(node::C, 0x0F));
    REQUIRE(!sensor::real_sensor_enabled(node::INVALID, 0xFF));

    // Shipped default follows config::REAL_SENSOR_NODES (bits set per
    // module after its hardware check, docs/BRINGUP.md 4.6).
    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        const bool bit = (config::REAL_SENSOR_NODES >> (id - node::MIN)) & 0x01;
        REQUIRE(sensor::real_sensor_enabled(id) == bit);
    }
}

} // namespace

int main()
{
    test_motion_settling_then_detections();
    test_motion_high_across_window_counts_once();
    test_sound_block_summary();
    test_sound_silence();
    test_sound_clap();
    test_sound_steady_noise();
    test_sound_clipping();
    test_sound_gap_and_threshold();
    test_sound_no_blocks_is_a_failed_read();
    test_sound_describe();
    test_latest_value_keeps_last_good();
    test_latest_value_stale_and_error();
    test_fallback_never_replaces_dht();
    test_fallback_switch();
    test_real_sensor_mask();

    std::cout << "Sensor source tests: PASS\n";
    return 0;
}

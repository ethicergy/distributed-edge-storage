/*
 * Real-sensor logic (sensor_sources.h) over fake inputs: AM312 motion,
 * LM393 sound, DHT11-style latest value, and the fallback switch.
 */

#include <iostream>
#include <vector>

#include "sensor_sources.h"
#include "test_support.h"

namespace {

// A GPIO whose level the test sets; edges() is what an ISR counting
// edges into the active level would report.
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

    std::uint32_t edges() override { return edge_count; }

    bool begin_ok = true;
    bool high = false;
    std::uint32_t edge_count = 0;
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

    void attempt(bool ok, std::int32_t value = 0)
    {
        if (ok) {
            ++pending.ok;
            pending.last_good_milli = value;
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

void test_sound_events_and_duty()
{
    FakeInput mic;
    mic.high = true; // active-low idle
    sensor::SoundSource sound(mic, false, 150);
    REQUIRE(sound.begin());
    REQUIRE(sound.kind() == sensor::Kind::SOUND);

    run(sound, 0, 1000);

    // Clap 1: three chattering edges within 40 ms, active 100 ms.
    for (std::uint64_t t = 1000; t < 1100; ++t) {
        if (t == 1000 || t == 1020 || t == 1040) {
            ++mic.edge_count;
        }
        mic.high = false;
        sound.poll(t);
    }
    mic.high = true;
    run(sound, 1100, 3000);

    // Clap 2: one edge, active 300 ms.
    ++mic.edge_count;
    mic.high = false;
    run(sound, 3000, 3300);
    mic.high = true;
    run(sound, 3300, 10000);

    sensor::Sample sample{};
    REQUIRE(sound.read(10000, sample));
    REQUIRE(sample.value_milli == 2000);
    REQUIRE(sample.aux == 4000); // 400 ms of 10 s = 4.000 %
    REQUIRE(sound.total_edges() == 4);

    // Quiet window.
    run(sound, 10000, 20000);
    REQUIRE(sound.read(20000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 0);

    // Knob too sensitive: DO stuck active, no edges -> 100 % active.
    mic.high = false;
    run(sound, 20000, 30000);
    REQUIRE(sound.read(30000, sample));
    REQUIRE(sample.value_milli == 0);
    REQUIRE(sample.aux == 100000);
}

void test_sound_edges_wrap_and_gap()
{
    FakeInput mic;
    mic.edge_count = 0xFFFFFFFEu;
    sensor::SoundSource sound(mic, true, 150);
    REQUIRE(sound.begin());

    sound.poll(0);
    mic.edge_count += 3; // wraps past zero
    sound.poll(10);
    mic.edge_count += 1; // 140 ms later: same event
    sound.poll(150);
    mic.edge_count += 1; // 150 ms after the last edge: new event
    sound.poll(300);

    sensor::Sample sample{};
    REQUIRE(sound.read(1000, sample));
    REQUIRE(sound.total_edges() == 5);
    REQUIRE(sample.value_milli == 2000);
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

    dht.attempt(true, 21500);
    dht.attempt(true, 21700);
    REQUIRE(source.read(2000, sample));
    REQUIRE(sample.value_milli == 21700);

    // Failures keep the last good value and are counted.
    dht.attempt(false);
    dht.attempt(false);
    REQUIRE(source.read(4000, sample));
    REQUIRE(sample.value_milli == 21700);
    REQUIRE(source.failures() == 3);

    // No attempt finished in this window: still the last good value.
    REQUIRE(source.read(5000, sample));
    REQUIRE(sample.value_milli == 21700);
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

    // Shipped default: every node simulated.
    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        REQUIRE(!sensor::real_sensor_enabled(id));
    }
}

} // namespace

int main()
{
    test_motion_settling_then_detections();
    test_motion_high_across_window_counts_once();
    test_sound_events_and_duty();
    test_sound_edges_wrap_and_gap();
    test_latest_value_keeps_last_good();
    test_fallback_switch();
    test_real_sensor_mask();

    std::cout << "Sensor source tests: PASS\n";
    return 0;
}

/*
 * Sensor reading codec, key schema and the simulated sources.
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "sensor.h"
#include "test_support.h"

namespace {

void test_codec_roundtrip()
{
    const sensor::Reading reading{
        sensor::Kind::PRESSURE,
        sensor::READING_FLAG_SIMULATED,
        123456,
        -40250,
        0x0102030405060708ULL
    };

    std::uint8_t encoded[sensor::READING_MAX_SIZE];
    REQUIRE(sensor::encode(reading, encoded) == sensor::READING_SIZE);

    // Big-endian layout (see sensor.h).
    REQUIRE(encoded[0] == 0x01);
    REQUIRE(encoded[1] == 3);
    REQUIRE(encoded[2] == 0x01);
    REQUIRE(encoded[3] == 0x00);
    REQUIRE(encoded[4] == 0x00 && encoded[5] == 0x01 && encoded[6] == 0xE2 && encoded[7] == 0x40);
    REQUIRE(encoded[12] == 0x01 && encoded[19] == 0x08);

    sensor::Reading decoded{};
    REQUIRE(sensor::decode(encoded, sensor::READING_SIZE, decoded));
    REQUIRE(decoded.kind == reading.kind);
    REQUIRE(decoded.flags == reading.flags);
    REQUIRE(decoded.sequence == reading.sequence);
    REQUIRE(decoded.value_milli == reading.value_milli);
    REQUIRE(decoded.hlc == reading.hlc);
    REQUIRE(decoded.aux == 0);

    // Rejections.
    REQUIRE(!sensor::decode(encoded, sensor::READING_SIZE - 1, decoded));
    REQUIRE(!sensor::decode(encoded, sensor::READING_SIZE_AUX, decoded));
    encoded[0] = 0x02;
    REQUIRE(!sensor::decode(encoded, sensor::READING_SIZE, decoded));
    encoded[0] = 0x01;
    encoded[1] = 9;
    REQUIRE(!sensor::decode(encoded, sensor::READING_SIZE, decoded));

    // Arbitrary text is not mistaken for a reading.
    const char* text = "hello, this is text!";
    REQUIRE(std::strlen(text) == sensor::READING_SIZE);
    REQUIRE(!sensor::decode(reinterpret_cast<const std::uint8_t*>(text), 20, decoded));
}

void test_codec_event_kinds()
{
    const sensor::Reading motion{
        sensor::Kind::MOTION,
        sensor::READING_FLAG_SETTLING,
        7,
        3000,
        0x0102030405060708ULL,
        sensor::AUX_NONE
    };

    std::uint8_t encoded[sensor::READING_MAX_SIZE];
    REQUIRE(sensor::encode(motion, encoded) == sensor::READING_SIZE_AUX);

    // v2 layout: v1 fields, then aux.
    REQUIRE(encoded[0] == 0x02);
    REQUIRE(encoded[1] == 5);
    REQUIRE(encoded[2] == 0x02);
    REQUIRE(encoded[3] == 0x00);
    REQUIRE(encoded[11] == 0xB8);
    REQUIRE(encoded[12] == 0x01 && encoded[19] == 0x08);
    REQUIRE(encoded[20] == 0xFF && encoded[23] == 0xFF);

    sensor::Reading decoded{};
    REQUIRE(sensor::decode(encoded, sensor::READING_SIZE_AUX, decoded));
    REQUIRE(decoded.kind == sensor::Kind::MOTION);
    REQUIRE(decoded.flags == sensor::READING_FLAG_SETTLING);
    REQUIRE(decoded.value_milli == 3000);
    REQUIRE(decoded.aux == sensor::AUX_NONE);
    REQUIRE(decoded.hlc == motion.hlc);

    // A v2 reading cut to v1 length is rejected.
    REQUIRE(!sensor::decode(encoded, sensor::READING_SIZE, decoded));

    const sensor::Reading sound{sensor::Kind::SOUND, 0, 1, 5000, 42, 37500};
    REQUIRE(sensor::encode(sound, encoded) == sensor::READING_SIZE_AUX);
    REQUIRE(sensor::decode(encoded, sensor::READING_SIZE_AUX, decoded));
    REQUIRE(decoded.kind == sensor::Kind::SOUND);
    REQUIRE(decoded.aux == 37500);

    REQUIRE(sensor::has_aux(sensor::Kind::MOTION));
    REQUIRE(sensor::has_aux(sensor::Kind::SOUND));
    REQUIRE(!sensor::has_aux(sensor::Kind::LIGHT));
    REQUIRE(std::string(sensor::kind_key(sensor::Kind::MOTION)) == "motion");
    REQUIRE(std::string(sensor::kind_key(sensor::Kind::SOUND)) == "sound");
    REQUIRE(std::string(sensor::kind_unit(sensor::Kind::SOUND)) == "events");
}

void test_format_value()
{
    char text[64];

    sensor::format_value(sensor::Kind::TEMPERATURE, 23202, 0, 0, text, sizeof(text));
    REQUIRE(std::string(text) == "23.202 degC");

    sensor::format_value(sensor::Kind::TEMPERATURE, -250, 0, 0, text, sizeof(text));
    REQUIRE(std::string(text) == "-0.250 degC");

    sensor::format_value(sensor::Kind::MOTION, 2000, 12, 0, text, sizeof(text));
    REQUIRE(std::string(text) == "2 events, last motion 12 s before the sample");

    sensor::format_value(sensor::Kind::MOTION, 0, sensor::AUX_NONE, 0, text, sizeof(text));
    REQUIRE(std::string(text) == "0 events, no motion since boot");

    sensor::format_value(sensor::Kind::MOTION, 0, 0, sensor::READING_FLAG_SETTLING, text, sizeof(text));
    REQUIRE(std::string(text) == "settling (PIR warm-up)");

    sensor::format_value(sensor::Kind::SOUND, 5000, 37540, 0, text, sizeof(text));
    REQUIRE(std::string(text) == "5 events, 37.5 % active");

    REQUIRE(sensor::format_value(sensor::Kind::SOUND, 5000, 37540, 0, text, 4) == 0);

    sensor::format_value(sensor::Kind::TEMPERATURE, 21500, 0, sensor::READING_FLAG_STALE, text, sizeof(text));
    REQUIRE(std::string(text) == "21.500 degC STALE (last good value, no good read lately)");

    sensor::format_value(sensor::Kind::TEMPERATURE, 0, 0, sensor::READING_FLAG_ERROR, text, sizeof(text));
    REQUIRE(std::string(text) == "ERROR (no good temp read since boot)");
}

void test_keys()
{
    char key[storage::KEY_MAX + 1];

    REQUIRE(sensor::latest_key(node::A, sensor::Kind::TEMPERATURE, key, sizeof(key)) == 8);
    REQUIRE(std::string(key) == "s/A/temp");

    REQUIRE(sensor::history_key(node::D, sensor::Kind::LIGHT, 7, key, sizeof(key)) > 0);
    REQUIRE(std::string(key) == "h/D/light/" + std::to_string(7 % config::SENSOR_HISTORY_SLOTS));

    REQUIRE(sensor::latest_key(node::B, sensor::Kind::HUMIDITY, key, 4) == 0);

    REQUIRE(sensor::kind_for_node(node::A) == sensor::Kind::LIGHT);
    REQUIRE(sensor::kind_for_node(node::B) == sensor::Kind::MOTION);
    REQUIRE(sensor::kind_for_node(node::C) == sensor::Kind::SOUND);
    REQUIRE(sensor::kind_for_node(node::D) == sensor::Kind::TEMPERATURE);
}

void test_simulated_sources_are_plausible()
{
    struct Range {
        sensor::Kind kind;
        std::int32_t min;
        std::int32_t max;
        std::int32_t max_step; // per 10 s sample
    };

    const Range ranges[] = {
        {sensor::Kind::TEMPERATURE, 15000, 30000, 1500},
        {sensor::Kind::HUMIDITY, 25000, 75000, 6000},
        {sensor::Kind::PRESSURE, 1000000, 1025000, 2000},
        {sensor::Kind::LIGHT, 0, 700000, 120000},
    };

    for (const Range& range : ranges) {
        sensor::SimulatedSource source(range.kind, 42);
        REQUIRE(source.begin());

        std::int32_t previous = 0;

        // 24 h at the default 10 s period.
        for (std::uint64_t t = 0; t < 24ULL * 3600ULL * 1000ULL; t += 10000) {
            sensor::Sample sample{};
            REQUIRE(source.read(t, sample));
            const std::int32_t value = sample.value_milli;
            REQUIRE(value >= range.min && value <= range.max);

            if (t > 0) {
                REQUIRE(std::abs(value - previous) <= range.max_step);
            }

            previous = value;
        }
    }
}

void test_simulated_event_sources()
{
    for (sensor::Kind kind : {sensor::Kind::MOTION, sensor::Kind::SOUND}) {
        sensor::SimulatedSource source(kind, 42);
        REQUIRE(source.begin());

        std::uint32_t windows_with_events = 0;
        bool saw_motion = false;

        // 24 h at a 30 s period.
        for (std::uint64_t t = 0; t < 24ULL * 3600ULL * 1000ULL; t += 30000) {
            sensor::Sample sample{};
            REQUIRE(source.read(t, sample));
            REQUIRE(sample.flags == 0);
            REQUIRE(sample.value_milli % 1000 == 0);
            REQUIRE(sample.value_milli >= 0 && sample.value_milli <= 6000);

            if (sample.value_milli > 0) {
                ++windows_with_events;
            }

            if (kind == sensor::Kind::MOTION) {
                if (sample.value_milli > 0) {
                    saw_motion = true;
                    REQUIRE(sample.aux >= 0 && sample.aux <= 30);
                }

                REQUIRE(saw_motion ? sample.aux >= 0 : sample.aux == sensor::AUX_NONE);
            } else {
                REQUIRE(sample.aux >= 0 && sample.aux <= 100000);
                REQUIRE((sample.value_milli == 0) == (sample.aux == 0));
            }
        }

        // Some quiet windows, some busy ones.
        REQUIRE(windows_with_events > 2880 / 10);
        REQUIRE(windows_with_events < 2880 * 9 / 10);
    }
}

} // namespace

int main()
{
    test_codec_roundtrip();
    test_codec_event_kinds();
    test_format_value();
    test_keys();
    test_simulated_sources_are_plausible();
    test_simulated_event_sources();

    std::cout << "Sensor tests: PASS\n";
    return 0;
}

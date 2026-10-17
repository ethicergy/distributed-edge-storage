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

    std::uint8_t encoded[sensor::READING_SIZE];
    REQUIRE(sensor::encode(reading, encoded));

    // Big-endian layout (see sensor.h).
    REQUIRE(encoded[0] == 0x01);
    REQUIRE(encoded[1] == 3);
    REQUIRE(encoded[2] == 0x01);
    REQUIRE(encoded[3] == 0x00);
    REQUIRE(encoded[4] == 0x00 && encoded[5] == 0x01 && encoded[6] == 0xE2 && encoded[7] == 0x40);
    REQUIRE(encoded[12] == 0x01 && encoded[19] == 0x08);

    sensor::Reading decoded{};
    REQUIRE(sensor::decode(encoded, sizeof(encoded), decoded));
    REQUIRE(decoded.kind == reading.kind);
    REQUIRE(decoded.flags == reading.flags);
    REQUIRE(decoded.sequence == reading.sequence);
    REQUIRE(decoded.value_milli == reading.value_milli);
    REQUIRE(decoded.hlc == reading.hlc);

    // Rejections.
    REQUIRE(!sensor::decode(encoded, sizeof(encoded) - 1, decoded));
    encoded[0] = 0x02;
    REQUIRE(!sensor::decode(encoded, sizeof(encoded), decoded));
    encoded[0] = 0x01;
    encoded[1] = 9;
    REQUIRE(!sensor::decode(encoded, sizeof(encoded), decoded));

    // Arbitrary text is not mistaken for a reading.
    const char* text = "hello, this is text!";
    REQUIRE(std::strlen(text) == sensor::READING_SIZE);
    REQUIRE(!sensor::decode(reinterpret_cast<const std::uint8_t*>(text), 20, decoded));
}

void test_keys()
{
    char key[storage::KEY_MAX + 1];

    REQUIRE(sensor::latest_key(node::A, sensor::Kind::TEMPERATURE, key, sizeof(key)) == 8);
    REQUIRE(std::string(key) == "s/A/temp");

    REQUIRE(sensor::history_key(node::D, sensor::Kind::LIGHT, 7, key, sizeof(key)) > 0);
    REQUIRE(std::string(key) == "h/D/light/" + std::to_string(7 % config::SENSOR_HISTORY_SLOTS));

    REQUIRE(sensor::latest_key(node::B, sensor::Kind::HUMIDITY, key, 4) == 0);

    REQUIRE(sensor::kind_for_node(node::A) == sensor::Kind::TEMPERATURE);
    REQUIRE(sensor::kind_for_node(node::B) == sensor::Kind::HUMIDITY);
    REQUIRE(sensor::kind_for_node(node::C) == sensor::Kind::PRESSURE);
    REQUIRE(sensor::kind_for_node(node::D) == sensor::Kind::LIGHT);
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
            std::int32_t value = 0;
            REQUIRE(source.read(t, value));
            REQUIRE(value >= range.min && value <= range.max);

            if (t > 0) {
                REQUIRE(std::abs(value - previous) <= range.max_step);
            }

            previous = value;
        }
    }
}

} // namespace

int main()
{
    test_codec_roundtrip();
    test_keys();
    test_simulated_sources_are_plausible();

    std::cout << "Sensor tests: PASS\n";
    return 0;
}

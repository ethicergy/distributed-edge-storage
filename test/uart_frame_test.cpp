/*
 * UART framing: CRC-8 check value, encode/decode, resynchronisation
 * after noise, bad CRC and bogus lengths, and the shared test vector
 * that pi/tests/test_framing.py checks against the Python side.
 */

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "packet_builder.h"
#include "test_support.h"
#include "uart_frame.h"

namespace {

std::vector<std::uint8_t> frame_for(const packet::Buffer& packet)
{
    std::uint8_t out[uart::MAX_FRAME];
    std::size_t length = 0;
    REQUIRE(uart::encode(packet.data, packet.length, out, sizeof(out), length));
    return std::vector<std::uint8_t>(out, out + length);
}

packet::Buffer sample_get(std::uint32_t request_id, const char* key)
{
    packet::Buffer buffer{};
    const packet::Route route{request_id, node::A, node::A, node::A, 0, 0};

    REQUIRE(packet::build_request(
        protocol::MessageType::GET_REQ,
        route,
        protocol::KeyView{reinterpret_cast<const std::uint8_t*>(key), static_cast<std::uint8_t>(std::strlen(key))},
        protocol::ValueView{nullptr, 0},
        buffer
    ));

    return buffer;
}

std::string hex(const std::vector<std::uint8_t>& bytes)
{
    std::string out;
    char digits[3];

    for (std::uint8_t byte : bytes) {
        std::snprintf(digits, sizeof(digits), "%02x", byte);
        out += digits;
    }

    return out;
}

std::vector<std::vector<std::uint8_t>> decode_all(uart::Decoder& decoder, const std::vector<std::uint8_t>& bytes, bool bytewise)
{
    std::vector<std::vector<std::uint8_t>> frames;

    auto drain = [&] {
        while (decoder.next()) {
            frames.emplace_back(decoder.envelope(), decoder.envelope() + decoder.envelope_length());
        }
    };

    if (bytewise) {
        for (std::uint8_t byte : bytes) {
            decoder.push(byte);
            drain();
        }
    } else {
        decoder.push(bytes.data(), bytes.size());
        drain();
    }

    return frames;
}

void test_crc8_check_value()
{
    const char* check = "123456789";
    REQUIRE(uart::crc8(reinterpret_cast<const std::uint8_t*>(check), 9) == 0xF4);
    REQUIRE(uart::crc8(nullptr, 0) == 0x00);
}

void test_shared_vector()
{
    // Must match pi/tests/test_framing.py::SHARED_VECTOR.
    const packet::Buffer packet = sample_get(0x01020304, "s/A/temp");
    const std::string encoded = hex(frame_for(packet));

    // AA 55 | len 0x1f | header (22) | key_len 08 "s/A/temp" | crc
    const std::string expected =
        "aa551f"
        "01030016010203040101010000000000000000000009"
        "08732f412f74656d70"
        "5d";

    std::printf("shared vector: %s\n", encoded.c_str());

    REQUIRE(encoded == expected);
    REQUIRE(encoded.size() == 2 * (4 + packet.length));
}

void test_roundtrip_and_noise()
{
    const packet::Buffer a = sample_get(1, "alpha");
    const packet::Buffer b = sample_get(2, "beta-key");

    std::vector<std::uint8_t> stream = {0x00, 0xAA, 0x13, 0x55, 0xAA, 0xAA};
    const std::vector<std::uint8_t> fa = frame_for(a);
    const std::vector<std::uint8_t> fb = frame_for(b);

    stream.insert(stream.end(), fa.begin(), fa.end());
    stream.push_back(0x42);
    stream.insert(stream.end(), fb.begin(), fb.end());

    for (bool bytewise : {false, true}) {
        uart::Decoder decoder;
        const auto frames = decode_all(decoder, stream, bytewise);

        REQUIRE(frames.size() == 2);
        REQUIRE(frames[0] == std::vector<std::uint8_t>(a.data, a.data + a.length));
        REQUIRE(frames[1] == std::vector<std::uint8_t>(b.data, b.data + b.length));
        REQUIRE(decoder.stats().frames == 2);
        REQUIRE(decoder.stats().discarded_bytes > 0);
    }
}

void test_bad_crc_resync()
{
    const packet::Buffer a = sample_get(7, "corrupt-me");
    const packet::Buffer b = sample_get(8, "survivor");

    std::vector<std::uint8_t> fa = frame_for(a);
    fa[10] ^= 0x01;

    const std::vector<std::uint8_t> fb = frame_for(b);

    std::vector<std::uint8_t> stream = fa;
    stream.insert(stream.end(), fb.begin(), fb.end());

    uart::Decoder decoder;
    const auto frames = decode_all(decoder, stream, true);

    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0] == std::vector<std::uint8_t>(b.data, b.data + b.length));
    REQUIRE(decoder.stats().crc_errors == 1);
}

void test_frame_hidden_behind_bogus_length()
{
    // Noise announces a 240-byte envelope; a real short frame follows.
    const packet::Buffer real = sample_get(9, "hidden");
    const std::vector<std::uint8_t> fr = frame_for(real);

    std::vector<std::uint8_t> stream = {0xAA, 0x55, 240};
    stream.insert(stream.end(), fr.begin(), fr.end());

    uart::Decoder decoder;
    auto frames = decode_all(decoder, stream, false);

    // Not enough bytes for the bogus frame yet: nothing decoded...
    REQUIRE(frames.empty());

    // ...until the line goes idle and the partial frame is abandoned.
    REQUIRE(decoder.abandon_partial());
    REQUIRE(decoder.next());
    REQUIRE(std::vector<std::uint8_t>(decoder.envelope(), decoder.envelope() + decoder.envelope_length()) ==
            std::vector<std::uint8_t>(real.data, real.data + real.length));

    // Invalid lengths are skipped immediately.
    uart::Decoder strict;
    std::vector<std::uint8_t> bad = {0xAA, 0x55, 3, 0xAA, 0x55, 251};
    bad.insert(bad.end(), fr.begin(), fr.end());
    frames = decode_all(strict, bad, true);
    REQUIRE(frames.size() == 1);
    REQUIRE(strict.stats().length_errors == 2);
}

void test_encode_limits()
{
    std::uint8_t envelope[uart::MAX_ENVELOPE + 1] = {};
    std::uint8_t out[uart::MAX_FRAME + 4];
    std::size_t length = 0;

    REQUIRE(!uart::encode(envelope, uart::MIN_ENVELOPE - 1, out, sizeof(out), length));
    REQUIRE(!uart::encode(envelope, uart::MAX_ENVELOPE + 1, out, sizeof(out), length));
    REQUIRE(uart::encode(envelope, uart::MAX_ENVELOPE, out, sizeof(out), length));
    REQUIRE(length == uart::MAX_FRAME);
    REQUIRE(!uart::encode(envelope, uart::MAX_ENVELOPE, out, uart::MAX_FRAME - 1, length));
}

} // namespace

int main()
{
    test_crc8_check_value();
    test_shared_vector();
    test_roundtrip_and_noise();
    test_bad_crc_resync();
    test_frame_hidden_behind_bogus_length();
    test_encode_limits();

    std::cout << "UART frame tests: PASS\n";
    return 0;
}

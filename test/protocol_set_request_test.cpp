#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "protocol.h"

using namespace protocol;

int main() {
    const std::uint8_t key[] = {
        't', 'e', 'm', 'p'
    };

    const std::uint8_t value[] = {
        '3', '1', '.', '5'
    };

    SetRequestPayload original{
        {key, 4},
        {value, 4}
    };

    std::uint8_t buffer[MAX_PAYLOAD_SIZE]{};
    std::size_t bytes_written = 0;

    Result result = serialize_set_request(
        original,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 10);

    const std::uint8_t expected[] = {
        0x04,
        't', 'e', 'm', 'p',
        0x04,
        '3', '1', '.', '5'
    };

    assert(std::memcmp(buffer, expected, 10) == 0);

    SetRequestPayload decoded{};

    result = deserialize_set_request(
        buffer,
        bytes_written,
        decoded
    );

    assert(result == Result::OK);

    assert(decoded.key.len == 4);
    assert(decoded.value.len == 4);

    assert(std::memcmp(decoded.key.data, key, 4) == 0);
    assert(std::memcmp(decoded.value.data, value, 4) == 0);


        // Output buffer too small.
    std::uint8_t small_buffer[9]{};
    bytes_written = 0;

    result = serialize_set_request(
        original,
        small_buffer,
        sizeof(small_buffer),
        bytes_written
    );

    assert(result == Result::BUFFER_TOO_SMALL);
    assert(bytes_written == 0);

    // Null output buffer.
    bytes_written = 0;

    result = serialize_set_request(
        original,
        nullptr,
        MAX_PAYLOAD_SIZE,
        bytes_written
    );

    assert(result == Result::INVALID_ARGUMENT);
    assert(bytes_written == 0);

    // Oversized key.
    std::uint8_t large_key[KEY_MAX + 1]{};

    SetRequestPayload oversized_key{
        {large_key, KEY_MAX + 1},
        {value, 4}
    };

    bytes_written = 0;

    result = serialize_set_request(
        oversized_key,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::INVALID_PAYLOAD);

    // Oversized value.
    std::uint8_t large_value[VALUE_MAX + 1]{};

    SetRequestPayload oversized_value{
        {key, 4},
        {large_value, VALUE_MAX + 1}
    };

    bytes_written = 0;

    result = serialize_set_request(
        oversized_value,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::INVALID_PAYLOAD);

    // Truncated payload: key length says 4, but only 3 key bytes exist.
    const std::uint8_t truncated_key[] = {
        0x04, 't', 'e', 'm'
    };

    SetRequestPayload invalid_payload{};

    result = deserialize_set_request(
        truncated_key,
        sizeof(truncated_key),
        invalid_payload
    );

    assert(result == Result::INVALID_PAYLOAD);

    // Incorrect value length.
    const std::uint8_t wrong_value_length[] = {
        0x04,
        't', 'e', 'm', 'p',
        0x05,
        '3', '1', '.', '5'
    };

    result = deserialize_set_request(
        wrong_value_length,
        sizeof(wrong_value_length),
        invalid_payload
    );

    assert(result == Result::INVALID_PAYLOAD);

    // Trailing bytes.
    const std::uint8_t trailing_bytes[] = {
        0x04,
        't', 'e', 'm', 'p',
        0x04,
        '3', '1', '.', '5',
        0xFF
    };

    result = deserialize_set_request(
        trailing_bytes,
        sizeof(trailing_bytes),
        invalid_payload
    );

    assert(result == Result::INVALID_PAYLOAD);

    std::cout << "SET_REQ validation tests: PASS\n";

    
    std::cout << "SET_REQ serialization test: PASS\n";
    std::cout << "SET_REQ round-trip test: PASS\n";

    return 0;
}
#include "protocol.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace protocol;

    // ------------------------------------------------------------
    // Normal REPL_REQ round-trip
    // ------------------------------------------------------------

    const std::uint8_t key_data[] = {'t', 'e', 'm', 'p'};
    const std::uint8_t value_data[] = {'4', '2'};

    ReplicationRequestPayload original{
        {key_data, 4},
        {value_data, 2}
    };

    std::uint8_t buffer[MAX_PAYLOAD_SIZE] = {};
    std::size_t bytes_written = 0;

    Result result = serialize_replication_request(
        original,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 8);

    // Expected wire payload:
    // 04 "temp" 02 "42"
    assert(buffer[0] == 4);
    assert(std::memcmp(buffer + 1, "temp", 4) == 0);
    assert(buffer[5] == 2);
    assert(std::memcmp(buffer + 6, "42", 2) == 0);

    ReplicationRequestPayload decoded{};

    result = deserialize_replication_request(
        buffer,
        bytes_written,
        decoded
    );

    assert(result == Result::OK);

    assert(decoded.key.len == 4);
    assert(std::memcmp(decoded.key.data, key_data, 4) == 0);

    assert(decoded.value.len == 2);
    assert(std::memcmp(decoded.value.data, value_data, 2) == 0);

    std::cout << "REPL_REQ round-trip test: PASS\n";


    // ------------------------------------------------------------
    // Empty value
    // ------------------------------------------------------------

    const std::uint8_t delete_key[] = {'d', 'e', 'l'};

    ReplicationRequestPayload empty_value{
        {delete_key, 3},
        {nullptr, 0}
    };

    result = serialize_replication_request(
        empty_value,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 5);

    // 03 "del" 00
    assert(buffer[0] == 3);
    assert(std::memcmp(buffer + 1, "del", 3) == 0);
    assert(buffer[4] == 0);

    decoded = {};

    result = deserialize_replication_request(
        buffer,
        bytes_written,
        decoded
    );

    assert(result == Result::OK);
    assert(decoded.key.len == 3);
    assert(std::memcmp(decoded.key.data, delete_key, 3) == 0);
    assert(decoded.value.len == 0);

    std::cout << "REPL_REQ empty-value test: PASS\n";


    // ------------------------------------------------------------
    // Oversized key
    // ------------------------------------------------------------

    std::uint8_t large_key[KEY_MAX + 1] = {};

    ReplicationRequestPayload bad_key{
        {large_key, static_cast<std::uint8_t>(KEY_MAX + 1)},
        {value_data, 2}
    };

    result = serialize_replication_request(
        bad_key,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::INVALID_PAYLOAD);

    std::cout << "REPL_REQ oversized-key test: PASS\n";


    // ------------------------------------------------------------
    // Oversized value
    // ------------------------------------------------------------

    std::uint8_t large_value[VALUE_MAX + 1] = {};

    ReplicationRequestPayload bad_value{
        {key_data, 4},
        {large_value, static_cast<std::uint8_t>(VALUE_MAX + 1)}
    };

    result = serialize_replication_request(
        bad_value,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::INVALID_PAYLOAD);

    std::cout << "REPL_REQ oversized-value test: PASS\n";


    // ------------------------------------------------------------
    // Buffer too small
    // ------------------------------------------------------------

    result = serialize_replication_request(
        original,
        buffer,
        7,
        bytes_written
    );

    assert(result == Result::BUFFER_TOO_SMALL);

    std::cout << "REPL_REQ buffer-size test: PASS\n";


    // ------------------------------------------------------------
    // Malformed payload: trailing byte
    // ------------------------------------------------------------

    std::uint8_t malformed[] = {
        4, 't', 'e', 'm', 'p',
        2, '4', '2',
        0x99
    };

    result = deserialize_replication_request(
        malformed,
        sizeof(malformed),
        decoded
    );

    assert(result == Result::INVALID_PAYLOAD);

    std::cout << "REPL_REQ malformed-payload test: PASS\n";


    return 0;
}
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "storage_record.h"

using namespace storage;
using namespace storage::record;

static void test_live_record()
{
    const uint8_t key[] = {'t', 'e', 'm', 'p'};
    const uint8_t value[] = {'4', '2'};

    Record input{};
    input.state = State::LIVE;
    input.version = Version{
        0x0102030405060708ULL,
        node::B
    };
    input.sequence = 0x11223344;
    input.key = KeyView{key, sizeof(key)};
    input.value = ValueView{value, sizeof(value)};

    uint8_t buffer[MAX_RECORD_SIZE]{};
    size_t bytes_written = 0;

    Result result = serialize(
        input,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);
    assert(bytes_written == 31);

    // Exact expected serialized bytes.
    const uint8_t expected[] = {
        0x52, 0x43,             // magic "RC"
        0x01,                   // format version
        0x00,                   // flags
        0x00, 0x1F,             // record length = 31
        0x04,                   // key length
        0x00, 0x02,             // value length
        0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, // HLC
        0x02,                   // version origin = B
        0x11, 0x22, 0x33, 0x44, // sequence
        0x74, 0x65, 0x6D, 0x70, // "temp"
        0x34, 0x32,             // "42"
        0xAB, 0xFB,             // CRC16
        0xA5                    // commit marker
    };

    assert(sizeof(expected) == bytes_written);
    assert(std::memcmp(buffer, expected, sizeof(expected)) == 0);

    // Deserialize.
    Record output{};
    result = deserialize(buffer, bytes_written, output);

    assert(result == Result::OK);

    assert(output.state == State::LIVE);
    assert(output.version.hlc == 0x0102030405060708ULL);
    assert(output.version.origin == node::B);
    assert(output.sequence == 0x11223344);

    assert(output.key.len == sizeof(key));
    assert(std::memcmp(output.key.data, key, sizeof(key)) == 0);

    assert(output.value.len == sizeof(value));
    assert(std::memcmp(output.value.data, value, sizeof(value)) == 0);

    std::cout << "Live record serialize/deserialize: PASS\n";
}

static void test_corruption_detection()
{
    const uint8_t key[] = {'t', 'e', 'm', 'p'};
    const uint8_t value[] = {'4', '2'};

    Record input{};
    input.state = State::LIVE;
    input.version = Version{100, node::A};
    input.sequence = 1;
    input.key = KeyView{key, sizeof(key)};
    input.value = ValueView{value, sizeof(value)};

    uint8_t buffer[MAX_RECORD_SIZE]{};
    size_t bytes_written = 0;

    assert(
        serialize(
            input,
            buffer,
            sizeof(buffer),
            bytes_written
        ) == Result::OK
    );

    Record output{};

    // 1. Corrupt commit marker.
    buffer[bytes_written - 1] ^= 0x01;

    assert(
        deserialize(buffer, bytes_written, output)
        == Result::CORRUPTED
    );

    // Restore.
    buffer[bytes_written - 1] = 0xA5;

    // 2. Corrupt key byte.
    buffer[22] ^= 0x01;

    assert(
        deserialize(buffer, bytes_written, output)
        == Result::CORRUPTED
    );

    // Restore.
    buffer[22] ^= 0x01;

    // 3. Corrupt record length.
    buffer[5] ^= 0x01;

    assert(
        deserialize(buffer, bytes_written, output)
        == Result::CORRUPTED
    );

    std::cout << "Corruption detection: PASS\n";
}

static void test_tombstone_record()
{
    const uint8_t key[] = {'t', 'e', 'm', 'p'};

    Record input{};
    input.state = State::TOMBSTONE;
    input.version = Version{
        100,
        node::A
    };
    input.sequence = 1;
    input.key = KeyView{key, sizeof(key)};
    input.value = ValueView{nullptr, 0};

    uint8_t buffer[MAX_RECORD_SIZE]{};
    size_t bytes_written = 0;

    Result result = serialize(
        input,
        buffer,
        sizeof(buffer),
        bytes_written
    );

    assert(result == Result::OK);

    Record output{};

    result = deserialize(
        buffer,
        bytes_written,
        output
    );

    assert(result == Result::OK);
    assert(output.state == State::TOMBSTONE);
    assert(output.version.hlc == 100);
    assert(output.version.origin == node::A);
    assert(output.sequence == 1);

    assert(output.key.len == sizeof(key));
    assert(std::memcmp(output.key.data, key, sizeof(key)) == 0);

    assert(output.value.len == 0);
    

    std::cout << "Tombstone record: PASS\n";
}

static void test_invalid_tombstone()
{
    const uint8_t key[] = {'t', 'e', 'm', 'p'};
    const uint8_t value[] = {'x'};

    Record input{};
    input.state = State::TOMBSTONE;
    input.version = Version{100, node::A};
    input.sequence = 1;
    input.key = KeyView{key, sizeof(key)};
    input.value = ValueView{value, sizeof(value)};

    uint8_t buffer[MAX_RECORD_SIZE]{};
    size_t bytes_written = 0;

    assert(
        serialize(
            input,
            buffer,
            sizeof(buffer),
            bytes_written
        ) == Result::INVALID_ARGUMENT
    );

    std::cout << "Invalid tombstone rejection: PASS\n";
}

int main()
{
    test_live_record();
    test_corruption_detection();
    test_tombstone_record();
    test_invalid_tombstone();

    std::cout << "\nStorage record tests: ALL PASS\n";
    return 0;
}
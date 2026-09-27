#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "storage_metadata.h"

using namespace storage;
using namespace storage::metadata;

static void test_superblock()
{
    Superblock input{
        0x11223344,
        Bank::A,
        0x55667788,
        0x99AABBCC,
        0x1234
    };

    uint8_t buffer[SUPERBLOCK_SIZE]{};

    assert(
        serialize_superblock(
            input,
            buffer,
            sizeof(buffer)
        ) == Result::OK
    );

    const uint8_t expected[] = {
        0x53, 0x42,             // magic = "SB"
        0x01,                   // format version
        0x00,                   // reserved
        0x11, 0x22, 0x33, 0x44, // generation
        0x00,                   // active bank = A
        0x55, 0x66, 0x77, 0x88, // active bank generation
        0x00, 0x00,             // reserved
        0x99, 0xAA, 0xBB, 0xCC, // last known sequence
        0x12, 0x34,             // stored record count
        0xC1, 0xBC,             // CRC16
        0xA5                    // commit marker
    };

    assert(sizeof(expected) == 24);

    assert(
        std::memcmp(
            buffer,
            expected,
            sizeof(expected)
        ) == 0
    );

    Superblock output{};

    assert(
        deserialize_superblock(
            buffer,
            sizeof(buffer),
            output
        ) == Result::OK
    );

    assert(output.generation == input.generation);
    assert(output.active_bank == input.active_bank);

    assert(
        output.active_bank_generation ==
        input.active_bank_generation
    );

    assert(
        output.last_known_sequence ==
        input.last_known_sequence
    );

    assert(
        output.stored_record_count ==
        input.stored_record_count
    );

    // Corrupt a field covered by the CRC.
    buffer[4] ^= 0x01;

    assert(
        deserialize_superblock(
            buffer,
            sizeof(buffer),
            output
        ) == Result::CORRUPTED
    );

    std::cout << "Superblock codec: PASS\n";
}

static void test_bank_header()
{
    BankHeader input{
        BankState::OPEN,
        0x01020304,
        BANK_RECORD_OFFSET,
        BANK_RECORD_AREA_END,
        0x0033,
        0x0123
    };

    uint8_t buffer[BANK_HEADER_SIZE]{};

    assert(
        serialize_bank_header(
            input,
            buffer,
            sizeof(buffer)
        ) == Result::OK
    );

    const uint8_t expected[] = {
        0x42, 0x48,             // magic = "BH"
        0x01,                   // format version
        0x00,                   // OPEN
        0x01, 0x02, 0x03, 0x04, // generation
        0x00, 0x20,             // first record offset
        0x3E, 0xE0,             // record area end
        0x00, 0x33,             // record count
        0x01, 0x23,             // records end offset
        0x1F, 0x90,             // CRC16
        0xA5                    // commit marker
    };

    assert(sizeof(expected) == 19);

    assert(
        std::memcmp(
            buffer,
            expected,
            sizeof(expected)
        ) == 0
    );

    BankHeader output{};

    assert(
        deserialize_bank_header(
            buffer,
            sizeof(buffer),
            output
        ) == Result::OK
    );

    assert(output.state == input.state);
    assert(output.generation == input.generation);

    assert(
        output.first_record_offset ==
        input.first_record_offset
    );

    assert(
        output.record_area_end ==
        input.record_area_end
    );

    assert(output.record_count == input.record_count);

    assert(
        output.records_end_offset ==
        input.records_end_offset
    );

    // Invalid commit marker.
    buffer[18] ^= 0x01;

    assert(
        deserialize_bank_header(
            buffer,
            sizeof(buffer),
            output
        ) == Result::CORRUPTED
    );

    std::cout << "Bank header codec: PASS\n";
}

static void test_bank_footer()
{
    BankFooter input{
        0x01020304,
        0x0033,
        0x0123
    };

    uint8_t buffer[BANK_FOOTER_SIZE]{};

    assert(
        serialize_bank_footer(
            input,
            buffer,
            sizeof(buffer)
        ) == Result::OK
    );

    const uint8_t expected[] = {
        0x46, 0x54,             // magic = "FT"
        0x01,                   // format version
        0x01,                   // SEALED
        0x01, 0x02, 0x03, 0x04, // generation
        0x00, 0x33,             // record count
        0x01, 0x23,             // records end offset
        0x4D, 0x5D,             // CRC16
        0xA5                    // commit marker
    };

    assert(sizeof(expected) == 15);

    assert(
        std::memcmp(
            buffer,
            expected,
            sizeof(expected)
        ) == 0
    );

    BankFooter output{};

    assert(
        deserialize_bank_footer(
            buffer,
            sizeof(buffer),
            output
        ) == Result::OK
    );

    assert(output.generation == input.generation);
    assert(output.record_count == input.record_count);

    assert(
        output.records_end_offset ==
        input.records_end_offset
    );

    // Corrupt reserved byte.
    buffer[31] = 0x01;

    assert(
        deserialize_bank_footer(
            buffer,
            sizeof(buffer),
            output
        ) == Result::CORRUPTED
    );

    std::cout << "Bank footer codec: PASS\n";
}

static void test_invalid_arguments()
{
    uint8_t buffer[SUPERBLOCK_SIZE]{};

    Superblock sb{
        1,
        Bank::A,
        1,
        1,
        1
    };

    assert(
        serialize_superblock(
            sb,
            nullptr,
            sizeof(buffer)
        ) == Result::BUFFER_TOO_SMALL
    );

    assert(
        serialize_superblock(
            sb,
            buffer,
            10
        ) == Result::BUFFER_TOO_SMALL
    );

    sb.active_bank = static_cast<Bank>(0xFF);

    assert(
        serialize_superblock(
            sb,
            buffer,
            sizeof(buffer)
        ) == Result::INVALID_ARGUMENT
    );

    std::cout << "Invalid metadata arguments: PASS\n";
}

int main()
{
    test_superblock();
    test_bank_header();
    test_bank_footer();
    test_invalid_arguments();

    std::cout << "\nStorage metadata tests: ALL PASS\n";

    return 0;
}
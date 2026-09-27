#pragma once

#include <cstddef>
#include <cstdint>

#include "storage_result.h"

namespace storage::metadata {

constexpr size_t SUPERBLOCK_SIZE = 64;
constexpr size_t BANK_HEADER_SIZE = 32;
constexpr size_t BANK_FOOTER_SIZE = 32;

constexpr uint16_t SUPERBLOCK_MAGIC = 0x5342; // "SB"
constexpr uint16_t BANK_HEADER_MAGIC = 0x4248; // "BH"
constexpr uint16_t BANK_FOOTER_MAGIC = 0x4654; // "FT"

constexpr uint8_t FORMAT_VERSION = 0x01;
constexpr uint8_t COMMIT_MARKER = 0xA5;

constexpr uint16_t BANK_A_BASE = 0x0200;
constexpr uint16_t BANK_B_BASE = 0x4100;

constexpr uint16_t BANK_SIZE = 16128;
constexpr uint16_t BANK_RECORD_OFFSET = 0x0020;
constexpr uint16_t BANK_RECORD_AREA_END = 0x3EE0;

constexpr uint16_t SUPERBLOCK_SLOT_0 = 0x0000;
constexpr uint16_t SUPERBLOCK_SLOT_1 = 0x0040;

enum class Bank : uint8_t {
    A = 0x00,
    B = 0x01
};

enum class BankState : uint8_t {
    OPEN = 0x00,
    SEALED = 0x01
};

struct Superblock {
    uint32_t generation;
    Bank active_bank;
    uint32_t active_bank_generation;
    uint32_t last_known_sequence;
    uint16_t stored_record_count;
};

struct BankHeader {
    BankState state;
    uint32_t generation;
    uint16_t first_record_offset;
    uint16_t record_area_end;
    uint16_t record_count;
    uint16_t records_end_offset;
};

struct BankFooter {
    uint32_t generation;
    uint16_t record_count;
    uint16_t records_end_offset;
};

Result serialize_superblock(
    const Superblock& value,
    uint8_t* output,
    size_t output_capacity
);

Result deserialize_superblock(
    const uint8_t* data,
    size_t length,
    Superblock& value
);

Result serialize_bank_header(
    const BankHeader& value,
    uint8_t* output,
    size_t output_capacity
);

Result deserialize_bank_header(
    const uint8_t* data,
    size_t length,
    BankHeader& value
);

Result serialize_bank_footer(
    const BankFooter& value,
    uint8_t* output,
    size_t output_capacity
);

Result deserialize_bank_footer(
    const uint8_t* data,
    size_t length,
    BankFooter& value
);

bool is_valid_bank(Bank bank);
bool is_valid_bank_state(BankState state);

}
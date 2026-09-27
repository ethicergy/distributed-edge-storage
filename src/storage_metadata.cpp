#include "storage_metadata.h"

#include <cstring>

#include "storage_record.h"

namespace storage::metadata {
namespace {

constexpr size_t SUPERBLOCK_CRC_OFFSET = 21;
constexpr size_t SUPERBLOCK_COMMIT_OFFSET = 23;

constexpr size_t HEADER_CRC_OFFSET = 16;
constexpr size_t HEADER_COMMIT_OFFSET = 18;

constexpr size_t FOOTER_CRC_OFFSET = 12;
constexpr size_t FOOTER_COMMIT_OFFSET = 14;

void write_u16_be(uint8_t* data, size_t offset, uint16_t value)
{
    data[offset] =
        static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[offset + 1] =
        static_cast<uint8_t>(value & 0xFFu);
}

void write_u32_be(uint8_t* data, size_t offset, uint32_t value)
{
    data[offset] =
        static_cast<uint8_t>((value >> 24) & 0xFFu);
    data[offset + 1] =
        static_cast<uint8_t>((value >> 16) & 0xFFu);
    data[offset + 2] =
        static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[offset + 3] =
        static_cast<uint8_t>(value & 0xFFu);
}

uint16_t read_u16_be(const uint8_t* data, size_t offset)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) |
        static_cast<uint16_t>(data[offset + 1])
    );
}

uint32_t read_u32_be(const uint8_t* data, size_t offset)
{
    return
        (static_cast<uint32_t>(data[offset]) << 24) |
        (static_cast<uint32_t>(data[offset + 1]) << 16) |
        (static_cast<uint32_t>(data[offset + 2]) << 8) |
        static_cast<uint32_t>(data[offset + 3]);
}

bool reserved_zero(
    const uint8_t* data,
    size_t offset,
    size_t length
)
{
    for (size_t i = 0; i < length; ++i) {
        if (data[offset + i] != 0) {
            return false;
        }
    }

    return true;
}

} // namespace

bool is_valid_bank(Bank bank)
{
    return bank == Bank::A || bank == Bank::B;
}

bool is_valid_bank_state(BankState state)
{
    return state == BankState::OPEN ||
           state == BankState::SEALED;
}

Result serialize_superblock(
    const Superblock& value,
    uint8_t* output,
    size_t output_capacity
)
{
    if (output == nullptr ||
        output_capacity < SUPERBLOCK_SIZE) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (!is_valid_bank(value.active_bank)) {
        return Result::INVALID_ARGUMENT;
    }

    std::memset(output, 0, SUPERBLOCK_SIZE);

    write_u16_be(output, 0, SUPERBLOCK_MAGIC);

    output[2] = FORMAT_VERSION;
    output[3] = 0;

    write_u32_be(output, 4, value.generation);

    output[8] = static_cast<uint8_t>(value.active_bank);

    write_u32_be(
        output,
        9,
        value.active_bank_generation
    );

    write_u16_be(output, 13, 0);

    write_u32_be(
        output,
        15,
        value.last_known_sequence
    );

    write_u16_be(
        output,
        19,
        value.stored_record_count
    );

    const uint16_t crc =
        record::crc16(output, SUPERBLOCK_CRC_OFFSET);

    write_u16_be(output, SUPERBLOCK_CRC_OFFSET, crc);

    // Written last by the persistent layer in actual EEPROM operation.
    output[SUPERBLOCK_COMMIT_OFFSET] = COMMIT_MARKER;

    return Result::OK;
}

Result deserialize_superblock(
    const uint8_t* data,
    size_t length,
    Superblock& value
)
{
    value = Superblock{};

    if (data == nullptr || length != SUPERBLOCK_SIZE) {
        return Result::CORRUPTED;
    }

    if (read_u16_be(data, 0) != SUPERBLOCK_MAGIC) {
        return Result::CORRUPTED;
    }

    if (data[2] != FORMAT_VERSION) {
        return Result::CORRUPTED;
    }

    if (data[3] != 0 ||
        !reserved_zero(data, 24, 40)) {
        return Result::CORRUPTED;
    }

    if (!is_valid_bank(static_cast<Bank>(data[8]))) {
        return Result::CORRUPTED;
    }

    if (data[SUPERBLOCK_COMMIT_OFFSET] != COMMIT_MARKER) {
        return Result::CORRUPTED;
    }

    const uint16_t stored_crc =
        read_u16_be(data, SUPERBLOCK_CRC_OFFSET);

    const uint16_t calculated_crc =
        record::crc16(data, SUPERBLOCK_CRC_OFFSET);

    if (stored_crc != calculated_crc) {
        return Result::CORRUPTED;
    }

    value.generation = read_u32_be(data, 4);

    value.active_bank =
        static_cast<Bank>(data[8]);

    value.active_bank_generation =
        read_u32_be(data, 9);

    value.last_known_sequence =
        read_u32_be(data, 15);

    value.stored_record_count =
        read_u16_be(data, 19);

    return Result::OK;
}

Result serialize_bank_header(
    const BankHeader& value,
    uint8_t* output,
    size_t output_capacity
)
{
    if (output == nullptr ||
        output_capacity < BANK_HEADER_SIZE) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (!is_valid_bank_state(value.state)) {
        return Result::INVALID_ARGUMENT;
    }

    if (value.first_record_offset != BANK_RECORD_OFFSET ||
        value.record_area_end != BANK_RECORD_AREA_END) {
        return Result::INVALID_ARGUMENT;
    }

    if (value.records_end_offset < BANK_RECORD_OFFSET ||
        value.records_end_offset > BANK_RECORD_AREA_END) {
        return Result::INVALID_ARGUMENT;
    }

    std::memset(output, 0, BANK_HEADER_SIZE);

    write_u16_be(output, 0, BANK_HEADER_MAGIC);

    output[2] = FORMAT_VERSION;
    output[3] = static_cast<uint8_t>(value.state);

    write_u32_be(output, 4, value.generation);

    write_u16_be(
        output,
        8,
        value.first_record_offset
    );

    write_u16_be(
        output,
        10,
        value.record_area_end
    );

    write_u16_be(
        output,
        12,
        value.record_count
    );

    write_u16_be(
        output,
        14,
        value.records_end_offset
    );

    const uint16_t crc =
        record::crc16(output, HEADER_CRC_OFFSET);

    write_u16_be(output, HEADER_CRC_OFFSET, crc);

    output[HEADER_COMMIT_OFFSET] = COMMIT_MARKER;

    return Result::OK;
}

Result deserialize_bank_header(
    const uint8_t* data,
    size_t length,
    BankHeader& value
)
{
    value = BankHeader{};

    if (data == nullptr || length != BANK_HEADER_SIZE) {
        return Result::CORRUPTED;
    }

    if (read_u16_be(data, 0) != BANK_HEADER_MAGIC) {
        return Result::CORRUPTED;
    }

    if (data[2] != FORMAT_VERSION) {
        return Result::CORRUPTED;
    }

    if (!is_valid_bank_state(
            static_cast<BankState>(data[3]))) {
        return Result::CORRUPTED;
    }

    if (read_u16_be(data, 8) != BANK_RECORD_OFFSET ||
        read_u16_be(data, 10) != BANK_RECORD_AREA_END) {
        return Result::CORRUPTED;
    }

    if (data[HEADER_COMMIT_OFFSET] != COMMIT_MARKER) {
        return Result::CORRUPTED;
    }

    if (!reserved_zero(data, 19, 13)) {
        return Result::CORRUPTED;
    }

    const uint16_t stored_crc =
        read_u16_be(data, HEADER_CRC_OFFSET);

    const uint16_t calculated_crc =
        record::crc16(data, HEADER_CRC_OFFSET);

    if (stored_crc != calculated_crc) {
        return Result::CORRUPTED;
    }

    value.state =
        static_cast<BankState>(data[3]);

    value.generation =
        read_u32_be(data, 4);

    value.first_record_offset =
        read_u16_be(data, 8);

    value.record_area_end =
        read_u16_be(data, 10);

    value.record_count =
        read_u16_be(data, 12);

    value.records_end_offset =
        read_u16_be(data, 14);

    if (value.records_end_offset < BANK_RECORD_OFFSET ||
        value.records_end_offset > BANK_RECORD_AREA_END) {
        return Result::CORRUPTED;
    }

    return Result::OK;
}

Result serialize_bank_footer(
    const BankFooter& value,
    uint8_t* output,
    size_t output_capacity
)
{
    if (output == nullptr ||
        output_capacity < BANK_FOOTER_SIZE) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (value.records_end_offset < BANK_RECORD_OFFSET ||
        value.records_end_offset > BANK_RECORD_AREA_END) {
        return Result::INVALID_ARGUMENT;
    }

    std::memset(output, 0, BANK_FOOTER_SIZE);

    write_u16_be(output, 0, BANK_FOOTER_MAGIC);

    output[2] = FORMAT_VERSION;
    output[3] =
        static_cast<uint8_t>(BankState::SEALED);

    write_u32_be(
        output,
        4,
        value.generation
    );

    write_u16_be(
        output,
        8,
        value.record_count
    );

    write_u16_be(
        output,
        10,
        value.records_end_offset
    );

    const uint16_t crc =
        record::crc16(output, FOOTER_CRC_OFFSET);

    write_u16_be(output, FOOTER_CRC_OFFSET, crc);

    output[FOOTER_COMMIT_OFFSET] = COMMIT_MARKER;

    return Result::OK;
}

Result deserialize_bank_footer(
    const uint8_t* data,
    size_t length,
    BankFooter& value
)
{
    value = BankFooter{};

    if (data == nullptr || length != BANK_FOOTER_SIZE) {
        return Result::CORRUPTED;
    }

    if (read_u16_be(data, 0) != BANK_FOOTER_MAGIC) {
        return Result::CORRUPTED;
    }

    if (data[2] != FORMAT_VERSION) {
        return Result::CORRUPTED;
    }

    if (data[3] !=
        static_cast<uint8_t>(BankState::SEALED)) {
        return Result::CORRUPTED;
    }

    if (data[FOOTER_COMMIT_OFFSET] != COMMIT_MARKER) {
        return Result::CORRUPTED;
    }

    if (!reserved_zero(data, 15, 17)) {
        return Result::CORRUPTED;
    }

    const uint16_t stored_crc =
        read_u16_be(data, FOOTER_CRC_OFFSET);

    const uint16_t calculated_crc =
        record::crc16(data, FOOTER_CRC_OFFSET);

    if (stored_crc != calculated_crc) {
        return Result::CORRUPTED;
    }

    value.generation =
        read_u32_be(data, 4);

    value.record_count =
        read_u16_be(data, 8);

    value.records_end_offset =
        read_u16_be(data, 10);

    if (value.records_end_offset < BANK_RECORD_OFFSET ||
        value.records_end_offset > BANK_RECORD_AREA_END) {
        return Result::CORRUPTED;
    }

    return Result::OK;
}

} // namespace storage::metadata
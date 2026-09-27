#include "storage_record.h"
#include <cstring>



namespace storage::record {

namespace {

constexpr std::size_t OFFSET_MAGIC = 0;
constexpr std::size_t OFFSET_FORMAT_VERSION = 2;
constexpr std::size_t OFFSET_FLAGS = 3;
constexpr std::size_t OFFSET_RECORD_LEN = 4;
constexpr std::size_t OFFSET_KEY_LEN = 6;
constexpr std::size_t OFFSET_VALUE_LEN = 7;
constexpr std::size_t OFFSET_VERSION_HLC = 9;
constexpr std::size_t OFFSET_VERSION_ORIGIN = 17;
constexpr std::size_t OFFSET_SEQUENCE = 18;
constexpr std::size_t OFFSET_KEY = 22;

constexpr std::size_t MIN_RECORD_SIZE =
    HEADER_SIZE + CRC_SIZE + COMMIT_MARKER_SIZE;

constexpr std::uint8_t FLAGS_TOMBSTONE = 0x01;
constexpr std::uint8_t FLAGS_RESERVED_MASK = 0xFE;

void write_u16_be(
    std::uint8_t* out,
    std::uint16_t value
) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value & 0xFF);
}

void write_u32_be(
    std::uint8_t* out,
    std::uint32_t value
) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value & 0xFF);
}

void write_u64_be(
    std::uint8_t* out,
    std::uint64_t value
) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(
            value >> (56 - (i * 8))
        );
    }
}

std::uint16_t read_u16_be(
    const std::uint8_t* data
) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1])
    );
}

std::uint32_t read_u32_be(
    const std::uint8_t* data
) {
    return
        (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_u64_be(
    const std::uint8_t* data
) {
    std::uint64_t value = 0;

    for (std::size_t i = 0; i < 8; ++i) {
        value =
            (value << 8) |
            static_cast<std::uint64_t>(data[i]);
    }

    return value;
}

bool is_valid_state(State state) {
    return state == State::LIVE ||
           state == State::TOMBSTONE;
}
}

std::uint16_t crc16(
    const std::uint8_t* data,
    std::size_t length
) {
    std::uint16_t crc = 0xFFFF;

    for (std::size_t i = 0; i < length; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;

        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000u) != 0) {
                crc =
                    static_cast<std::uint16_t>(
                        (crc << 1) ^ 0x1021u
                    );
            } else {
                crc =
                    static_cast<std::uint16_t>(
                        crc << 1
                    );
            }
        }
    }

    return crc;
}
Result serialize(
    const Record& record,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
) {
    bytes_written = 0;

    if (out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (!is_valid_state(record.state)) {
        return Result::INVALID_ARGUMENT;
    }

    if (record.key.len > KEY_MAX) {
        return Result::KEY_TOO_LARGE;
    }

    if (record.value.len > VALUE_MAX) {
        return Result::VALUE_TOO_LARGE;
    }

    if (record.key.len > 0 && record.key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (record.value.len > 0 && record.value.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (record.state == State::TOMBSTONE &&
        record.value.len != 0) {
        return Result::INVALID_ARGUMENT;
    }

    const std::size_t total_length =
        HEADER_SIZE +
        record.key.len +
        record.value.len +
        CRC_SIZE +
        COMMIT_MARKER_SIZE;

    if (total_length > MAX_RECORD_SIZE) {
        return Result::INVALID_ARGUMENT;
    }

    if (out_capacity < total_length) {
        return Result::BUFFER_TOO_SMALL;
    }

    const std::uint8_t flags =
        (record.state == State::TOMBSTONE)
            ? FLAGS_TOMBSTONE
            : 0x00;

    write_u16_be(
        out + OFFSET_MAGIC,
        MAGIC
    );

    out[OFFSET_FORMAT_VERSION] = FORMAT_VERSION;
    out[OFFSET_FLAGS] = flags;

    write_u16_be(
        out + OFFSET_RECORD_LEN,
        static_cast<std::uint16_t>(total_length)
    );

    out[OFFSET_KEY_LEN] =
        static_cast<std::uint8_t>(record.key.len);

    write_u16_be(
        out + OFFSET_VALUE_LEN,
        static_cast<std::uint16_t>(record.value.len)
    );

    write_u64_be(
        out + OFFSET_VERSION_HLC,
        record.version.hlc
    );

    out[OFFSET_VERSION_ORIGIN] =
        record.version.origin;

    write_u32_be(
        out + OFFSET_SEQUENCE,
        record.sequence
    );

    if (record.key.len > 0) {
        std::memcpy(
            out + OFFSET_KEY,
            record.key.data,
            record.key.len
        );
    }

    const std::size_t value_offset =
        OFFSET_KEY + record.key.len;

    if (record.value.len > 0) {
        std::memcpy(
            out + value_offset,
            record.value.data,
            record.value.len
        );
    }

    const std::size_t crc_offset =
        value_offset + record.value.len;

    const std::uint16_t checksum =
        crc16(out, crc_offset);

    write_u16_be(
        out + crc_offset,
        checksum
    );

    out[crc_offset + CRC_SIZE] = COMMIT_MARKER;

    bytes_written = total_length;

    return Result::OK;
}

Result deserialize(
    const std::uint8_t* data,
    std::size_t record_len,
    Record& record
) {
    record = {};

    if (data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (record_len < MIN_RECORD_SIZE ||
        record_len > MAX_RECORD_SIZE) {
        return Result::CORRUPTED;
    }

    const std::uint16_t stored_magic =
        read_u16_be(data + OFFSET_MAGIC);

    if (stored_magic != MAGIC) {
        return Result::CORRUPTED;
    }

    if (data[OFFSET_FORMAT_VERSION] != FORMAT_VERSION) {
        return Result::CORRUPTED;
    }

    const std::uint8_t flags =
        data[OFFSET_FLAGS];

    if ((flags & FLAGS_RESERVED_MASK) != 0) {
        return Result::CORRUPTED;
    }

    const std::uint16_t encoded_length =
        read_u16_be(data + OFFSET_RECORD_LEN);

    if (encoded_length != record_len) {
        return Result::CORRUPTED;
    }

    const std::uint8_t key_len =
        data[OFFSET_KEY_LEN];

    if (key_len > KEY_MAX) {
        return Result::CORRUPTED;
    }

    const std::uint16_t value_len =
        read_u16_be(data + OFFSET_VALUE_LEN);

    if (value_len > VALUE_MAX) {
        return Result::CORRUPTED;
    }

    const bool tombstone =
        (flags & FLAGS_TOMBSTONE) != 0;

    if (tombstone && value_len != 0) {
        return Result::CORRUPTED;
    }

    const std::size_t expected_length =
        HEADER_SIZE +
        key_len +
        value_len +
        CRC_SIZE +
        COMMIT_MARKER_SIZE;

    if (expected_length != record_len) {
        return Result::CORRUPTED;
    }

    const std::size_t value_offset =
        OFFSET_KEY + key_len;

    const std::size_t crc_offset =
        value_offset + value_len;

    if (data[record_len - 1] != COMMIT_MARKER) {
        return Result::CORRUPTED;
    }

    const std::uint16_t stored_crc =
        read_u16_be(data + crc_offset);

    const std::uint16_t calculated_crc =
        crc16(data, crc_offset);

    if (stored_crc != calculated_crc) {
        return Result::CORRUPTED;
    }

    record.state =
        tombstone
            ? State::TOMBSTONE
            : State::LIVE;

    record.version.hlc =
        read_u64_be(data + OFFSET_VERSION_HLC);

    record.version.origin =
        data[OFFSET_VERSION_ORIGIN];

    record.sequence =
        read_u32_be(data + OFFSET_SEQUENCE);

    record.key = {
        data + OFFSET_KEY,
        key_len
    };

    record.value = {
        data + value_offset,
        value_len
    };

    return Result::OK;
}

} // namespace storage::record
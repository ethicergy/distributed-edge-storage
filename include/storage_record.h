#pragma once

#include <cstddef>
#include <cstdint>

#include "storage.h"

namespace storage::record {

constexpr std::uint16_t MAGIC = 0x5243;
constexpr std::uint8_t FORMAT_VERSION = 0x01;
constexpr std::uint8_t COMMIT_MARKER = 0xA5;

constexpr std::size_t HEADER_SIZE = 22;
constexpr std::size_t CRC_SIZE = 2;
constexpr std::size_t COMMIT_MARKER_SIZE = 1;

constexpr std::size_t MAX_RECORD_SIZE =
    HEADER_SIZE +
    KEY_MAX +
    VALUE_MAX +
    CRC_SIZE +
    COMMIT_MARKER_SIZE;

static_assert(
    MAX_RECORD_SIZE == 249,
    "Persistent record size must remain 249 bytes"
);

struct Record {
    State state;

    Version version;

    std::uint32_t sequence;

    KeyView key;
    ValueView value;
};

Result serialize(
    const Record& record,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& bytes_written
);

Result deserialize(
    const std::uint8_t* data,
    std::size_t record_len,
    Record& record
);

std::uint16_t crc16(
    const std::uint8_t* data,
    std::size_t length
);

} // namespace storage::record
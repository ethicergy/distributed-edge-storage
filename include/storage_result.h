#pragma once

#include <cstdint>

namespace storage {

enum class Result : uint8_t {
    OK = 0x00,
    NOT_FOUND,
    STALE_VERSION,
    NOT_INITIALIZED,
    INVALID_ARGUMENT,
    KEY_TOO_LARGE,
    VALUE_TOO_LARGE,
    BUFFER_TOO_SMALL,
    INDEX_FULL,
    NO_SPACE,
    STORAGE_ERROR,
    CORRUPTED
};

} // namespace storage
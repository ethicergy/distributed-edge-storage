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

// Why the last failed storage operation failed, for the console. Result
// folds several of these into STORAGE_ERROR (and kv:: folds them all),
// so a full log and a dead I2C bus would otherwise look the same.
enum class Fault : uint8_t {
    NONE = 0x00,
    I2C_ERROR,        // EEPROM did not ACK / bus error
    WRITE_TIMEOUT,    // EEPROM write cycle not finished in time
    OUT_OF_RANGE,     // address outside the EEPROM
    VERIFY_MISMATCH,  // read-back after a write differed
    NO_SPACE,         // log full (compaction pending)
    INDEX_FULL,       // all MAX_LOGICAL_KEYS slots used
    CORRUPTED         // on-EEPROM structure inconsistent
};

constexpr const char* to_string(Fault fault)
{
    switch (fault) {
        case Fault::NONE: return "no fault recorded";
        case Fault::I2C_ERROR: return "I2C_ERROR";
        case Fault::WRITE_TIMEOUT: return "WRITE_TIMEOUT";
        case Fault::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case Fault::VERIFY_MISMATCH: return "VERIFY_MISMATCH";
        case Fault::NO_SPACE: return "NO_SPACE";
        case Fault::INDEX_FULL: return "INDEX_FULL";
        case Fault::CORRUPTED: return "CORRUPTED";
    }
    return "?";
}

} // namespace storage
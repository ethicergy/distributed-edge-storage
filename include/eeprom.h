#pragma once

#include <cstddef>
#include <cstdint>

namespace eeprom {

constexpr std::uint8_t I2C_ADDRESS = 0x50;

constexpr std::uint16_t SIZE_BYTES = 32768;
constexpr std::uint16_t LAST_ADDRESS = SIZE_BYTES - 1;

constexpr std::size_t PAGE_SIZE = 64;
constexpr std::uint32_t I2C_FREQUENCY_HZ = 400000;
constexpr std::uint32_t WRITE_CYCLE_TIMEOUT_MS = 10;
constexpr std::size_t READ_CHUNK_SIZE = 64;

enum class Result : std::uint8_t {
    OK = 0x00,
    INVALID_ARGUMENT,
    OUT_OF_RANGE,
    NOT_INITIALIZED,
    I2C_ERROR,
    WRITE_TIMEOUT
};

class AT24C256 {
public:
    Result begin(
        std::uint8_t sda_pin,
        std::uint8_t scl_pin
    );

    Result read(
        std::uint16_t address,
        std::uint8_t* buffer,
        std::size_t length
    );

    Result write(
        std::uint16_t address,
        const std::uint8_t* data,
        std::size_t length
    );

    bool ready() const;

private:
    bool initialized_ = false;
};

} // namespace eeprom
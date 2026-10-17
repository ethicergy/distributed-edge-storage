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

/*
 * Byte-addressable persistent memory as seen by the Storage Manager.
 *
 * The firmware uses AT24C256 below. Host tests and the cluster
 * simulator use an in-RAM implementation (sim/fake_eeprom.h) that
 * can also inject power loss in the middle of a write.
 */
class Device {
public:
    virtual ~Device() = default;

    virtual Result read(
        std::uint16_t address,
        std::uint8_t* buffer,
        std::size_t length
    ) = 0;

    virtual Result write(
        std::uint16_t address,
        const std::uint8_t* data,
        std::size_t length
    ) = 0;

    virtual bool ready() const = 0;
};

class AT24C256 final : public Device {
public:
    Result begin(
        std::uint8_t sda_pin,
        std::uint8_t scl_pin
    );

    Result read(
        std::uint16_t address,
        std::uint8_t* buffer,
        std::size_t length
    ) override;

    Result write(
        std::uint16_t address,
        const std::uint8_t* data,
        std::size_t length
    ) override;

    bool ready() const override;

private:
    bool initialized_ = false;
};

} // namespace eeprom
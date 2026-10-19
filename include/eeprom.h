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

    // Wear since boot: completed page writes (one write cycle each).
    std::uint32_t total_page_writes() const;
    std::uint32_t hottest_page_writes(std::uint16_t& page) const;

private:
    bool initialized_ = false;
    std::uint32_t page_writes_[SIZE_BYTES / PAGE_SIZE] = {};
};

// The modules also answer at 0x58: the chip's ID/security block
// (0x58-0x5F with A2..A0). The driver must only ever address the data
// array at 0x50-0x57; a write to the other block may lock it permanently.
static_assert(
    (I2C_ADDRESS & 0xF8) == 0x50,
    "AT24C256 data array is at 0x50-0x57; never address the 0x58 ID block"
);

} // namespace eeprom
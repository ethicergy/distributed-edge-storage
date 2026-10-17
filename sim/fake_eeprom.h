#pragma once

/*
 * Host-side AT24C256 stand-in.
 *
 * - 32 KB of RAM, erased to 0xFF like a factory-fresh chip.
 * - Power-loss injection: arm_power_loss(n) lets exactly n more
 *   bytes reach the array; the write that crosses the budget is
 *   torn at that byte and every access fails until power_cycle().
 *   This models a brown-out in the middle of an I2C page write.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "eeprom.h"

namespace sim {

class FakeEeprom final : public eeprom::Device {
public:
    FakeEeprom()
    {
        erase();
    }

    void erase()
    {
        std::memset(memory_, 0xFF, sizeof(memory_));
    }

    eeprom::Result read(
        std::uint16_t address,
        std::uint8_t* buffer,
        std::size_t length
    ) override
    {
        if (!powered_) {
            return eeprom::Result::I2C_ERROR;
        }

        if (length > 0 && buffer == nullptr) {
            return eeprom::Result::INVALID_ARGUMENT;
        }

        if (static_cast<std::size_t>(address) + length >
            eeprom::SIZE_BYTES) {
            return eeprom::Result::OUT_OF_RANGE;
        }

        if (length > 0) {
            std::memcpy(buffer, memory_ + address, length);
        }

        ++read_calls_;
        return eeprom::Result::OK;
    }

    eeprom::Result write(
        std::uint16_t address,
        const std::uint8_t* data,
        std::size_t length
    ) override
    {
        if (!powered_) {
            return eeprom::Result::I2C_ERROR;
        }

        if (length > 0 && data == nullptr) {
            return eeprom::Result::INVALID_ARGUMENT;
        }

        if (static_cast<std::size_t>(address) + length >
            eeprom::SIZE_BYTES) {
            return eeprom::Result::OUT_OF_RANGE;
        }

        if (length == 0) {
            return eeprom::Result::OK;
        }

        // One write cycle per page touched, like the driver's one I2C
        // page-write transaction per page segment.
        for (std::size_t page = address / eeprom::PAGE_SIZE;
             page <= (address + length - 1) / eeprom::PAGE_SIZE;
             ++page) {
            ++page_cycles_[page];
        }

        for (std::size_t i = 0; i < length; ++i) {
            if (budget_armed_) {
                if (budget_ == 0) {
                    powered_ = false;
                    return eeprom::Result::I2C_ERROR;
                }

                --budget_;
            }

            memory_[address + i] = data[i];
            ++bytes_written_;
        }

        ++write_calls_;
        return eeprom::Result::OK;
    }

    bool ready() const override
    {
        return powered_;
    }

    void arm_power_loss(std::size_t bytes_until_loss)
    {
        budget_armed_ = true;
        budget_ = bytes_until_loss;
    }

    void power_cycle()
    {
        powered_ = true;
        budget_armed_ = false;
        budget_ = 0;
    }

    bool powered() const { return powered_; }

    std::uint64_t bytes_written() const { return bytes_written_; }
    std::uint64_t write_calls() const { return write_calls_; }
    std::uint64_t read_calls() const { return read_calls_; }

    std::uint8_t* raw() { return memory_; }

    // Highest number of write cycles any page has seen (wear).
    std::uint64_t max_page_cycles() const
    {
        std::uint64_t highest = 0;
        for (std::uint64_t cycles : page_cycles_) {
            highest = cycles > highest ? cycles : highest;
        }
        return highest;
    }

    static constexpr std::size_t PAGE_COUNT = eeprom::SIZE_BYTES / eeprom::PAGE_SIZE;

    std::uint64_t page_cycles(std::size_t page) const
    {
        return page < PAGE_COUNT ? page_cycles_[page] : 0;
    }

    void copy_from(const FakeEeprom& other)
    {
        std::memcpy(memory_, other.memory_, sizeof(memory_));
    }

private:
    std::uint8_t memory_[eeprom::SIZE_BYTES];
    std::uint64_t page_cycles_[eeprom::SIZE_BYTES / eeprom::PAGE_SIZE] = {};
    bool powered_ = true;
    bool budget_armed_ = false;
    std::size_t budget_ = 0;
    std::uint64_t bytes_written_ = 0;
    std::uint64_t write_calls_ = 0;
    std::uint64_t read_calls_ = 0;
};

} // namespace sim

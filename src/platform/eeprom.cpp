#include "eeprom.h"

#include <Arduino.h>
#include <Wire.h>

#include "eeprom_poll.h"
#include "i2c_bus.h"

namespace eeprom {

namespace {

bool valid_range(
    std::uint16_t address,
    std::size_t length
) {
    if (length == 0) {
        return address <= SIZE_BYTES;
    }

    if (address > LAST_ADDRESS) {
        return false;
    }

    return length <=
        (static_cast<std::size_t>(SIZE_BYTES) - address);
}

// Logic and its tests: eeprom_poll.h, test/eeprom_poll_test.cpp.
Result wait_for_write_cycle() {
    return eeprom::wait_for_write_cycle(
        [] { return static_cast<std::uint32_t>(millis()); },
        [] {
            Wire.beginTransmission(I2C_ADDRESS);
            return Wire.endTransmission(true) == 0;
        },
        [] { delay(1); }
    );
}

} // namespace

Result AT24C256::begin(
    std::uint8_t sda_pin,
    std::uint8_t scl_pin
) {
    initialized_ = false;

    i2c_bus::Lock lock;

    if (!Wire.begin(sda_pin, scl_pin)) {
        return Result::I2C_ERROR;
    }

    Wire.setClock(I2C_FREQUENCY_HZ);

    Wire.beginTransmission(I2C_ADDRESS);

    const std::uint8_t error =
        Wire.endTransmission(true);

    if (error != 0) {
        return Result::I2C_ERROR;
    }

    initialized_ = true;

    return Result::OK;
}

Result AT24C256::read(
    std::uint16_t address,
    std::uint8_t* buffer,
    std::size_t length
) {
    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (length > 0 && buffer == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (!valid_range(address, length)) {
        return Result::OUT_OF_RANGE;
    }

    if (length == 0) {
        return Result::OK;
    }

    // Held for the whole transfer, write cycles included (i2c_bus.h).
    i2c_bus::Lock lock;

    std::size_t remaining = length;
    std::uint16_t current_address = address;
    std::uint8_t* current_buffer = buffer;

    while (remaining > 0) {
        const std::size_t chunk =
            (remaining < READ_CHUNK_SIZE)
                ? remaining
                : READ_CHUNK_SIZE;

        Wire.beginTransmission(I2C_ADDRESS);

        Wire.write(
            static_cast<std::uint8_t>(
                current_address >> 8
            )
        );

        Wire.write(
            static_cast<std::uint8_t>(
                current_address & 0xFF
            )
        );

        const std::uint8_t address_result =
            Wire.endTransmission(false);

        if (address_result != 0) {
            return Result::I2C_ERROR;
        }

        const std::size_t received =
    Wire.requestFrom(
        static_cast<std::uint8_t>(I2C_ADDRESS),
        static_cast<std::size_t>(chunk),
        true
    );

        if (received != chunk) {
            return Result::I2C_ERROR;
        }

        for (std::size_t i = 0; i < chunk; ++i) {
            if (!Wire.available()) {
                return Result::I2C_ERROR;
            }

            current_buffer[i] = Wire.read();
        }

        current_address =
            static_cast<std::uint16_t>(
                current_address + chunk
            );

        current_buffer += chunk;
        remaining -= chunk;
    }

    return Result::OK;
}

Result AT24C256::write(
    std::uint16_t address,
    const std::uint8_t* data,
    std::size_t length
) {
    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (length > 0 && data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (!valid_range(address, length)) {
        return Result::OUT_OF_RANGE;
    }

    if (length == 0) {
        return Result::OK;
    }

    // Held for the whole transfer, write cycles included (i2c_bus.h).
    i2c_bus::Lock lock;

    std::size_t remaining = length;
    std::uint16_t current_address = address;
    const std::uint8_t* current_data = data;

    while (remaining > 0) {
        const std::size_t page_offset =
            current_address % PAGE_SIZE;

        const std::size_t bytes_to_page_end =
            PAGE_SIZE - page_offset;

        const std::size_t chunk =
            (remaining < bytes_to_page_end)
                ? remaining
                : bytes_to_page_end;

        Wire.beginTransmission(I2C_ADDRESS);

        Wire.write(
            static_cast<std::uint8_t>(
                current_address >> 8
            )
        );

        Wire.write(
            static_cast<std::uint8_t>(
                current_address & 0xFF
            )
        );

        const std::size_t written =
            Wire.write(current_data, chunk);

        if (written != chunk) {
            return Result::I2C_ERROR;
        }

        const std::uint8_t write_result =
            Wire.endTransmission(true);

        if (write_result != 0) {
            return Result::I2C_ERROR;
        }

        const Result wait_result =
            wait_for_write_cycle();

        if (wait_result != Result::OK) {
            return wait_result;
        }

        ++page_writes_[current_address / PAGE_SIZE];

        current_address =
            static_cast<std::uint16_t>(
                current_address + chunk
            );

        current_data += chunk;
        remaining -= chunk;
    }

    return Result::OK;
}

bool AT24C256::ready() const {
    return initialized_;
}

std::uint32_t AT24C256::total_page_writes() const {
    std::uint32_t total = 0;

    for (std::uint32_t writes : page_writes_) {
        total += writes;
    }

    return total;
}

std::uint32_t AT24C256::hottest_page_writes(std::uint16_t& page) const {
    std::uint32_t highest = 0;
    page = 0;

    for (std::uint16_t i = 0; i < SIZE_BYTES / PAGE_SIZE; ++i) {
        if (page_writes_[i] > highest) {
            highest = page_writes_[i];
            page = i;
        }
    }

    return highest;
}

} // namespace eeprom
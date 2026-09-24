#include <Arduino.h>
#include <cstring>

#include "eeprom.h"

namespace {

constexpr std::uint8_t SDA_PIN = 8;
constexpr std::uint8_t SCL_PIN = 9;

constexpr std::uint16_t CROSS_PAGE_ADDRESS = 0x003F;
constexpr std::uint16_t END_TEST_ADDRESS = 0x7FFE;

eeprom::AT24C256 memory;

void print_result(
    const char* operation,
    eeprom::Result result
) {
    Serial.print(operation);
    Serial.print(": ");

    if (result == eeprom::Result::OK) {
        Serial.println("OK");
    } else {
        Serial.print("FAIL (result=");
        Serial.print(static_cast<int>(result));
        Serial.println(")");
    }
}

bool expect_result(
    const char* operation,
    eeprom::Result actual,
    eeprom::Result expected
) {
    Serial.print(operation);
    Serial.print(": ");

    if (actual == expected) {
        Serial.println("PASS");
        return true;
    }

    Serial.print("FAIL (expected=");
    Serial.print(static_cast<int>(expected));
    Serial.print(", actual=");
    Serial.print(static_cast<int>(actual));
    Serial.println(")");

    return false;
}

bool buffers_equal(
    const std::uint8_t* lhs,
    const std::uint8_t* rhs,
    std::size_t length
) {
    return std::memcmp(lhs, rhs, length) == 0;
}

bool test_cross_page_write() {
    std::uint8_t original[4]{};
    std::uint8_t test_data[2] = {
        0xA5,
        0x5A
    };
    std::uint8_t readback[2]{};

    eeprom::Result result = memory.read(
        0x003E,
        original,
        sizeof(original)
    );

    if (!expect_result(
            "Read bytes around page boundary",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    result = memory.write(
        CROSS_PAGE_ADDRESS,
        test_data,
        sizeof(test_data)
    );

    if (!expect_result(
            "2-byte write across 64-byte page boundary",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    result = memory.read(
        CROSS_PAGE_ADDRESS,
        readback,
        sizeof(readback)
    );

    if (!expect_result(
            "Read back cross-page write",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    if (!buffers_equal(
            test_data,
            readback,
            sizeof(test_data))) {
        Serial.println("Cross-page data verification: FAIL");
        return false;
    }

    Serial.println("Cross-page data verification: PASS");

    // Restore the original two bytes at 0x003F-0x0040.
    result = memory.write(
        CROSS_PAGE_ADDRESS,
        &original[1],
        2
    );

    if (!expect_result(
            "Restore page-boundary bytes",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    return true;
}

bool test_multiple_page_write() {
    constexpr std::size_t TEST_LENGTH = 128;
    constexpr std::uint16_t TEST_ADDRESS = 0x0180;

    std::uint8_t original[TEST_LENGTH]{};
    std::uint8_t pattern[TEST_LENGTH]{};
    std::uint8_t readback[TEST_LENGTH]{};

    for (std::size_t i = 0; i < TEST_LENGTH; ++i) {
        pattern[i] = static_cast<std::uint8_t>(
            0x20 + (i & 0x3F)
        );
    }

    eeprom::Result result = memory.read(
        TEST_ADDRESS,
        original,
        TEST_LENGTH
    );

    if (!expect_result(
            "Read original multi-page test region",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    result = memory.write(
        TEST_ADDRESS,
        pattern,
        TEST_LENGTH
    );

    if (!expect_result(
            "128-byte multi-page write",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    result = memory.read(
        TEST_ADDRESS,
        readback,
        TEST_LENGTH
    );

    if (!expect_result(
            "Read back 128-byte write",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    if (!buffers_equal(
            pattern,
            readback,
            TEST_LENGTH)) {
        Serial.println("128-byte data verification: FAIL");
        return false;
    }

    Serial.println("128-byte data verification: PASS");

    result = memory.write(
        TEST_ADDRESS,
        original,
        TEST_LENGTH
    );

    if (!expect_result(
            "Restore multi-page test region",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    return true;
}

bool test_end_of_device() {
    std::uint8_t original[2]{};
    const std::uint8_t test_data[2] = {
        0x3C,
        0xC3
    };
    std::uint8_t readback[2]{};

    eeprom::Result result = memory.read(
        END_TEST_ADDRESS,
        original,
        sizeof(original)
    );

    if (!expect_result(
            "Read final two EEPROM bytes",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    result = memory.write(
        END_TEST_ADDRESS,
        test_data,
        sizeof(test_data)
    );

    if (!expect_result(
            "Write exactly through 0x7FFF",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    result = memory.read(
        END_TEST_ADDRESS,
        readback,
        sizeof(readback)
    );

    if (!expect_result(
            "Read final two EEPROM bytes back",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    if (!buffers_equal(
            test_data,
            readback,
            sizeof(test_data))) {
        Serial.println("End-of-device data verification: FAIL");
        return false;
    }

    Serial.println("End-of-device data verification: PASS");

    result = memory.write(
        END_TEST_ADDRESS,
        original,
        sizeof(original)
    );

    if (!expect_result(
            "Restore final two EEPROM bytes",
            result,
            eeprom::Result::OK)) {
        return false;
    }

    return true;
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("PHASE 2C-1D - AT24C256 DRIVER TEST");
    Serial.println("========================================");

    bool all_passed = true;

    // Driver must reject access before initialization.
    {
        std::uint8_t value = 0;

        all_passed &= expect_result(
            "Read before begin",
            memory.read(0x0000, &value, 1),
            eeprom::Result::NOT_INITIALIZED
        );

        all_passed &= expect_result(
            "Write before begin",
            memory.write(0x0000, &value, 1),
            eeprom::Result::NOT_INITIALIZED
        );
    }

    print_result(
        "EEPROM begin",
        memory.begin(SDA_PIN, SCL_PIN)
    );

    if (!memory.ready()) {
        Serial.println("EEPROM ready state: FAIL");
        all_passed = false;
    } else {
        Serial.println("EEPROM ready state: PASS");
    }

    // Null buffers.
    all_passed &= expect_result(
        "Read with null buffer",
        memory.read(0x0000, nullptr, 1),
        eeprom::Result::INVALID_ARGUMENT
    );

    all_passed &= expect_result(
        "Write with null buffer",
        memory.write(0x0000, nullptr, 1),
        eeprom::Result::INVALID_ARGUMENT
    );

    // Invalid address/range cases.
    std::uint8_t value = 0x55;

    all_passed &= expect_result(
        "Write starting at 0x7FFF with length 2",
        memory.write(0x7FFF, &value, 2),
        eeprom::Result::OUT_OF_RANGE
    );

    all_passed &= expect_result(
        "Read starting at 0x7FFF with length 2",
        memory.read(0x7FFF, &value, 2),
        eeprom::Result::OUT_OF_RANGE
    );

    all_passed &= expect_result(
        "Write starting at 0x8000",
        memory.write(0x8000, &value, 1),
        eeprom::Result::OUT_OF_RANGE
    );

    // Empty operations should be harmless.
    all_passed &= expect_result(
        "Zero-length write at 0x8000",
        memory.write(0x8000, nullptr, 0),
        eeprom::Result::OK
    );

    all_passed &= test_cross_page_write();
    all_passed &= test_multiple_page_write();
    all_passed &= test_end_of_device();

    Serial.println();

    if (all_passed) {
        Serial.println("========================================");
        Serial.println("PHASE 2C-1D RESULT: PASS");
        Serial.println("AT24C256 driver validation complete.");
        Serial.println("========================================");
    } else {
        Serial.println("========================================");
        Serial.println("PHASE 2C-1D RESULT: FAIL");
        Serial.println("Do not proceed to record implementation.");
        Serial.println("========================================");
    }
}

void loop() {
    delay(1000);
}
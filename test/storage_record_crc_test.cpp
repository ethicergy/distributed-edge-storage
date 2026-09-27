#include <cassert>
#include <cstdint>
#include <iostream>

#include "storage_record.h"

using namespace storage::record;

int main() {
    const std::uint8_t data[] = {
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39
    };

    // CRC-16/CCITT-FALSE of ASCII "123456789".
    constexpr std::uint16_t EXPECTED = 0x29B1;

    const std::uint16_t actual =
        crc16(data, sizeof(data));

    assert(actual == EXPECTED);

    // Empty input with the selected initial value.
    assert(crc16(nullptr, 0) == 0xFFFF);

    std::cout << "Storage record CRC tests: PASS\n";

    return 0;
}
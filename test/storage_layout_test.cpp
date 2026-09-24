#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "storage.h"

using namespace storage;

int main() {
    // The storage design reserves exactly 48 bytes per index entry.
    //
    // IndexEntry is private, so the compile-time static_assert in
    // storage.h already verifies its exact size. This test verifies
    // the overall baseline index budget through the public constant.

    constexpr std::size_t EXPECTED_INDEX_BYTES =
        MAX_LOGICAL_KEYS * 48;

    static_assert(
        EXPECTED_INDEX_BYTES == 3072,
        "Unexpected RAM index size"
    );

    // The object must be constructible with its default state.
    StorageManager storage;

    // begin() is intentionally not called yet because EEPROM-backed
    // initialization belongs to the later implementation phase.
    //
    // The object existing successfully is the only runtime check here.
    (void)storage;

    assert(MAX_LOGICAL_KEYS == 64);
    assert(KEY_MAX == 32);
    assert(VALUE_MAX == 192);

    std::cout << "Storage index capacity: 64 entries\n";
    std::cout << "Storage index budget: 3072 bytes\n";
    std::cout << "Storage layout tests: PASS\n";

    return 0;
}
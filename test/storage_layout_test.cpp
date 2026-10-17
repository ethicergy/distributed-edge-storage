#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "../sim/fake_eeprom.h"
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

    // The object is constructed on a device and is not ready until
    // begin() succeeds (behaviour is covered by storage_manager_test).
    sim::FakeEeprom device;
    StorageManager storage(device);

    assert(!storage.ready());
    assert(StorageManager::slot_capacity() == MAX_LOGICAL_KEYS);

    assert(MAX_LOGICAL_KEYS == 64);
    assert(KEY_MAX == 32);
    assert(VALUE_MAX == 192);

    std::cout << "Storage index capacity: 64 entries\n";
    std::cout << "Storage index budget: 3072 bytes\n";
    std::cout << "Storage layout tests: PASS\n";

    return 0;
}
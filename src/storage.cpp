#include "storage.h"

#include <cstring>

namespace storage {

int compare_versions(
    const Version& lhs,
    const Version& rhs
) {
    if (lhs.hlc < rhs.hlc) {
        return -1;
    }

    if (lhs.hlc > rhs.hlc) {
        return 1;
    }

    if (lhs.origin < rhs.origin) {
        return -1;
    }

    if (lhs.origin > rhs.origin) {
        return 1;
    }

    return 0;
}

bool StorageManager::keys_equal(
    const IndexEntry& entry,
    KeyView key
) {
    if (entry.key_len != key.len) {
        return false;
    }

    if (key.len == 0) {
        return true;
    }

    return std::memcmp(
        entry.key,
        key.data,
        key.len
    ) == 0;
}

int StorageManager::find_index_entry(KeyView key) const {
    for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
        const IndexEntry& entry = index_[i];

        if (!entry.valid) {
            continue;
        }

        if (keys_equal(entry, key)) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int StorageManager::find_free_index_entry() const {
    for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
        if (!index_[i].valid) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void StorageManager::update_index_entry(
    std::size_t index,
    KeyView key,
    State state,
    std::uint16_t value_len,
    std::uint16_t record_address,
    Version version
) {
    IndexEntry& entry = index_[index];

    entry = IndexEntry{};

    if (key.len > 0) {
        std::memcpy(
            entry.key,
            key.data,
            key.len
        );
    }

    entry.key_len = static_cast<std::uint8_t>(key.len);
    entry.state = state;
    entry.value_len = value_len;
    entry.record_address = record_address;
    entry.version_hlc = version.hlc;
    entry.version_origin = version.origin;
    entry.valid = 1;
}

} // namespace storage
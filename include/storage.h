#pragma once

#include <cstddef>
#include <cstdint>

#include "node_id.h"

namespace storage {

constexpr std::size_t KEY_MAX = 32;
constexpr std::size_t VALUE_MAX = 192;
constexpr std::size_t MAX_LOGICAL_KEYS = 64;

using Hlc = std::uint64_t;

struct ByteView {
    const std::uint8_t* data;
    std::size_t len;
};

using KeyView = ByteView;
using ValueView = ByteView;

struct Version {
    Hlc hlc;
    node::Id origin;
};

enum class State : std::uint8_t {
    LIVE = 0x00,
    TOMBSTONE = 0x01
};

enum class Result : std::uint8_t {
    OK = 0x00,
    NOT_FOUND,
    STALE_VERSION,
    NOT_INITIALIZED,
    INVALID_ARGUMENT,
    KEY_TOO_LARGE,
    VALUE_TOO_LARGE,
    BUFFER_TOO_SMALL,
    INDEX_FULL,
    NO_SPACE,
    STORAGE_ERROR,
    CORRUPTED
};

class StorageManager {
public:
    Result begin();

    Result apply(
        KeyView key,
        ValueView value,
        Version version,
        State state
    );

    Result set(
        KeyView key,
        ValueView value,
        Version version
    );

    Result remove(
        KeyView key,
        Version version
    );

    Result get(
        KeyView key,
        std::uint8_t* value_out,
        std::size_t value_capacity,
        std::size_t& value_len
    );

    bool ready() const;


    private:
    struct IndexEntry {
        Hlc version_hlc;

        std::uint8_t key[KEY_MAX];

        std::uint16_t value_len;
        std::uint16_t record_address;

        std::uint8_t key_len;
        State state;
        node::Id version_origin;
        std::uint8_t valid;
    };

    static_assert(
        sizeof(IndexEntry) == 48,
        "IndexEntry layout must remain 48 bytes"
    );

    IndexEntry index_[MAX_LOGICAL_KEYS]{};
    bool initialized_ = false;

        int find_index_entry(KeyView key) const;

    int find_free_index_entry() const;

    void update_index_entry(
        std::size_t index,
        KeyView key,
        State state,
        std::uint16_t value_len,
        std::uint16_t record_address,
        Version version
    );

    static bool keys_equal(
        const IndexEntry& entry,
        KeyView key
    );

};

int compare_versions(
    const Version& lhs,
    const Version& rhs
);

} // namespace storage
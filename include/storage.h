#pragma once

#include <cstddef>
#include <cstdint>

#include "node_id.h"
#include "eeprom.h"
#include "storage_metadata.h"
#include "storage_result.h"


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

    Result max_persisted_hlc(Hlc& out) const;
    
    Result compact();  

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


        Result initialize_empty_store();

    Result write_committed_block(
        uint16_t address,
        const uint8_t* data,
        size_t length,
        size_t commit_offset
    );




    Result write_bank_header(
        metadata::Bank bank,
        metadata::BankState state,
        uint32_t generation,
        uint16_t record_count,
        uint16_t records_end_offset
    );

    Result write_bank_footer(
        metadata::Bank bank,
        uint32_t generation,
        uint16_t record_count,
        uint16_t records_end_offset
    );

    Result write_superblock(
        uint32_t generation,
        metadata::Bank active_bank,
        uint32_t active_bank_generation,
        uint32_t last_known_sequence,
        uint16_t stored_record_count
    );




    Result clear_range(
        uint16_t address,
        size_t length
    );

    Result map_eeprom_result(eeprom::Result result) const;

    eeprom::AT24C256 eeprom_;

    metadata::Bank active_bank_ = metadata::Bank::A;
    uint32_t superblock_generation_ = 0;
    uint32_t active_bank_generation_ = 0;
    uint32_t next_sequence_ = 1;
uint16_t append_address_ = 0;
    uint16_t record_count_ = 0;


    Result append_record(
        KeyView key,
        ValueView value,
        Version version,
        State state,
        uint16_t& record_address
    );

    Result append_record_to_bank(
        metadata::Bank bank,
        uint16_t& append_address,
        uint32_t& next_sequence,
        KeyView key,
        ValueView value,
        Version version,
        State state,
        uint16_t& record_address
    );
    
    Result read_value(
    const IndexEntry& entry,
    uint8_t* value_out,
    size_t value_capacity,
    size_t& value_len
);
        Result recover_active_bank();

    bool erased_tail(
        uint16_t address
    );



};

int compare_versions(
    const Version& lhs,
    const Version& rhs
);

} // namespace storage
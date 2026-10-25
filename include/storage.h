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

static_assert(
    MAX_LOGICAL_KEYS <= 64,
    "slot bitmaps are 64-bit"
);

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

// What begin() found on the device.
enum class BootKind : std::uint8_t {
    NONE = 0,
    FRESH_FORMAT,      // no valid superblock; empty store created
    RECOVERED,         // log replayed cleanly
    RECOVERED_TORN_TAIL // log replayed; an interrupted append was discarded
};

// One logical key as held in the RAM index.
struct EntryInfo {
    std::uint8_t key[KEY_MAX];
    std::uint8_t key_len;
    Version version;
    State state;
    std::uint16_t value_len;
};

struct Stats {
    std::size_t live_keys;
    std::size_t tombstones;
    std::size_t slots_used;
    std::size_t slots_pending_drop;

    std::size_t log_used_bytes;
    std::size_t log_free_bytes;
    std::size_t log_capacity_bytes;

    std::uint32_t compactions_completed;
    std::uint32_t compactions_aborted;
    std::uint32_t keys_dropped;

    std::uint8_t active_bank;
    std::uint32_t superblock_generation;
    bool compaction_active;
    BootKind boot_kind;
};

/*
 * Persistent, versioned key/value store on one EEPROM.
 *
 * - Append-only log of CRC'd records in the active bank; a commit
 *   marker is written last, so a torn append is detectable.
 * - RAM index (one entry per logical key) rebuilt from the log at
 *   boot; the highest Version per key wins.
 * - Compaction copies the current record of every key into the
 *   other bank, then switches banks through an A/B superblock.
 *   It runs incrementally (compaction_step) so the caller keeps
 *   servicing the radio while it runs, and is crash safe: the new
 *   bank becomes authoritative only when its superblock commits.
 *
 * Only this class touches the EEPROM (rule R1).
 */
class StorageManager {
public:
    explicit StorageManager(eeprom::Device& device);

    // device must already be initialised.
    Result begin();

    // Erase everything and create an empty store.
    Result format();

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

    // Version and state of a key (tombstones included).
    Result get_version(
        KeyView key,
        Version& version,
        State& state
    ) const;

    bool ready() const;

    Result max_persisted_hlc(Hlc& out) const;

    // -------- iteration (anti-entropy, console dump) --------

    static constexpr std::size_t slot_capacity()
    {
        return MAX_LOGICAL_KEYS;
    }

    // false when the slot is empty.
    bool slot_info(
        std::size_t slot,
        EntryInfo& info
    ) const;

    int find_slot(KeyView key) const;

    Result read_slot_value(
        std::size_t slot,
        std::uint8_t* value_out,
        std::size_t value_capacity,
        std::size_t& value_len
    );

    // -------- lifecycle --------

    // Blocking full compaction (tests, console).
    Result compact();

    // True when the log is low on space or dropped keys wait to be
    // reclaimed under index pressure.
    bool compaction_needed() const;

    bool compaction_active() const;

    // Starts a compaction if none is running.
    Result compaction_begin();

    // Performs a bounded amount of compaction work. done is set
    // when the bank switch has committed (or nothing was running).
    Result compaction_step(bool& done);

    // Forget a key locally on the next compaction (no tombstone is
    // written). Used for garbage collection decided by anti-entropy.
    Result mark_drop(std::size_t slot);

    // Cause of the most recent failure of apply/set/remove/compaction/
    // format; NONE after one of those succeeds.
    Fault last_fault() const { return last_fault_; }

    bool drop_pending(std::size_t slot) const;

    Stats stats() const;

    std::size_t free_log_bytes() const;

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

    enum class CompactionPhase : std::uint8_t {
        IDLE = 0,
        CLEARING,
        COPYING,
        FINALIZING
    };

    struct Compaction {
        CompactionPhase phase = CompactionPhase::IDLE;
        metadata::Bank destination = metadata::Bank::B;
        std::uint32_t bank_generation = 0;
        std::uint32_t superblock_generation = 0;
        std::uint16_t clear_cursor = 0;
        std::uint16_t append_address = 0;
        std::uint32_t next_sequence = 0;
        std::uint16_t record_count = 0;
        std::uint64_t copied_mask = 0;
        std::uint16_t new_addresses[MAX_LOGICAL_KEYS]{};
    };

    eeprom::Device& eeprom_;

    IndexEntry index_[MAX_LOGICAL_KEYS]{};
    bool initialized_ = false;

    std::uint64_t drop_mask_ = 0;
    Compaction compaction_{};

    std::uint32_t compactions_completed_ = 0;
    std::uint32_t compactions_aborted_ = 0;
    std::uint32_t keys_dropped_ = 0;
    BootKind boot_kind_ = BootKind::NONE;
    mutable Fault last_fault_ = Fault::NONE;

    Result fail(Result result, Fault fault) const
    {
        last_fault_ = fault;
        return result;
    }

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

    Result recover_active_bank(bool& torn_tail);

    bool erased_range(
        uint16_t address,
        uint16_t end_address
    );

    Result copy_one_for_compaction(std::size_t slot);

    Result finalize_compaction();

    void abort_compaction();
};

int compare_versions(
    const Version& lhs,
    const Version& rhs
);

} // namespace storage

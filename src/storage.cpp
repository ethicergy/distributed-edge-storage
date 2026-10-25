#include "storage.h"
#include "config.h"
#include "eeprom.h"
#include "storage_metadata.h"
#include "storage_record.h"

#include <cstring>

namespace storage {
namespace {

constexpr uint16_t BANK_A_HEADER =
    metadata::BANK_A_BASE;

constexpr uint16_t BANK_A_FOOTER =
    metadata::BANK_A_BASE + metadata::BANK_SIZE
    - metadata::BANK_FOOTER_SIZE;

constexpr uint16_t BANK_B_HEADER =
    metadata::BANK_B_BASE;

constexpr uint16_t BANK_B_FOOTER =
    metadata::BANK_B_BASE + metadata::BANK_SIZE
    - metadata::BANK_FOOTER_SIZE;

uint16_t bank_header_address(metadata::Bank bank)
{
    return bank == metadata::Bank::A
        ? BANK_A_HEADER
        : BANK_B_HEADER;
}

uint16_t bank_footer_address(metadata::Bank bank)
{
    return bank == metadata::Bank::A
        ? BANK_A_FOOTER
        : BANK_B_FOOTER;
}

uint16_t bank_record_start(metadata::Bank bank)
{
    return bank_header_address(bank)
         + metadata::BANK_RECORD_OFFSET;
}

uint16_t bank_record_end(metadata::Bank bank)
{
    return static_cast<uint16_t>(
        bank_header_address(bank) +
        metadata::BANK_RECORD_AREA_END
    );
}

metadata::Bank other_bank(metadata::Bank bank)
{
    return bank == metadata::Bank::A
        ? metadata::Bank::B
        : metadata::Bank::A;
}

// End of the region an interrupted append starting at address can
// have touched.
uint16_t torn_tail_limit(uint16_t address, uint16_t record_end)
{
    const size_t remaining =
        static_cast<size_t>(record_end - address);

    return remaining > record::MAX_RECORD_SIZE
        ? static_cast<uint16_t>(address + record::MAX_RECORD_SIZE)
        : record_end;
}

constexpr std::uint64_t slot_bit(std::size_t slot)
{
    return 1ULL << slot;
}

} // namespace

StorageManager::StorageManager(eeprom::Device& device)
    : eeprom_(device)
{
}

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

Result StorageManager::map_eeprom_result(
    eeprom::Result result
) const
{
    switch (result) {
    case eeprom::Result::OK:
        return Result::OK;

    case eeprom::Result::INVALID_ARGUMENT:
        return Result::INVALID_ARGUMENT;

    case eeprom::Result::OUT_OF_RANGE:
        return fail(Result::STORAGE_ERROR, Fault::OUT_OF_RANGE);

    case eeprom::Result::NOT_INITIALIZED:
        return Result::NOT_INITIALIZED;

    case eeprom::Result::I2C_ERROR:
        return fail(Result::STORAGE_ERROR, Fault::I2C_ERROR);

    case eeprom::Result::WRITE_TIMEOUT:
        return fail(Result::STORAGE_ERROR, Fault::WRITE_TIMEOUT);
    }

    return fail(Result::STORAGE_ERROR, Fault::I2C_ERROR);
}

Result StorageManager::write_committed_block(
    uint16_t address,
    const uint8_t* data,
    size_t length,
    size_t commit_offset
)
{
    if (data == nullptr ||
        length == 0 ||
        length > metadata::SUPERBLOCK_SIZE ||
        commit_offset >= length) {
        return Result::INVALID_ARGUMENT;
    }

    uint8_t temporary[metadata::SUPERBLOCK_SIZE]{};

    std::memcpy(
        temporary,
        data,
        length
    );

    const uint8_t commit_marker =
        temporary[commit_offset];

    // Suppress the commit marker during the first write.
    temporary[commit_offset] = 0x00;

    eeprom::Result eeprom_result =
        eeprom_.write(
            address,
            temporary,
            length
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    // Verify everything written before commit.
    uint8_t verify[metadata::SUPERBLOCK_SIZE]{};

    eeprom_result =
        eeprom_.read(
            address,
            verify,
            length
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    if (std::memcmp(
            verify,
            temporary,
            length
        ) != 0) {
        return fail(Result::STORAGE_ERROR, Fault::VERIFY_MISMATCH);
    }

    // Commit marker is deliberately written last.
    eeprom_result =
        eeprom_.write(
            static_cast<uint16_t>(
                address + commit_offset
            ),
            &commit_marker,
            1
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    // Verify the complete committed block.
    eeprom_result =
        eeprom_.read(
            address,
            verify,
            length
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    if (std::memcmp(
            verify,
            data,
            length
        ) != 0) {
        return fail(Result::STORAGE_ERROR, Fault::VERIFY_MISMATCH);
    }

    return Result::OK;
}


Result StorageManager::write_bank_header(
    metadata::Bank bank,
    metadata::BankState state,
    uint32_t generation,
    uint16_t record_count,
    uint16_t records_end_offset
)
{
    metadata::BankHeader header{
        state,
        generation,
        metadata::BANK_RECORD_OFFSET,
        metadata::BANK_RECORD_AREA_END,
        record_count,
        records_end_offset
    };

    uint8_t buffer[
        metadata::BANK_HEADER_SIZE
    ]{};

    const Result serialize_result =
        metadata::serialize_bank_header(
            header,
            buffer,
            sizeof(buffer)
        );

    if (serialize_result != Result::OK) {
        return serialize_result;
    }

    return write_committed_block(
        bank_header_address(bank),
        buffer,
        sizeof(buffer),
        18
    );
}

Result StorageManager::write_bank_footer(
    metadata::Bank bank,
    uint32_t generation,
    uint16_t record_count,
    uint16_t records_end_offset
)
{
    metadata::BankFooter footer{
        generation,
        record_count,
        records_end_offset
    };

    uint8_t buffer[
        metadata::BANK_FOOTER_SIZE
    ]{};

    const Result serialize_result =
        metadata::serialize_bank_footer(
            footer,
            buffer,
            sizeof(buffer)
        );

    if (serialize_result != Result::OK) {
        return serialize_result;
    }

    return write_committed_block(
        bank_footer_address(bank),
        buffer,
        sizeof(buffer),
        14
    );
}

Result StorageManager::write_superblock(
    uint32_t generation,
    metadata::Bank active_bank,
    uint32_t active_bank_generation,
    uint32_t last_known_sequence,
    uint16_t stored_record_count
)
{
    metadata::Superblock superblock{
        generation,
        active_bank,
        active_bank_generation,
        last_known_sequence,
        stored_record_count
    };

    uint8_t buffer[
        metadata::SUPERBLOCK_SIZE
    ]{};

    const Result serialize_result =
        metadata::serialize_superblock(
            superblock,
            buffer,
            sizeof(buffer)
        );

    if (serialize_result != Result::OK) {
        return serialize_result;
    }

    const uint16_t slot =
        (generation % 2u == 1u)
            ? metadata::SUPERBLOCK_SLOT_0
            : metadata::SUPERBLOCK_SLOT_1;

    return write_committed_block(
        slot,
        buffer,
        sizeof(buffer),
        23
    );
}







Result StorageManager::clear_range(
    uint16_t address,
    size_t length
)
{
    constexpr size_t CHUNK_SIZE = 64;

    uint8_t erased[CHUNK_SIZE];

    for (size_t i = 0; i < CHUNK_SIZE; ++i) {
        erased[i] = 0xFF;
    }

    size_t remaining = length;
    uint16_t current = address;

    while (remaining > 0) {
        const size_t chunk =
            remaining > CHUNK_SIZE
                ? CHUNK_SIZE
                : remaining;

        const eeprom::Result result =
            eeprom_.write(
                current,
                erased,
                chunk
            );

        if (result != eeprom::Result::OK) {
            return map_eeprom_result(result);
        }

        current = static_cast<uint16_t>(
            current + chunk
        );

        remaining -= chunk;
    }

    return Result::OK;
}


Result StorageManager::initialize_empty_store()
{
    // Use Bank A as the first authoritative bank.
    active_bank_ = metadata::Bank::A;
    superblock_generation_ = 1;
    active_bank_generation_ = 1;
    next_sequence_ = 1;

    append_address_ =
        static_cast<uint16_t>(
            metadata::BANK_A_BASE +
            metadata::BANK_RECORD_OFFSET
        );

    record_count_ = 0;

    // Clear Bank A record area so every byte after the
    // append position is known to be unused.
    Result result = clear_range(
        static_cast<uint16_t>(
            metadata::BANK_A_BASE +
            metadata::BANK_RECORD_OFFSET
        ),
        metadata::BANK_RECORD_AREA_END -
        metadata::BANK_RECORD_OFFSET
    );

    if (result != Result::OK) {
        return result;
    }

    // Create the initial active bank as OPEN.
    result = write_bank_header(
        active_bank_,
        metadata::BankState::OPEN,
        active_bank_generation_,
        0,
        metadata::BANK_RECORD_OFFSET
    );

    if (result != Result::OK) {
        return result;
    }

    // Create an empty footer checkpoint.
    result = write_bank_footer(
        active_bank_,
        active_bank_generation_,
        0,
        metadata::BANK_RECORD_OFFSET
    );

    if (result != Result::OK) {
        return result;
    }

    // Finally publish Bank A through Superblock generation 1.
    //
    // This is the point at which the empty store becomes
    // authoritative for recovery.
    result = write_superblock(
        superblock_generation_,
        active_bank_,
        active_bank_generation_,
        0,
        0
    );

    if (result != Result::OK) {
        return result;
    }

    return Result::OK;
}

bool StorageManager::erased_range(
    uint16_t address,
    uint16_t end_address
)
{
    constexpr size_t CHUNK_SIZE = 64;

    uint8_t buffer[CHUNK_SIZE]{};

    uint16_t current = address;

    while (current < end_address) {
        const size_t remaining =
            static_cast<size_t>(
                end_address - current
            );

        const size_t chunk =
            remaining > CHUNK_SIZE
                ? CHUNK_SIZE
                : remaining;

        const eeprom::Result result =
            eeprom_.read(
                current,
                buffer,
                chunk
            );

        if (result != eeprom::Result::OK) {
            return false;
        }

        for (size_t i = 0; i < chunk; ++i) {
            if (buffer[i] != 0xFF) {
                return false;
            }
        }

        current = static_cast<uint16_t>(
            current + chunk
        );
    }

    return true;
}

Result StorageManager::recover_active_bank(bool& torn_tail)
{
    torn_tail = false;

    // RAM index is rebuilt entirely from persistent records.
    std::memset(
        index_,
        0,
        sizeof(index_)
    );

    const uint16_t record_start =
        bank_record_start(active_bank_);

    const uint16_t record_end =
        bank_record_end(active_bank_);

    uint16_t current_address = record_start;

    uint32_t highest_sequence = 0;
    uint16_t committed_records = 0;

    /*
     * An interrupted append writes at most one record, so a torn
     * tail is confined to MAX_RECORD_SIZE bytes after the last
     * committed record, with only erased bytes behind it. Such a
     * tail is wiped so the next append starts on erased bytes.
     * Anything else that fails to decode is real corruption.
     */
    auto handle_bad_tail = [&]() -> Result {
        const uint16_t limit =
            torn_tail_limit(current_address, record_end);

        if (!erased_range(limit, record_end)) {
            return fail(Result::CORRUPTED, Fault::CORRUPTED);
        }

        const Result clear_result =
            clear_range(
                current_address,
                static_cast<size_t>(limit - current_address)
            );

        if (clear_result != Result::OK) {
            return clear_result;
        }

        torn_tail = true;
        return Result::OK;
    };

    while (current_address < record_end) {

        // First check whether the next record area is erased.
        uint8_t probe[2]{};

        eeprom::Result eeprom_result =
            eeprom_.read(
                current_address,
                probe,
                sizeof(probe)
            );

        if (eeprom_result != eeprom::Result::OK) {
            return map_eeprom_result(eeprom_result);
        }

        if (probe[0] == 0xFF &&
            probe[1] == 0xFF) {

            const uint16_t limit =
                torn_tail_limit(current_address, record_end);

            // Known unused tail, unless an append was torn after
            // its first two bytes were still erased.
            if (!erased_range(current_address, limit)) {
                const Result tail_result = handle_bad_tail();

                if (tail_result != Result::OK) {
                    return tail_result;
                }
            }

            break;
        }

        // Read the fixed record header.
        uint8_t header[
            record::HEADER_SIZE
        ]{};

        const size_t header_read =
            static_cast<size_t>(record_end - current_address) <
                    record::HEADER_SIZE
                ? static_cast<size_t>(record_end - current_address)
                : record::HEADER_SIZE;

        eeprom_result =
            eeprom_.read(
                current_address,
                header,
                header_read
            );

        if (eeprom_result != eeprom::Result::OK) {
            return map_eeprom_result(eeprom_result);
        }

        const uint16_t record_length =
            static_cast<uint16_t>(
                (static_cast<uint16_t>(header[4]) << 8) |
                static_cast<uint16_t>(header[5])
            );

        const bool plausible_header =
            header_read == record::HEADER_SIZE &&
            (header[0] == 0x52) &&
            (header[1] == 0x43) &&
            record_length >=
                record::HEADER_SIZE +
                record::CRC_SIZE +
                record::COMMIT_MARKER_SIZE &&
            record_length <=
                record::MAX_RECORD_SIZE &&
            record_length <=
                static_cast<uint16_t>(
                    record_end - current_address
                );

        record::Record persistent_record{};

        uint8_t record_buffer[
            record::MAX_RECORD_SIZE
        ]{};

        bool decoded = false;

        if (plausible_header) {
            eeprom_result =
                eeprom_.read(
                    current_address,
                    record_buffer,
                    record_length
                );

            if (eeprom_result != eeprom::Result::OK) {
                return map_eeprom_result(eeprom_result);
            }

            decoded =
                record::deserialize(
                    record_buffer,
                    record_length,
                    persistent_record
                ) == Result::OK;
        }

        if (!decoded) {
            const Result tail_result = handle_bad_tail();

            if (tail_result != Result::OK) {
                return tail_result;
            }

            break;
        }

        // Track physical sequence information.
        if (persistent_record.sequence >
            highest_sequence) {
            highest_sequence =
                persistent_record.sequence;
        }

        ++committed_records;

        // Rebuild logical state.
        const int existing_index =
            find_index_entry(
                persistent_record.key
            );

        if (existing_index >= 0) {

            IndexEntry& current_entry =
                index_[existing_index];

            const Version current_version{
                current_entry.version_hlc,
                current_entry.version_origin
            };

            if (compare_versions(
                    persistent_record.version,
                    current_version
                ) > 0) {

                update_index_entry(
                    static_cast<size_t>(
                        existing_index
                    ),
                    persistent_record.key,
                    persistent_record.state,
                    static_cast<uint16_t>(
                        persistent_record.value.len
                    ),
                    current_address,
                    persistent_record.version
                );
            }

        } else {

            const int free_index =
                find_free_index_entry();

            if (free_index < 0) {
                return fail(Result::INDEX_FULL, Fault::INDEX_FULL);
            }

            update_index_entry(
                static_cast<size_t>(free_index),
                persistent_record.key,
                persistent_record.state,
                static_cast<uint16_t>(
                    persistent_record.value.len
                ),
                current_address,
                persistent_record.version
            );
        }

        current_address =
            static_cast<uint16_t>(
                current_address + record_length
            );
    }

    append_address_ = current_address;

    record_count_ = committed_records;

    if (highest_sequence + 1 > next_sequence_) {
        next_sequence_ = highest_sequence + 1;
    }

    return Result::OK;
}

Result StorageManager::append_record(
    KeyView key,
    ValueView value,
    Version version,
    State state,
    uint16_t& record_address
)
{
    return append_record_to_bank(
        active_bank_,
        append_address_,
        next_sequence_,
        key,
        value,
        version,
        state,
        record_address
    );
}
Result StorageManager::append_record_to_bank(
    metadata::Bank bank,
    uint16_t& append_address,
    uint32_t& next_sequence,
    KeyView key,
    ValueView value,
    Version version,
    State state,
    uint16_t& record_address
)
{
    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (key.len == 0) {
        return Result::INVALID_ARGUMENT;
    }

    if (key.len > KEY_MAX) {
        return Result::KEY_TOO_LARGE;
    }

    if (key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (value.len > VALUE_MAX) {
        return Result::VALUE_TOO_LARGE;
    }

    if (value.len > 0 && value.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (state != State::LIVE &&
        state != State::TOMBSTONE) {
        return Result::INVALID_ARGUMENT;
    }

    if (state == State::TOMBSTONE &&
        value.len != 0) {
        return Result::INVALID_ARGUMENT;
    }

    if (!metadata::is_valid_bank(bank)) {
        return Result::INVALID_ARGUMENT;
    }

    record::Record persistent_record{
        state,
        version,
        next_sequence,
        key,
        value
    };

    uint8_t buffer[record::MAX_RECORD_SIZE]{};
    size_t record_length = 0;

    Result result = record::serialize(
        persistent_record,
        buffer,
        sizeof(buffer),
        record_length
    );

    if (result != Result::OK) {
        return result;
    }

    const uint16_t record_area_end =
        static_cast<uint16_t>(
            bank_header_address(bank) +
            metadata::BANK_RECORD_AREA_END
        );

    if (append_address < bank_record_start(bank) ||
        append_address > record_area_end) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    const size_t remaining =
        static_cast<size_t>(
            record_area_end - append_address
        );

    if (record_length > remaining) {
        return fail(Result::NO_SPACE, Fault::NO_SPACE);
    }

    record_address = append_address;

    // Write everything except the commit marker.
    eeprom::Result eeprom_result =
        eeprom_.write(
            record_address,
            buffer,
            record_length - 1
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    // Verify record body before committing.
    uint8_t verify[
        record::MAX_RECORD_SIZE
    ]{};

    eeprom_result =
        eeprom_.read(
            record_address,
            verify,
            record_length - 1
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    if (std::memcmp(
            verify,
            buffer,
            record_length - 1
        ) != 0) {
        return fail(Result::STORAGE_ERROR, Fault::VERIFY_MISMATCH);
    }

    // Commit marker is written last.
    const uint8_t commit_marker =
        buffer[record_length - 1];

    eeprom_result =
        eeprom_.write(
            static_cast<uint16_t>(
                record_address + record_length - 1
            ),
            &commit_marker,
            1
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    // Verify complete committed record.
    eeprom_result =
        eeprom_.read(
            record_address,
            verify,
            record_length
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    if (std::memcmp(
            verify,
            buffer,
            record_length
        ) != 0) {
        return fail(Result::STORAGE_ERROR, Fault::VERIFY_MISMATCH);
    }

    append_address =
        static_cast<uint16_t>(
            append_address + record_length
        );

    ++next_sequence;

    return Result::OK;
}

/*
 * Compaction
 * ----------
 * CLEARING   : erase the destination bank's record area a few pages
 *              per step, then write its header (OPEN).
 * COPYING    : copy the current record of one key per step. A key
 *              written again after it was copied is copied again.
 *              Keys marked for drop are skipped.
 * FINALIZING : footer, header SEALED, then the superblock that makes
 *              the destination authoritative (the atomic switch).
 *
 * Until the superblock commits, the source bank stays authoritative
 * and keeps receiving appends, so a crash at any point loses nothing.
 */

bool StorageManager::compaction_active() const
{
    return compaction_.phase != CompactionPhase::IDLE;
}

bool StorageManager::compaction_needed() const
{
    if (!initialized_ || compaction_active()) {
        return false;
    }

    if (free_log_bytes() < config::COMPACTION_TRIGGER_FREE_BYTES) {
        return true;
    }

    if (drop_mask_ == 0) {
        return false;
    }

    std::size_t used = 0;

    for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
        if (index_[i].valid) {
            ++used;
        }
    }

    return used * 100 >=
        MAX_LOGICAL_KEYS *
            config::COMPACTION_INDEX_PRESSURE_PERCENT;
}

Result StorageManager::compaction_begin()
{
    last_fault_ = Fault::NONE;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (compaction_active()) {
        return Result::OK;
    }

    compaction_ = Compaction{};

    compaction_.phase = CompactionPhase::CLEARING;
    compaction_.destination = other_bank(active_bank_);
    compaction_.bank_generation = active_bank_generation_ + 1;
    compaction_.superblock_generation = superblock_generation_ + 1;
    compaction_.clear_cursor =
        bank_record_start(compaction_.destination);
    compaction_.append_address =
        bank_record_start(compaction_.destination);

    return Result::OK;
}

void StorageManager::abort_compaction()
{
    compaction_ = Compaction{};
    ++compactions_aborted_;
}

Result StorageManager::copy_one_for_compaction(std::size_t slot)
{
    const IndexEntry& entry = index_[slot];

    if (entry.state != State::LIVE &&
        entry.state != State::TOMBSTONE) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    uint8_t value_buffer[VALUE_MAX]{};
    size_t value_length = 0;

    if (entry.state == State::LIVE) {
        const Result read_result = read_value(
            entry,
            value_buffer,
            sizeof(value_buffer),
            value_length
        );

        if (read_result != Result::OK) {
            return read_result;
        }
    }

    uint16_t destination_record_address = 0;

    const Result result = append_record_to_bank(
        compaction_.destination,
        compaction_.append_address,
        next_sequence_,
        KeyView{entry.key, entry.key_len},
        ValueView{
            entry.state == State::LIVE ? value_buffer : nullptr,
            value_length
        },
        Version{entry.version_hlc, entry.version_origin},
        entry.state,
        destination_record_address
    );

    if (result != Result::OK) {
        return result;
    }

    compaction_.new_addresses[slot] = destination_record_address;
    compaction_.copied_mask |= slot_bit(slot);
    ++compaction_.record_count;

    return Result::OK;
}

Result StorageManager::finalize_compaction()
{
    const metadata::Bank destination = compaction_.destination;

    const uint16_t destination_records_end =
        static_cast<uint16_t>(
            compaction_.append_address -
            bank_header_address(destination)
        );

    Result result = write_bank_footer(
        destination,
        compaction_.bank_generation,
        compaction_.record_count,
        destination_records_end
    );

    if (result != Result::OK) {
        return result;
    }

    result = write_bank_header(
        destination,
        metadata::BankState::SEALED,
        compaction_.bank_generation,
        compaction_.record_count,
        destination_records_end
    );

    if (result != Result::OK) {
        return result;
    }

    // The atomic authority switch.
    result = write_superblock(
        compaction_.superblock_generation,
        destination,
        compaction_.bank_generation,
        next_sequence_ == 0 ? 0 : next_sequence_ - 1,
        compaction_.record_count
    );

    if (result != Result::OK) {
        // Old bank remains authoritative.
        return result;
    }

    // Only now update runtime state.
    for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
        if (!index_[i].valid) {
            continue;
        }

        if ((compaction_.copied_mask & slot_bit(i)) != 0) {
            index_[i].record_address = compaction_.new_addresses[i];
            continue;
        }

        // Not copied: only dropped keys get here.
        index_[i] = IndexEntry{};
        ++keys_dropped_;
    }

    // A drop requested after its key was already copied waits for
    // the next compaction.
    drop_mask_ &= compaction_.copied_mask;

    active_bank_ = destination;
    superblock_generation_ = compaction_.superblock_generation;
    active_bank_generation_ = compaction_.bank_generation;
    append_address_ = compaction_.append_address;
    record_count_ = compaction_.record_count;

    compaction_ = Compaction{};
    ++compactions_completed_;

    return Result::OK;
}

Result StorageManager::compaction_step(bool& done)
{
    last_fault_ = Fault::NONE;

    done = false;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    switch (compaction_.phase) {
        case CompactionPhase::IDLE:
            done = true;
            return Result::OK;

        case CompactionPhase::CLEARING: {
            const uint16_t end =
                bank_record_end(compaction_.destination);

            const size_t budget =
                config::COMPACTION_CLEAR_PAGES_PER_STEP *
                eeprom::PAGE_SIZE;

            const size_t remaining =
                static_cast<size_t>(end - compaction_.clear_cursor);

            const size_t chunk =
                remaining < budget ? remaining : budget;

            if (chunk > 0) {
                const Result clear_result =
                    clear_range(compaction_.clear_cursor, chunk);

                if (clear_result != Result::OK) {
                    abort_compaction();
                    return clear_result;
                }

                compaction_.clear_cursor =
                    static_cast<uint16_t>(
                        compaction_.clear_cursor + chunk
                    );
            }

            if (compaction_.clear_cursor < end) {
                return Result::OK;
            }

            const Result header_result = write_bank_header(
                compaction_.destination,
                metadata::BankState::OPEN,
                compaction_.bank_generation,
                0,
                metadata::BANK_RECORD_OFFSET
            );

            if (header_result != Result::OK) {
                abort_compaction();
                return header_result;
            }

            compaction_.phase = CompactionPhase::COPYING;
            return Result::OK;
        }

        case CompactionPhase::COPYING: {
            for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
                if (!index_[i].valid ||
                    (compaction_.copied_mask & slot_bit(i)) != 0 ||
                    (drop_mask_ & slot_bit(i)) != 0) {
                    continue;
                }

                const Result copy_result = copy_one_for_compaction(i);

                if (copy_result != Result::OK) {
                    abort_compaction();
                    return copy_result;
                }

                return Result::OK;
            }

            /*
             * Every key is copied. Finalize in this same step: a
             * write landing between "all copied" and the switch would
             * otherwise be mistaken for a dropped key.
             */
            compaction_.phase = CompactionPhase::FINALIZING;
        }
        // fall through

        case CompactionPhase::FINALIZING: {
            const Result result = finalize_compaction();

            if (result != Result::OK) {
                abort_compaction();
                return result;
            }

            done = true;
            return Result::OK;
        }
    }

    return fail(Result::CORRUPTED, Fault::CORRUPTED);
}

Result StorageManager::compact()
{
    last_fault_ = Fault::NONE;

    Result result = compaction_begin();

    if (result != Result::OK) {
        return result;
    }

    bool done = false;

    while (!done) {
        result = compaction_step(done);

        if (result != Result::OK) {
            return result;
        }
    }

    return Result::OK;
}

Result StorageManager::mark_drop(std::size_t slot)
{
    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (slot >= MAX_LOGICAL_KEYS || !index_[slot].valid) {
        return Result::INVALID_ARGUMENT;
    }

    drop_mask_ |= slot_bit(slot);

    return Result::OK;
}

bool StorageManager::drop_pending(std::size_t slot) const
{
    return slot < MAX_LOGICAL_KEYS &&
           (drop_mask_ & slot_bit(slot)) != 0;
}

std::size_t StorageManager::free_log_bytes() const
{
    if (!initialized_) {
        return 0;
    }

    const uint16_t end = bank_record_end(active_bank_);

    return append_address_ >= end
        ? 0
        : static_cast<std::size_t>(end - append_address_);
}

Result StorageManager::apply(
    KeyView key,
    ValueView value,
    Version version,
    State state
)
{
    last_fault_ = Fault::NONE;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (key.len == 0) {
        return Result::INVALID_ARGUMENT;
    }

    if (key.len > KEY_MAX) {
        return Result::KEY_TOO_LARGE;
    }

    if (key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (value.len > VALUE_MAX) {
        return Result::VALUE_TOO_LARGE;
    }

    if (value.len > 0 && value.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (state != State::LIVE &&
        state != State::TOMBSTONE) {
        return Result::INVALID_ARGUMENT;
    }

    if (state == State::TOMBSTONE &&
        value.len != 0) {
        return Result::INVALID_ARGUMENT;
    }

    const int existing_index =
        find_index_entry(key);

    int target_index = existing_index;

    if (existing_index >= 0) {

        const IndexEntry& entry =
            index_[existing_index];

        const Version current_version{
            entry.version_hlc,
            entry.version_origin
        };

        const int version_comparison =
            compare_versions(
                version,
                current_version
            );

        if (version_comparison <= 0) {
            return Result::STALE_VERSION;
        }

    } else {

        target_index = find_free_index_entry();

        if (target_index < 0) {
            return fail(Result::INDEX_FULL, Fault::INDEX_FULL);
        }
    }

    uint16_t record_address = 0;

    const Result append_result =
        append_record(
            key,
            value,
            version,
            state,
            record_address
        );

    if (append_result == Result::NO_SPACE &&
        !compaction_active()) {
        // The caller gets NO_SPACE for this write; the background
        // compaction makes room for the next one.
        (void)compaction_begin();
        last_fault_ = Fault::NO_SPACE; // compaction_begin cleared it
    }

    if (append_result != Result::OK) {
        return append_result;
    }

    update_index_entry(
        static_cast<std::size_t>(target_index),
        key,
        state,
        static_cast<uint16_t>(value.len),
        record_address,
        version
    );

    const std::uint64_t bit =
        slot_bit(static_cast<std::size_t>(target_index));

    // A newer version revives a key that was marked for drop.
    drop_mask_ &= ~bit;

    // A running compaction must copy this key (again).
    compaction_.copied_mask &= ~bit;

    return Result::OK;
}
Result StorageManager::set(
    KeyView key,
    ValueView value,
    Version version
)
{
    last_fault_ = Fault::NONE;

    return apply(
        key,
        value,
        version,
        State::LIVE
    );
}

Result StorageManager::remove(
    KeyView key,
    Version version
)
{
    last_fault_ = Fault::NONE;

    return apply(
        key,
        ValueView{nullptr, 0},
        version,
        State::TOMBSTONE
    );
}

Result StorageManager::read_value(
    const IndexEntry& entry,
    uint8_t* value_out,
    size_t value_capacity,
    size_t& value_len
)
{
    value_len = 0;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (entry.state == State::TOMBSTONE) {
        return Result::NOT_FOUND;
    }

    if (entry.value_len > value_capacity) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (entry.value_len > 0 && value_out == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    uint8_t buffer[record::MAX_RECORD_SIZE]{};

    uint8_t header[record::HEADER_SIZE]{};

    eeprom::Result eeprom_result =
        eeprom_.read(
            entry.record_address,
            header,
            sizeof(header)
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    const uint16_t record_length =
        static_cast<uint16_t>(
            (static_cast<uint16_t>(header[4]) << 8) |
            static_cast<uint16_t>(header[5])
        );

    if (record_length < record::HEADER_SIZE +
                           record::CRC_SIZE +
                           record::COMMIT_MARKER_SIZE ||
        record_length > record::MAX_RECORD_SIZE) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    eeprom_result =
        eeprom_.read(
            entry.record_address,
            buffer,
            record_length
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    record::Record persistent_record{};

    const Result decode_result =
        record::deserialize(
            buffer,
            record_length,
            persistent_record
        );

    if (decode_result != Result::OK) {
        return decode_result;
    }

    if (persistent_record.state != State::LIVE ||
        persistent_record.key.len != entry.key_len ||
        std::memcmp(
            persistent_record.key.data,
            entry.key,
            entry.key_len
        ) != 0 ||
        persistent_record.version.hlc != entry.version_hlc ||
        persistent_record.version.origin != entry.version_origin) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    if (persistent_record.value.len > 0) {
        std::memcpy(
            value_out,
            persistent_record.value.data,
            persistent_record.value.len
        );
    }

    value_len = persistent_record.value.len;

    return Result::OK;
}

Result StorageManager::get(
    KeyView key,
    uint8_t* value_out,
    size_t value_capacity,
    size_t& value_len
)
{
    value_len = 0;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (key.len == 0 ||
        key.len > KEY_MAX ||
        (key.len > 0 && key.data == nullptr)) {
        return key.len > KEY_MAX
            ? Result::KEY_TOO_LARGE
            : Result::INVALID_ARGUMENT;
    }

    const int index =
        find_index_entry(key);

    if (index < 0) {
        return Result::NOT_FOUND;
    }

    const IndexEntry& entry =
        index_[index];

    if (entry.state == State::TOMBSTONE) {
        return Result::NOT_FOUND;
    }

    return read_value(
        entry,
        value_out,
        value_capacity,
        value_len
    );
}



Result StorageManager::begin()
{
    if (initialized_) {
        return Result::OK;
    }

    if (!eeprom_.ready()) {
        return Result::NOT_INITIALIZED;
    }

    std::memset(index_, 0, sizeof(index_));
    drop_mask_ = 0;
    compaction_ = Compaction{};
    boot_kind_ = BootKind::NONE;

    eeprom::Result eeprom_result = eeprom::Result::OK;

    uint8_t slot0[
        metadata::SUPERBLOCK_SIZE
    ]{};

    uint8_t slot1[
        metadata::SUPERBLOCK_SIZE
    ]{};

    eeprom_result =
        eeprom_.read(
            metadata::SUPERBLOCK_SLOT_0,
            slot0,
            sizeof(slot0)
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    eeprom_result =
        eeprom_.read(
            metadata::SUPERBLOCK_SLOT_1,
            slot1,
            sizeof(slot1)
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    metadata::Superblock sb0{};
    metadata::Superblock sb1{};

    const bool valid0 =
        deserialize_superblock(
            slot0,
            sizeof(slot0),
            sb0
        ) == Result::OK;

    const bool valid1 =
        deserialize_superblock(
            slot1,
            sizeof(slot1),
            sb1
        ) == Result::OK;

    // Fresh EEPROM.
    if (!valid0 && !valid1) {
        const Result result =
            initialize_empty_store();

        if (result != Result::OK) {
            return result;
        }

        boot_kind_ = BootKind::FRESH_FORMAT;
        initialized_ = true;
        return Result::OK;
    }

    // Select newest valid superblock.
    metadata::Superblock selected{};

    if (valid0 && valid1) {
        selected =
            sb0.generation >= sb1.generation
                ? sb0
                : sb1;
    } else {
        selected = valid0 ? sb0 : sb1;
    }

    active_bank_ = selected.active_bank;
    superblock_generation_ = selected.generation;
    active_bank_generation_ =
        selected.active_bank_generation;
    next_sequence_ =
        selected.last_known_sequence + 1;
    record_count_ =
        selected.stored_record_count;

    uint8_t header_buffer[
        metadata::BANK_HEADER_SIZE
    ]{};

    eeprom_result =
        eeprom_.read(
            bank_header_address(active_bank_),
            header_buffer,
            sizeof(header_buffer)
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    metadata::BankHeader header{};

    if (deserialize_bank_header(
            header_buffer,
            sizeof(header_buffer),
            header
        ) != Result::OK) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    uint8_t footer_buffer[
        metadata::BANK_FOOTER_SIZE
    ]{};

    eeprom_result =
        eeprom_.read(
            bank_footer_address(active_bank_),
            footer_buffer,
            sizeof(footer_buffer)
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

    metadata::BankFooter footer{};

    if (deserialize_bank_footer(
            footer_buffer,
            sizeof(footer_buffer),
            footer
        ) != Result::OK) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    if ((header.state != metadata::BankState::OPEN &&
         header.state != metadata::BankState::SEALED) ||
        header.generation != selected.active_bank_generation ||
        footer.generation != header.generation) {
        return fail(Result::CORRUPTED, Fault::CORRUPTED);
    }

    bool torn_tail = false;

    const Result recovery_result =
        recover_active_bank(torn_tail);

    if (recovery_result != Result::OK) {
        return recovery_result;
    }

    boot_kind_ = torn_tail
        ? BootKind::RECOVERED_TORN_TAIL
        : BootKind::RECOVERED;

    initialized_ = true;
    return Result::OK;
}

Result StorageManager::format()
{
    last_fault_ = Fault::NONE;

    if (!eeprom_.ready()) {
        return Result::NOT_INITIALIZED;
    }

    initialized_ = false;

    std::memset(index_, 0, sizeof(index_));
    drop_mask_ = 0;
    compaction_ = Compaction{};

    // Invalidate both superblock slots first, so an interrupted
    // format can never fall back to an old generation.
    Result result = clear_range(
        metadata::SUPERBLOCK_SLOT_0,
        2 * metadata::SUPERBLOCK_SIZE
    );

    if (result != Result::OK) {
        return result;
    }

    result = initialize_empty_store();

    if (result != Result::OK) {
        return result;
    }

    boot_kind_ = BootKind::FRESH_FORMAT;
    initialized_ = true;
    return Result::OK;
}

bool StorageManager::ready() const
{
    return initialized_;
}

Result StorageManager::max_persisted_hlc(Hlc& out) const
{
    out = 0;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
        const IndexEntry& entry = index_[i];

        if (!entry.valid) {
            continue;
        }

        if (entry.version_hlc > out) {
            out = entry.version_hlc;
        }
    }

    return Result::OK;
}

Result StorageManager::get_version(
    KeyView key,
    Version& version,
    State& state
) const
{
    version = Version{0, node::INVALID};
    state = State::LIVE;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (key.len == 0 || key.data == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (key.len > KEY_MAX) {
        return Result::KEY_TOO_LARGE;
    }

    const int index = find_index_entry(key);

    if (index < 0) {
        return Result::NOT_FOUND;
    }

    version = Version{
        index_[index].version_hlc,
        index_[index].version_origin
    };

    state = index_[index].state;

    return Result::OK;
}

bool StorageManager::slot_info(
    std::size_t slot,
    EntryInfo& info
) const
{
    if (!initialized_ ||
        slot >= MAX_LOGICAL_KEYS ||
        !index_[slot].valid) {
        return false;
    }

    const IndexEntry& entry = index_[slot];

    std::memcpy(info.key, entry.key, entry.key_len);
    info.key_len = entry.key_len;
    info.version = Version{entry.version_hlc, entry.version_origin};
    info.state = entry.state;
    info.value_len = entry.value_len;

    return true;
}

int StorageManager::find_slot(KeyView key) const
{
    if (!initialized_ ||
        key.len == 0 ||
        key.len > KEY_MAX ||
        key.data == nullptr) {
        return -1;
    }

    return find_index_entry(key);
}

Result StorageManager::read_slot_value(
    std::size_t slot,
    std::uint8_t* value_out,
    std::size_t value_capacity,
    std::size_t& value_len
)
{
    value_len = 0;

    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    if (slot >= MAX_LOGICAL_KEYS || !index_[slot].valid) {
        return Result::NOT_FOUND;
    }

    return read_value(
        index_[slot],
        value_out,
        value_capacity,
        value_len
    );
}

Stats StorageManager::stats() const
{
    Stats stats{};

    for (std::size_t i = 0; i < MAX_LOGICAL_KEYS; ++i) {
        if (!index_[i].valid) {
            continue;
        }

        ++stats.slots_used;

        if (index_[i].state == State::TOMBSTONE) {
            ++stats.tombstones;
        } else {
            ++stats.live_keys;
        }

        if ((drop_mask_ & slot_bit(i)) != 0) {
            ++stats.slots_pending_drop;
        }
    }

    stats.log_capacity_bytes =
        metadata::BANK_RECORD_AREA_END -
        metadata::BANK_RECORD_OFFSET;

    stats.log_free_bytes = free_log_bytes();

    stats.log_used_bytes =
        initialized_
            ? stats.log_capacity_bytes - stats.log_free_bytes
            : 0;

    stats.compactions_completed = compactions_completed_;
    stats.compactions_aborted = compactions_aborted_;
    stats.keys_dropped = keys_dropped_;
    stats.active_bank = static_cast<std::uint8_t>(active_bank_);
    stats.superblock_generation = superblock_generation_;
    stats.compaction_active = compaction_active();
    stats.boot_kind = boot_kind_;

    return stats;
}

} // namespace storage

#include "storage.h"
#include "eeprom.h"
#include "storage_metadata.h"
#include "storage_record.h"
#include <Arduino.h>

#include <cstring>

namespace storage {
namespace {

constexpr uint8_t EEPROM_SDA_PIN = 8;
constexpr uint8_t EEPROM_SCL_PIN = 9;

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
        return Result::STORAGE_ERROR;

    case eeprom::Result::NOT_INITIALIZED:
        return Result::NOT_INITIALIZED;

    case eeprom::Result::I2C_ERROR:
    case eeprom::Result::WRITE_TIMEOUT:
        return Result::STORAGE_ERROR;
    }

    return Result::STORAGE_ERROR;
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
        return Result::STORAGE_ERROR;
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
        return Result::STORAGE_ERROR;
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

bool StorageManager::erased_tail(
    uint16_t address
)
{
    constexpr size_t CHUNK_SIZE = 64;

    const uint16_t end_address =
        static_cast<uint16_t>(
            bank_header_address(active_bank_) +
            metadata::BANK_RECORD_AREA_END
        );

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

Result StorageManager::recover_active_bank()
{
    // RAM index is rebuilt entirely from persistent records.
    std::memset(
        index_,
        0,
        sizeof(index_)
    );

    const uint16_t record_start =
        bank_record_start(active_bank_);

    const uint16_t record_end =
        static_cast<uint16_t>(
            bank_header_address(active_bank_) +
            metadata::BANK_RECORD_AREA_END
        );

    uint16_t current_address = record_start;

    uint32_t highest_sequence = 0;
    uint16_t committed_records = 0;

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

        // Known unused tail.
        if (probe[0] == 0xFF &&
            probe[1] == 0xFF) {
            break;
        }

        // Read the fixed record header.
        uint8_t header[
            record::HEADER_SIZE
        ]{};

        eeprom_result =
            eeprom_.read(
                current_address,
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

        const bool plausible_header =
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

        if (!plausible_header) {
            // If everything after this position is erased,
            // this is an incomplete tail record.
            if (erased_tail(current_address)) {
                break;
            }

            return Result::CORRUPTED;
        }

        uint8_t record_buffer[
            record::MAX_RECORD_SIZE
        ]{};

        eeprom_result =
            eeprom_.read(
                current_address,
                record_buffer,
                record_length
            );

        if (eeprom_result != eeprom::Result::OK) {
            return map_eeprom_result(eeprom_result);
        }

        record::Record persistent_record{};

        const Result decode_result =
            record::deserialize(
                record_buffer,
                record_length,
                persistent_record
            );

        if (decode_result != Result::OK) {
            // Same tail-vs-middle distinction.
            if (erased_tail(current_address)) {
                break;
            }

            return Result::CORRUPTED;
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
                return Result::INDEX_FULL;
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

    next_sequence_ =
        highest_sequence == 0
            ? 1
            : highest_sequence + 1;

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
        return Result::CORRUPTED;
    }

    const size_t remaining =
        static_cast<size_t>(
            record_area_end - append_address
        );

    if (record_length > remaining) {
        return Result::NO_SPACE;
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
        return Result::STORAGE_ERROR;
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
        return Result::STORAGE_ERROR;
    }

    append_address =
        static_cast<uint16_t>(
            append_address + record_length
        );

    ++next_sequence;

    return Result::OK;
}

Result StorageManager::compact()
{
    if (!initialized_) {
        return Result::NOT_INITIALIZED;
    }

    const metadata::Bank source_bank =
        active_bank_;

    const metadata::Bank destination_bank =
        source_bank == metadata::Bank::A
            ? metadata::Bank::B
            : metadata::Bank::A;

    const uint32_t new_bank_generation =
        active_bank_generation_ + 1;

    const uint32_t new_superblock_generation =
        superblock_generation_ + 1;

    uint16_t destination_append_address =
        bank_record_start(destination_bank);

    uint32_t destination_next_sequence =
        next_sequence_;

    uint16_t new_record_count = 0;

    // Temporary record-address map.
    //
    // We must NOT modify the RAM index until the new bank
    // is completely committed. Otherwise a failed compaction
    // could leave the index pointing at an incomplete bank.
    uint16_t new_record_addresses[
        MAX_LOGICAL_KEYS
    ]{};

    std::memset(
        new_record_addresses,
        0,
        sizeof(new_record_addresses)
    );

    // --------------------------------------------------
    // 1. Clear destination bank record area.
    // --------------------------------------------------

    Result result = clear_range(
        bank_record_start(destination_bank),
        metadata::BANK_RECORD_AREA_END -
        metadata::BANK_RECORD_OFFSET
    );

    if (result != Result::OK) {
        return result;
    }

    // --------------------------------------------------
    // 2. Create destination bank as OPEN.
    // --------------------------------------------------

    result = write_bank_header(
        destination_bank,
        metadata::BankState::OPEN,
        new_bank_generation,
        0,
        metadata::BANK_RECORD_OFFSET
    );

    if (result != Result::OK) {
        return result;
    }

    // --------------------------------------------------
    // 3. Copy exactly one current record per logical key.
    //
    // This includes TOMBSTONE entries.
    // --------------------------------------------------

    for (std::size_t i = 0;
         i < MAX_LOGICAL_KEYS;
         ++i) {

        const IndexEntry& entry =
            index_[i];

        if (!entry.valid) {
            continue;
        }

        if (entry.state != State::LIVE &&
            entry.state != State::TOMBSTONE) {
            return Result::CORRUPTED;
        }

        uint8_t value_buffer[
            VALUE_MAX
        ]{};

        size_t value_length = 0;

        if (entry.state == State::LIVE) {

            result = read_value(
                entry,
                value_buffer,
                sizeof(value_buffer),
                value_length
            );

            if (result != Result::OK) {
                return result;
            }

        } else {

            value_length = 0;
        }

        const Version version{
            entry.version_hlc,
            entry.version_origin
        };

        uint16_t destination_record_address = 0;

        result = append_record_to_bank(
            destination_bank,
            destination_append_address,
            destination_next_sequence,
            KeyView{
                entry.key,
                entry.key_len
            },
            ValueView{
                entry.state == State::LIVE
                    ? value_buffer
                    : nullptr,
                value_length
            },
            version,
            entry.state,
            destination_record_address
        );

        if (result != Result::OK) {
            return result;
        }

        new_record_addresses[i] =
            destination_record_address;

        ++new_record_count;
    }

    // --------------------------------------------------
    // 4. Write footer for the completed destination bank.
    // --------------------------------------------------

    const uint16_t destination_records_end =
        static_cast<uint16_t>(
            destination_append_address -
            bank_header_address(destination_bank)
        );

    result = write_bank_footer(
        destination_bank,
        new_bank_generation,
        new_record_count,
        destination_records_end
    );

    if (result != Result::OK) {
        return result;
    }

    // --------------------------------------------------
    // 5. Seal destination bank.
    // --------------------------------------------------

    result = write_bank_header(
        destination_bank,
        metadata::BankState::SEALED,
        new_bank_generation,
        new_record_count,
        destination_records_end
    );

    if (result != Result::OK) {
        return result;
    }

    // --------------------------------------------------
    // 6. Publish the new bank through the new superblock.
    //
    // This is the atomic authority switch.
    // --------------------------------------------------

    const uint32_t highest_sequence =
    destination_next_sequence == 0
        ? 0
        : destination_next_sequence - 1;

    result = write_superblock(
        new_superblock_generation,
        destination_bank,
        new_bank_generation,
        highest_sequence,
        new_record_count
    );

    if (result != Result::OK) {
        // Old bank remains authoritative because the new
        // superblock was never committed.
        return result;
    }
    

    // --------------------------------------------------
    // 7. Only NOW update runtime state.
    // --------------------------------------------------

    for (std::size_t i = 0;
         i < MAX_LOGICAL_KEYS;
         ++i) {

        if (!index_[i].valid) {
            continue;
        }

        index_[i].record_address =
            new_record_addresses[i];
    }

    active_bank_ =
        destination_bank;

    superblock_generation_ =
        new_superblock_generation;

    active_bank_generation_ =
        new_bank_generation;

    append_address_ =
        destination_append_address;

    next_sequence_ =
        destination_next_sequence;

    record_count_ =
        new_record_count;

    return Result::OK;
}

Result StorageManager::apply(
    KeyView key,
    ValueView value,
    Version version,
    State state
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
            return Result::INDEX_FULL;
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

    return Result::OK;
}
Result StorageManager::set(
    KeyView key,
    ValueView value,
    Version version
)
{
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
        return Result::CORRUPTED;
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
        return Result::CORRUPTED;
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

    eeprom::Result eeprom_result =
        eeprom_.begin(
            EEPROM_SDA_PIN,
            EEPROM_SCL_PIN
        );

    if (eeprom_result != eeprom::Result::OK) {
        return map_eeprom_result(eeprom_result);
    }

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
            Serial.println("Storage: no valid superblock, initializing fresh store");
        if (result != Result::OK) {
            return result;
        }

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
    Serial.println("Storage: valid superblock found, recovering existing store");
    superblock_generation_ = selected.generation;
    active_bank_generation_ =
        selected.active_bank_generation;
    next_sequence_ =
        selected.last_known_sequence + 1;
    record_count_ =
        selected.stored_record_count;

    // For this checkpoint we only support
    // reopening an empty persistent bank.
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
        return Result::CORRUPTED;
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
        return Result::CORRUPTED;
    }

    if ((header.state != metadata::BankState::OPEN &&
     header.state != metadata::BankState::SEALED) ||
    header.generation != selected.active_bank_generation ||
    footer.generation != header.generation) {
    return Result::CORRUPTED;
}
        const Result recovery_result =
        recover_active_bank();

    if (recovery_result != Result::OK) {
        return recovery_result;
    }

    

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

} // namespace storage
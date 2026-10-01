#include "dedup_manager.h"

#include <cstring>

namespace request {

DedupManager::DedupManager(
    std::uint64_t retention_ms
)
    : retention_ms_(retention_ms)
{
}

bool DedupManager::begin()
{
    if (retention_ms_ == 0) {
        return false;
    }

    for (std::size_t i = 0;
         i < MAX_DEDUP_ENTRIES;
         ++i) {

        entries_[i] = Entry{};
    }

    initialized_ = true;

    return true;
}

int DedupManager::find(
    Identity identity
) const
{
    for (std::size_t i = 0;
         i < MAX_DEDUP_ENTRIES;
         ++i) {

        if (entries_[i].state ==
            DedupState::FREE) {

            continue;
        }

        if (entries_[i].identity == identity) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int DedupManager::find_free() const
{
    for (std::size_t i = 0;
         i < MAX_DEDUP_ENTRIES;
         ++i) {

        if (entries_[i].state ==
            DedupState::FREE) {

            return static_cast<int>(i);
        }
    }

    return -1;
}

bool DedupManager::expired(
    const Entry& entry,
    std::uint64_t now_ms
) const
{
    if (entry.state !=
        DedupState::COMPLETED) {

        return false;
    }

    if (now_ms <
        entry.completed_at_ms) {

        return false;
    }

    return (
        now_ms - entry.completed_at_ms
    ) >= retention_ms_;
}

DedupResult DedupManager::begin_request(
    Identity identity,
    std::uint64_t now_ms
)
{
    if (!initialized_) {
        return DedupResult::NOT_READY;
    }

    if (!identity.valid()) {
        return DedupResult::INVALID_ARGUMENT;
    }

    /*
     * First clean expired completed entries.
     */
    expire(now_ms);

    const int existing =
        find(identity);

    if (existing >= 0) {

        const Entry& entry =
            entries_[existing];

        if (entry.state ==
            DedupState::IN_PROGRESS) {

            return DedupResult::
                DUPLICATE_IN_PROGRESS;
        }

        if (entry.state ==
            DedupState::COMPLETED) {

            return DedupResult::
                DUPLICATE_COMPLETED;
        }
    }

    const int slot = find_free();

    if (slot < 0) {
        return DedupResult::TABLE_FULL;
    }

    Entry& entry = entries_[slot];

    entry = Entry{};

    entry.identity = identity;
    entry.state =
        DedupState::IN_PROGRESS;

    return DedupResult::NEW_REQUEST;
}

DedupResult DedupManager::complete_request(
    Identity identity,
    const std::uint8_t* response,
    std::size_t response_length,
    std::uint64_t now_ms
)
{
    if (!initialized_) {
        return DedupResult::NOT_READY;
    }

    if (!identity.valid()) {
        return DedupResult::INVALID_ARGUMENT;
    }

    if (response_length >
        MAX_RESPONSE_SIZE) {

        return DedupResult::BUFFER_TOO_SMALL;
    }

    if (response_length > 0 &&
        response == nullptr) {

        return DedupResult::INVALID_ARGUMENT;
    }

    expire(now_ms);

    const int slot =
        find(identity);

    if (slot < 0) {
        return DedupResult::UNKNOWN_REQUEST;
    }

    Entry& entry = entries_[slot];

    if (entry.state ==
        DedupState::COMPLETED) {

        return DedupResult::
            DUPLICATE_COMPLETED;
    }

    if (entry.state !=
        DedupState::IN_PROGRESS) {

        return DedupResult::
            UNKNOWN_REQUEST;
    }

    if (response_length > 0) {
        std::memcpy(
            entry.response,
            response,
            response_length
        );
    }

    entry.response_length =
        static_cast<std::uint8_t>(
            response_length
        );

    entry.completed_at_ms =
        now_ms;

    entry.state =
        DedupState::COMPLETED;

    return DedupResult::NEW_REQUEST;
}

DedupResult DedupManager::get_state(
    Identity identity,
    DedupState& state,
    std::uint64_t now_ms
)
{
    state = DedupState::FREE;

    if (!initialized_) {
        return DedupResult::NOT_READY;
    }

    if (!identity.valid()) {
        return DedupResult::INVALID_ARGUMENT;
    }

    expire(now_ms);

    const int slot =
        find(identity);

    if (slot < 0) {
        return DedupResult::UNKNOWN_REQUEST;
    }

    state =
        entries_[slot].state;

    if (state ==
        DedupState::IN_PROGRESS) {

        return DedupResult::
            DUPLICATE_IN_PROGRESS;
    }

    return DedupResult::
        DUPLICATE_COMPLETED;
}

DedupResult DedupManager::get_cached_response(
    Identity identity,
    std::uint8_t* output,
    std::size_t output_capacity,
    std::size_t& output_length,
    std::uint64_t now_ms
)
{
    output_length = 0;

    if (!initialized_) {
        return DedupResult::NOT_READY;
    }

    if (!identity.valid()) {
        return DedupResult::INVALID_ARGUMENT;
    }

    if (output == nullptr) {
        return DedupResult::INVALID_ARGUMENT;
    }

    expire(now_ms);

    const int slot =
        find(identity);

    if (slot < 0) {
        return DedupResult::UNKNOWN_REQUEST;
    }

    const Entry& entry =
        entries_[slot];

    if (entry.state !=
        DedupState::COMPLETED) {

        return DedupResult::
            DUPLICATE_IN_PROGRESS;
    }

    if (output_capacity <
        entry.response_length) {

        return DedupResult::BUFFER_TOO_SMALL;
    }

    if (entry.response_length > 0) {
        std::memcpy(
            output,
            entry.response,
            entry.response_length
        );
    }

    output_length =
        entry.response_length;

    return DedupResult::
        DUPLICATE_COMPLETED;
}

void DedupManager::expire(
    std::uint64_t now_ms
)
{
    if (!initialized_) {
        return;
    }

    for (std::size_t i = 0;
         i < MAX_DEDUP_ENTRIES;
         ++i) {

        Entry& entry =
            entries_[i];

        if (entry.state !=
            DedupState::COMPLETED) {

            continue;
        }

        if (expired(
                entry,
                now_ms
            )) {

            entry = Entry{};
        }
    }
}

DedupResult DedupManager::remove(
    Identity identity
)
{
    if (!initialized_) {
        return DedupResult::NOT_READY;
    }

    if (!identity.valid()) {
        return DedupResult::INVALID_ARGUMENT;
    }

    const int slot =
        find(identity);

    if (slot < 0) {
        return DedupResult::UNKNOWN_REQUEST;
    }

    entries_[slot] = Entry{};

    return DedupResult::DUPLICATE_COMPLETED;
}

std::size_t DedupManager::active_count() const
{
    std::size_t count = 0;

    if (!initialized_) {
        return 0;
    }

    for (std::size_t i = 0;
         i < MAX_DEDUP_ENTRIES;
         ++i) {

        if (entries_[i].state !=
            DedupState::FREE) {

            ++count;
        }
    }

    return count;
}

bool DedupManager::ready() const
{
    return initialized_;
}

std::uint64_t DedupManager::retention_ms() const
{
    return retention_ms_;
}

} // namespace request
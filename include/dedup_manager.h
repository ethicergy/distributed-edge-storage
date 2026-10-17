#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "request_identity.h"

namespace request {

constexpr std::size_t MAX_DEDUP_ENTRIES =
    config::MAX_DEDUP_ENTRIES;
// Cached responses are acknowledgements (SET_ACK, DEL_ACK, REPL_ACK):
// 22-byte header + status + key length + key <= 56 bytes. GET
// responses are never cached (reads are idempotent).
constexpr std::size_t MAX_RESPONSE_SIZE = 64;

constexpr std::uint64_t DEFAULT_DEDUP_RETENTION_MS =
    10000;

enum class DedupState : std::uint8_t {
    FREE = 0,
    IN_PROGRESS,
    COMPLETED
};

enum class DedupResult : std::uint8_t {
    NEW_REQUEST = 0,
    DUPLICATE_IN_PROGRESS,
    DUPLICATE_COMPLETED,
    TABLE_FULL,
    INVALID_ARGUMENT,
    NOT_READY,
    BUFFER_TOO_SMALL,
    UNKNOWN_REQUEST
};

class DedupManager {
public:
    /*
     * in_progress_timeout_ms == 0 keeps IN_PROGRESS entries until
     * they are completed or removed. A non-zero value reclaims an
     * IN_PROGRESS entry that has been open for that long, so a code
     * path that forgets to complete a request cannot leak the slot.
     *
     * evict_after_ms != 0: when the table is full, the oldest
     * COMPLETED entry older than this is reused for a new request.
     * Set it to the longest time a sender can still retransmit, so an
     * evicted entry can no longer see a retry.
     */
    explicit DedupManager(
        std::uint64_t retention_ms =
            DEFAULT_DEDUP_RETENTION_MS,
        std::uint64_t in_progress_timeout_ms = 0,
        std::uint64_t evict_after_ms = 0
    );

    bool begin();

    DedupResult begin_request(
        Identity identity,
        std::uint64_t now_ms
    );

    DedupResult complete_request(
        Identity identity,
        const std::uint8_t* response,
        std::size_t response_length,
        std::uint64_t now_ms
    );

    DedupResult get_state(
        Identity identity,
        DedupState& state,
        std::uint64_t now_ms
    );

    DedupResult get_cached_response(
        Identity identity,
        std::uint8_t* output,
        std::size_t output_capacity,
        std::size_t& output_length,
        std::uint64_t now_ms
    );

    void expire(
        std::uint64_t now_ms
    );

    DedupResult remove(
        Identity identity
    );

    std::size_t active_count() const;

    bool ready() const;

    std::uint64_t retention_ms() const;

private:
    struct Entry {
        Identity identity{};

        DedupState state =
            DedupState::FREE;

        std::uint8_t response[
            MAX_RESPONSE_SIZE
        ]{};

        std::uint8_t response_length = 0;

        std::uint64_t started_at_ms = 0;
        std::uint64_t completed_at_ms = 0;
    };

    int find(
        Identity identity
    ) const;

    int find_free() const;

    int find_evictable(
        std::uint64_t now_ms
    ) const;

    bool expired(
        const Entry& entry,
        std::uint64_t now_ms
    ) const;

    std::uint64_t retention_ms_;
    std::uint64_t in_progress_timeout_ms_;
    std::uint64_t evict_after_ms_;

    Entry entries_[MAX_DEDUP_ENTRIES]{};

    bool initialized_ = false;
};

} // namespace request
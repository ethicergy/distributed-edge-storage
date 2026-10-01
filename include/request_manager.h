#pragma once

#include <cstddef>
#include <cstdint>

#include "request_identity.h"

namespace request {

constexpr std::size_t MAX_PENDING_REQUESTS = 8;
constexpr std::size_t MAX_PACKET_SIZE = 250;

constexpr std::uint64_t DEFAULT_INITIAL_TIMEOUT_MS = 500;
constexpr std::uint64_t DEFAULT_MAX_RETRY_TIMEOUT_MS = 2000;
constexpr std::uint8_t DEFAULT_MAX_RETRANSMISSIONS = 3;
constexpr std::uint64_t DEFAULT_OVERALL_DEADLINE_MS = 5000;

struct TimingConfig {
    std::uint64_t initial_timeout_ms =
        DEFAULT_INITIAL_TIMEOUT_MS;

    std::uint64_t max_retry_timeout_ms =
        DEFAULT_MAX_RETRY_TIMEOUT_MS;

    std::uint8_t max_retransmissions =
        DEFAULT_MAX_RETRANSMISSIONS;

    std::uint64_t overall_deadline_ms =
        DEFAULT_OVERALL_DEADLINE_MS;
};

enum class State : std::uint8_t {
    FREE = 0,
    PENDING,
    COMPLETED,
    FAILED
};

enum class Result : std::uint8_t {
    OK = 0,
    INVALID_ARGUMENT,
    NOT_READY,
    TABLE_FULL,
    DUPLICATE,
    UNKNOWN_REQUEST,
    ALREADY_TERMINAL,
    BUFFER_TOO_SMALL
};

enum class SendFailurePolicy : std::uint8_t {
    RETRY = 0
};

using SendFn = bool (*)(
    void* context,
    node::Id destination,
    const std::uint8_t* packet,
    std::size_t packet_length,
    bool retry
);

struct CompletionView {
    const std::uint8_t* data;
    std::size_t length;
};

struct RequestInfo {
    Identity identity;
    node::Id destination;
    State state;
    std::uint8_t retry_count;
};

class Manager {
public:
    explicit Manager(
        TimingConfig config = TimingConfig{}
    );

    Result begin();

    Result submit(
        Identity identity,
        node::Id destination,
        const std::uint8_t* packet,
        std::size_t packet_length,
        std::uint64_t now_ms,
        SendFn send_fn,
        void* send_context,
        bool& initial_send_ok
    );

    void tick(
        std::uint64_t now_ms
    );

    Result complete(
        Identity identity,
        const std::uint8_t* response,
        std::size_t response_length
    );

    Result get_state(
        Identity identity,
        State& state
    ) const;

    Result get_info(
        Identity identity,
        RequestInfo& info
    ) const;

    Result get_completion(
        Identity identity,
        std::uint8_t* output,
        std::size_t output_capacity,
        std::size_t& output_length
    ) const;

    Result release(
        Identity identity
    );

    std::size_t pending_count() const;

    bool ready() const;

private:
    struct Entry {
        Identity identity{};
        node::Id destination = node::INVALID;

        std::uint8_t packet[MAX_PACKET_SIZE]{};
        std::uint8_t packet_length = 0;

        std::uint8_t response[MAX_PACKET_SIZE]{};
        std::uint8_t response_length = 0;

        std::uint64_t created_at_ms = 0;
        std::uint64_t deadline_ms = 0;
        std::uint64_t next_retry_ms = 0;
        std::uint64_t current_retry_timeout_ms = 0;

        std::uint8_t retry_count = 0;

        State state = State::FREE;

        SendFn send_fn = nullptr;
        void* send_context = nullptr;
    };

    int find(
        Identity identity
    ) const;

    int find_free() const;

    std::uint64_t retry_delay(
        std::uint64_t current_timeout
    ) const;

    void fail_entry(
        Entry& entry
    );

    TimingConfig config_;
    Entry entries_[MAX_PENDING_REQUESTS]{};
    bool initialized_ = false;
};

} // namespace request
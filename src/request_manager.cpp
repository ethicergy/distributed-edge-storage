#include "request_manager.h"

#include <cstring>

namespace request {

Manager::Manager(TimingConfig config)
    : config_(config)
{
}

Result Manager::begin()
{
    if (config_.initial_timeout_ms == 0 ||
        config_.max_retry_timeout_ms == 0 ||
        config_.overall_deadline_ms == 0) {

        return Result::INVALID_ARGUMENT;
    }

    for (std::size_t i = 0;
         i < MAX_PENDING_REQUESTS;
         ++i) {

        entries_[i] = Entry{};
    }

    initialized_ = true;

    return Result::OK;
}

int Manager::find(
    Identity identity
) const
{
    for (std::size_t i = 0;
         i < MAX_PENDING_REQUESTS;
         ++i) {

        if (entries_[i].state == State::FREE) {
            continue;
        }

        if (entries_[i].identity == identity) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int Manager::find_free() const
{
    for (std::size_t i = 0;
         i < MAX_PENDING_REQUESTS;
         ++i) {

        if (entries_[i].state == State::FREE) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

std::uint64_t Manager::retry_delay(
    std::uint64_t current_timeout
) const
{
    if (current_timeout >=
        config_.max_retry_timeout_ms) {

        return config_.max_retry_timeout_ms;
    }

    if (current_timeout >
        config_.max_retry_timeout_ms / 2) {

        return config_.max_retry_timeout_ms;
    }

    return current_timeout * 2;
}

void Manager::fail_entry(
    Entry& entry
)
{
    entry.state = State::FAILED;
}

Result Manager::submit(
    Identity identity,
    node::Id destination,
    const std::uint8_t* packet,
    std::size_t packet_length,
    std::uint64_t now_ms,
    SendFn send_fn,
    void* send_context,
    bool& initial_send_ok
)
{
    initial_send_ok = false;

    if (!initialized_) {
        return Result::NOT_READY;
    }

    if (!identity.valid()) {
        return Result::INVALID_ARGUMENT;
    }

    /*
     * destination == identity.origin is allowed: a primary may
     * replicate to a replica that is also the client of the
     * original request.
     */
    if (destination < node::MIN ||
        destination > node::MAX) {

        return Result::INVALID_ARGUMENT;
    }

    if (packet == nullptr ||
        packet_length == 0 ||
        packet_length > MAX_PACKET_SIZE) {

        return Result::INVALID_ARGUMENT;
    }

    if (send_fn == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    if (find(identity) >= 0) {
        return Result::DUPLICATE;
    }

    const int slot = find_free();

    if (slot < 0) {
        return Result::TABLE_FULL;
    }

    Entry& entry = entries_[slot];

    entry = Entry{};

    entry.identity = identity;
    entry.destination = destination;

    std::memcpy(
        entry.packet,
        packet,
        packet_length
    );

    entry.packet_length =
        static_cast<std::uint8_t>(
            packet_length
        );

    entry.created_at_ms = now_ms;

    entry.deadline_ms =
        now_ms +
        config_.overall_deadline_ms;

    entry.current_retry_timeout_ms =
        config_.initial_timeout_ms;

    entry.next_retry_ms =
        now_ms +
        config_.initial_timeout_ms;

    entry.retry_count = 0;

    entry.state = State::PENDING;

    entry.send_fn = send_fn;
    entry.send_context = send_context;

    /*
     * The original transmission is not a retry.
     * If transport immediately rejects it, the logical
     * request remains pending and will retry normally.
     */
    initial_send_ok =
        send_fn(
            send_context,
            destination,
            entry.packet,
            packet_length,
            false
        );

    return Result::OK;
}

void Manager::tick(
    std::uint64_t now_ms
)
{
    if (!initialized_) {
        return;
    }

    for (std::size_t i = 0;
         i < MAX_PENDING_REQUESTS;
         ++i) {

        Entry& entry = entries_[i];

        if (entry.state != State::PENDING) {
            continue;
        }

        /*
         * Overall deadline always wins.
         */
        if (now_ms >= entry.deadline_ms) {
            fail_entry(entry);
            continue;
        }

        if (now_ms < entry.next_retry_ms) {
            continue;
        }

        /*
         * Maximum retransmissions exhausted.
         */
        if (entry.retry_count >=
            config_.max_retransmissions) {

            /*
             * Leave the request pending until the overall
             * deadline so the terminal timeout remains
             * exactly the configured deadline.
             */
            entry.next_retry_ms =
                entry.deadline_ms;

            continue;
        }

        ++entry.retry_count;

        const bool send_ok =
            entry.send_fn(
                entry.send_context,
                entry.destination,
                entry.packet,
                entry.packet_length,
                true
            );

        (void)send_ok;

        /*
         * Backoff applies after the retry just transmitted.
         */
        entry.current_retry_timeout_ms =
            retry_delay(
                entry.current_retry_timeout_ms
            );

        entry.next_retry_ms =
            now_ms +
            entry.current_retry_timeout_ms;

        /*
         * Never schedule beyond the terminal deadline.
         */
        if (entry.next_retry_ms >
            entry.deadline_ms) {

            entry.next_retry_ms =
                entry.deadline_ms;
        }
    }
}

Result Manager::complete(
    Identity identity,
    const std::uint8_t* response,
    std::size_t response_length
)
{
    if (!initialized_) {
        return Result::NOT_READY;
    }

    if (!identity.valid()) {
        return Result::INVALID_ARGUMENT;
    }

    const int slot = find(identity);

    if (slot < 0) {
        /*
         * No matching request exists.
         * Treat it as a late/unknown response.
         */
        return Result::UNKNOWN_REQUEST;
    }

    Entry& entry = entries_[slot];

    if (entry.state != State::PENDING) {
        /*
         * Terminal request:
         * late duplicate response is ignored.
         */
        return Result::ALREADY_TERMINAL;
    }

    if (response_length > MAX_PACKET_SIZE) {
        return Result::BUFFER_TOO_SMALL;
    }

    if (response_length > 0 &&
        response == nullptr) {

        return Result::INVALID_ARGUMENT;
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

    entry.state = State::COMPLETED;

    return Result::OK;
}

Result Manager::get_state(
    Identity identity,
    State& state
) const
{
    state = State::FREE;

    if (!initialized_) {
        return Result::NOT_READY;
    }

    const int slot = find(identity);

    if (slot < 0) {
        return Result::UNKNOWN_REQUEST;
    }

    state = entries_[slot].state;

    return Result::OK;
}

Result Manager::get_info(
    Identity identity,
    RequestInfo& info
) const
{
    info = {};

    if (!initialized_) {
        return Result::NOT_READY;
    }

    const int slot = find(identity);

    if (slot < 0) {
        return Result::UNKNOWN_REQUEST;
    }

    const Entry& entry = entries_[slot];

    info.identity = entry.identity;
    info.destination = entry.destination;
    info.state = entry.state;
    info.retry_count = entry.retry_count;

    return Result::OK;
}

Result Manager::get_completion(
    Identity identity,
    std::uint8_t* output,
    std::size_t output_capacity,
    std::size_t& output_length
) const
{
    output_length = 0;

    if (!initialized_) {
        return Result::NOT_READY;
    }

    if (output == nullptr) {
        return Result::INVALID_ARGUMENT;
    }

    const int slot = find(identity);

    if (slot < 0) {
        return Result::UNKNOWN_REQUEST;
    }

    const Entry& entry = entries_[slot];

    if (entry.state != State::COMPLETED) {
        return Result::ALREADY_TERMINAL;
    }

    if (output_capacity <
        entry.response_length) {

        return Result::BUFFER_TOO_SMALL;
    }

    if (entry.response_length > 0) {
        std::memcpy(
            output,
            entry.response,
            entry.response_length
        );
    }

    output_length = entry.response_length;

    return Result::OK;
}

Result Manager::release(
    Identity identity
)
{
    if (!initialized_) {
        return Result::NOT_READY;
    }

    const int slot = find(identity);

    if (slot < 0) {
        return Result::UNKNOWN_REQUEST;
    }

    entries_[slot] = Entry{};

    return Result::OK;
}

std::size_t Manager::pending_count() const
{
    std::size_t count = 0;

    if (!initialized_) {
        return 0;
    }

    for (std::size_t i = 0;
         i < MAX_PENDING_REQUESTS;
         ++i) {

        if (entries_[i].state == State::PENDING) {
            ++count;
        }
    }

    return count;
}

bool Manager::ready() const
{
    return initialized_;
}

} // namespace request
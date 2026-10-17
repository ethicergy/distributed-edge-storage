#include "node_runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "kv_response.h"
#include "reliable_packet.h"

#ifndef EDGE_FAULT_INJECTION
#define EDGE_FAULT_INJECTION 0
#endif

namespace edge {

namespace {

request::TimingConfig client_timing()
{
    request::TimingConfig timing{};
    timing.initial_timeout_ms = config::CLIENT_INITIAL_TIMEOUT_MS;
    timing.max_retry_timeout_ms = config::CLIENT_MAX_RETRY_TIMEOUT_MS;
    timing.max_retransmissions = config::CLIENT_MAX_RETRANSMISSIONS;
    timing.overall_deadline_ms = config::CLIENT_ATTEMPT_DEADLINE_MS;
    return timing;
}

request::TimingConfig replication_timing()
{
    request::TimingConfig timing{};
    timing.initial_timeout_ms = config::REPL_INITIAL_TIMEOUT_MS;
    timing.max_retry_timeout_ms = config::REPL_MAX_RETRY_TIMEOUT_MS;
    timing.max_retransmissions = config::REPL_MAX_RETRANSMISSIONS;
    timing.overall_deadline_ms = config::REPL_DEADLINE_MS;
    return timing;
}

char letter(node::Id id)
{
    return (id >= node::MIN && id <= node::MAX)
        ? static_cast<char>('A' + (id - node::MIN))
        : '?';
}

bool is_mutation(protocol::MessageType type)
{
    return type == protocol::MessageType::SET_REQ ||
           type == protocol::MessageType::DEL_REQ;
}

protocol::MessageType request_type_for(OpType type)
{
    switch (type) {
        case OpType::SET: return protocol::MessageType::SET_REQ;
        case OpType::GET: return protocol::MessageType::GET_REQ;
        case OpType::DEL: return protocol::MessageType::DEL_REQ;
    }

    return protocol::MessageType::GET_REQ;
}

protocol::KeyView request_key(const protocol::DecodedMessage& message)
{
    switch (message.header.msg_type) {
        case protocol::MessageType::SET_REQ:
            return message.payload.set_request.key;
        case protocol::MessageType::GET_REQ:
            return message.payload.get_request.key;
        case protocol::MessageType::DEL_REQ:
            return message.payload.delete_request.key;
        case protocol::MessageType::REPL_REQ:
            return message.payload.replication_request.key;
        case protocol::MessageType::RESYNC_DATA:
            return message.payload.resync_data.key;
        default:
            return protocol::KeyView{nullptr, 0};
    }
}

void format_key(
    const std::uint8_t* key,
    std::size_t length,
    char* out,
    std::size_t capacity
)
{
    std::size_t i = 0;

    for (; i < length && i + 1 < capacity; ++i) {
        const char c = static_cast<char>(key[i]);
        out[i] = (c >= 32 && c < 127) ? c : '.';
    }

    out[i] = '\0';
}

bool decode_buffer(
    const std::uint8_t* data,
    std::size_t length,
    protocol::DecodedMessage& message
)
{
    protocol::PacketView packet{};

    if (protocol::deserialize_packet(data, length, packet) !=
        protocol::Result::OK) {
        return false;
    }

    return protocol::decode_message(packet, message) ==
           protocol::Result::OK;
}

} // namespace

// ------------------------------------------------------------------
// Names
// ------------------------------------------------------------------

const char* to_string(NodeState state)
{
    switch (state) {
        case NodeState::BOOTING: return "BOOTING";
        case NodeState::CATCHUP: return "CATCHUP";
        case NodeState::ACTIVE: return "ACTIVE";
    }

    return "?";
}

const char* to_string(OpType type)
{
    switch (type) {
        case OpType::SET: return "SET";
        case OpType::GET: return "GET";
        case OpType::DEL: return "DEL";
    }

    return "?";
}

const char* to_string(ClientStatus status)
{
    switch (status) {
        case ClientStatus::OK: return "OK";
        case ClientStatus::NOT_FOUND: return "NOT_FOUND";
        case ClientStatus::UNDER_REPLICATED: return "UNDER_REPLICATED";
        case ClientStatus::STORAGE_ERROR: return "STORAGE_ERROR";
        case ClientStatus::INVALID_REQUEST: return "INVALID_REQUEST";
        case ClientStatus::KEY_TOO_LARGE: return "KEY_TOO_LARGE";
        case ClientStatus::VALUE_TOO_LARGE: return "VALUE_TOO_LARGE";
        case ClientStatus::NOT_READY: return "NOT_READY";
        case ClientStatus::BUSY: return "BUSY";
        case ClientStatus::NO_ROUTE: return "NO_ROUTE";
        case ClientStatus::STALE_VERSION: return "STALE_VERSION";
        case ClientStatus::TIMEOUT: return "TIMEOUT";
    }

    return "?";
}

// ------------------------------------------------------------------
// Construction / boot
// ------------------------------------------------------------------

Runtime::Runtime(
    node::Id local_node_id,
    storage::StorageManager& storage,
    Link& link,
    hlc::PhysicalNowFn physical_now
)
    : local_node_id_(local_node_id),
      storage_(storage),
      link_(link),
      clock_(physical_now),
      membership_(local_node_id, config::MEMBERSHIP_TIMEOUT_MS),
      engine_(local_node_id, storage, placement_),
      client_requests_(client_timing()),
      replication_requests_(replication_timing()),
      dedup_(
          config::DEDUP_RETENTION_MS,
          config::DEDUP_IN_PROGRESS_TIMEOUT_MS,
          config::DEDUP_EVICT_AFTER_MS
      ),
      ae_(local_node_id)
{
}

void Runtime::set_log(LogFn log, void* context, LogLevel level)
{
    log_ = log;
    log_context_ = context;
    log_level_ = level;
}

void Runtime::logf(LogLevel level, const char* format, ...)
{
    if (log_ == nullptr ||
        level == LogLevel::OFF ||
        static_cast<std::uint8_t>(level) >
            static_cast<std::uint8_t>(log_level_)) {
        return;
    }

    char line[192];

    const int prefix = std::snprintf(
        line,
        sizeof(line),
        "[%c] ",
        letter(local_node_id_)
    );

    va_list args;
    va_start(args, format);
    std::vsnprintf(
        line + prefix,
        sizeof(line) - static_cast<std::size_t>(prefix),
        format,
        args
    );
    va_end(args);

    log_(log_context_, line);
}

bool Runtime::begin(std::uint64_t now_ms, std::uint32_t random_seed)
{
    if (local_node_id_ < node::MIN || local_node_id_ > node::MAX) {
        return false;
    }

    storage::Result storage_result = storage_.begin();

    if (storage_result == storage::Result::CORRUPTED &&
        config::STORAGE_AUTO_FORMAT_ON_CORRUPTION) {

        logf(
            LogLevel::INFO,
            "storage CORRUPTED: reformatting; replicas will repopulate it"
        );

        storage_result = storage_.format();
    }

    if (storage_result != storage::Result::OK) {
        logf(
            LogLevel::INFO,
            "storage begin failed (%u)",
            static_cast<unsigned>(storage_result)
        );
        return false;
    }

    const storage::Stats stats = storage_.stats();

    static const char* const BOOT_KINDS[] = {
        "none", "fresh format", "recovered", "recovered (torn tail discarded)"
    };

    logf(
        LogLevel::INFO,
        "storage %s: %u live, %u tombstones, %u/%u log bytes used",
        BOOT_KINDS[static_cast<std::uint8_t>(stats.boot_kind)],
        static_cast<unsigned>(stats.live_keys),
        static_cast<unsigned>(stats.tombstones),
        static_cast<unsigned>(stats.log_used_bytes),
        static_cast<unsigned>(stats.log_capacity_bytes)
    );

    // Reboot rule: the HLC starts at or above every persisted version.
    storage::Hlc max_hlc = 0;
    (void)storage_.max_persisted_hlc(max_hlc);

    if (!clock_.begin(max_hlc) ||
        !membership_.begin(now_ms) ||
        !placement_.begin() ||
        !engine_.begin() ||
        !ids_.begin(local_node_id_, random_seed) ||
        client_requests_.begin() != request::Result::OK ||
        replication_requests_.begin() != request::Result::OK ||
        !dedup_.begin()) {

        logf(LogLevel::INFO, "runtime begin failed");
        return false;
    }

    ae_.reset();

    for (ClientOp& op : ops_) {
        op = ClientOp{};
    }

    for (PendingReplication& pending : replications_) {
        pending = PendingReplication{};
    }

    counters_ = Counters{};
    last_view_ = 0;
    started_ = true;

    enter_state(NodeState::BOOTING, now_ms);

    return true;
}

void Runtime::enter_state(NodeState state, std::uint64_t now_ms)
{
    state_ = state;
    state_since_ms_ = now_ms;
    catchup_rounds_sent_ = 0;

    if (state == NodeState::ACTIVE) {
        // First heartbeat right away; first anti-entropy round soon,
        // staggered by node id so the nodes do not all talk at once.
        last_heartbeat_ms_ = 0;
        next_ae_round_ms_ =
            now_ms + config::AE_PERIOD_MS / 4 +
            static_cast<std::uint64_t>(local_node_id_) * 700u;
        next_gc_ms_ = now_ms + config::AE_PERIOD_MS;
    }

    logf(LogLevel::INFO, "state -> %s", to_string(state));
}

placement::AliveMask Runtime::view(std::uint64_t now_ms) const
{
    return placement::alive_mask(membership_, now_ms);
}

// ------------------------------------------------------------------
// Radio input
// ------------------------------------------------------------------

void Runtime::on_frame(
    const std::uint8_t* data,
    std::size_t length,
    node::Id link_source,
    std::uint64_t now_ms
)
{
    if (!started_ || data == nullptr) {
        return;
    }

    ++counters_.frames_rx;

    protocol::PacketView packet{};

    if (protocol::deserialize_packet(data, length, packet) !=
        protocol::Result::OK) {
        ++counters_.frames_rx_invalid;
        return;
    }

    const protocol::PacketHeader& header = packet.header;

    // The link layer knows who really sent the frame.
    if (link_source != node::INVALID &&
        link_source != header.sender_node_id) {
        ++counters_.frames_rx_spoofed;
        return;
    }

    if (header.sender_node_id == local_node_id_) {
        return;
    }

    if (header.destination_node_id != local_node_id_ &&
        header.destination_node_id != protocol::NODE_ID_BROADCAST) {
        return;
    }

    protocol::DecodedMessage message{};

    if (protocol::decode_message(packet, message) != protocol::Result::OK) {
        ++counters_.frames_rx_invalid;
        return;
    }

    // Every packet carries the sender's clock; merging it keeps the
    // cluster's clocks converged (heartbeats included).
    clock_.adopt_physical(header.hlc);
    clock_.on_receive(header.hlc);

    switch (header.msg_type) {
        case protocol::MessageType::HEARTBEAT:
            ++counters_.heartbeats_rx;
            ++counters_.heartbeats_from[header.sender_node_id - node::MIN];
            membership_.heartbeat_received(header.sender_node_id, now_ms);
            break;

        case protocol::MessageType::SET_REQ:
        case protocol::MessageType::GET_REQ:
        case protocol::MessageType::DEL_REQ:
            handle_client_request(message, now_ms);
            break;

        case protocol::MessageType::SET_ACK:
        case protocol::MessageType::GET_RESP:
        case protocol::MessageType::DEL_ACK:
            handle_client_response(message, data, length, now_ms);
            break;

        case protocol::MessageType::REPL_REQ:
            handle_replication_request(message, now_ms);
            break;

        case protocol::MessageType::REPL_ACK:
            handle_replication_ack(message, data, length, now_ms);
            break;

        case protocol::MessageType::VERSION_TABLE:
            handle_version_table(message, now_ms);
            break;

        case protocol::MessageType::RESYNC_DATA:
            handle_resync(message, now_ms);
            break;
    }

    deliver_completions();
}

// ------------------------------------------------------------------
// Server side: client requests on the primary
// ------------------------------------------------------------------

void Runtime::handle_client_request(
    const protocol::DecodedMessage& message,
    std::uint64_t now_ms
)
{
    const protocol::PacketHeader& header = message.header;

    const request::Identity identity{
        header.origin_node_id,
        header.request_id
    };

    if (!identity.valid()) {
        ++counters_.frames_rx_invalid;
        return;
    }

    const protocol::KeyView key = request_key(message);

    char key_text[storage::KEY_MAX + 1];
    format_key(key.data, key.len, key_text, sizeof(key_text));

    // Not serving yet (boot gate): send the client to another owner.
    // Not cached, so a retry after we became ACTIVE is served.
    if (state_ != NodeState::ACTIVE) {
        ++counters_.requests_rejected_not_ready;

        kv::OperationResponse response{};
        response.result = kv::Result::NOT_READY;
        if (key.data != nullptr && key.len <= storage::KEY_MAX) {
            response.key_length = key.len;
            std::memcpy(response.key, key.data, key.len);
        }

        respond_to_client(header, identity, response, false, now_ms);
        return;
    }

    /*
     * Exactly-once matters for mutations. A GET is idempotent: a
     * retransmitted GET is simply executed again, which keeps the
     * dedup table for SET/DEL and replication traffic.
     */
    const bool mutation = is_mutation(header.msg_type);

    const request::DedupResult dedup =
        mutation
            ? dedup_.begin_request(identity, now_ms)
            : request::DedupResult::NEW_REQUEST;

    switch (dedup) {
        case request::DedupResult::NEW_REQUEST:
            break;

        case request::DedupResult::DUPLICATE_IN_PROGRESS:
            ++counters_.dedup_suppressed;
            logf(
                LogLevel::DEBUG,
                "%s %s from %c#%lu: duplicate in progress, suppressed",
                packet::to_string(header.msg_type),
                key_text,
                letter(header.origin_node_id),
                static_cast<unsigned long>(header.request_id)
            );
            return;

        case request::DedupResult::DUPLICATE_COMPLETED: {
            std::uint8_t cached[protocol::MAX_PACKET_SIZE];
            std::size_t cached_length = 0;

            if (dedup_.get_cached_response(
                    identity,
                    cached,
                    sizeof(cached),
                    cached_length,
                    now_ms
                ) == request::DedupResult::DUPLICATE_COMPLETED) {

                ++counters_.dedup_replays;

                logf(
                    LogLevel::INFO,
                    "%s %s from %c#%lu: duplicate, cached response replayed",
                    packet::to_string(header.msg_type),
                    key_text,
                    letter(header.origin_node_id),
                    static_cast<unsigned long>(header.request_id)
                );

                deliver(header.origin_node_id, cached, cached_length, now_ms);
            }

            return;
        }

        case request::DedupResult::TABLE_FULL: {
            // Defined answer instead of silence; the client retries.
            ++counters_.dedup_table_full;

            kv::OperationResponse response{};
            response.result = kv::Result::STORAGE_ERROR;

            if (key.data != nullptr && key.len <= storage::KEY_MAX) {
                response.key_length = key.len;
                std::memcpy(response.key, key.data, key.len);
            }

            logf(LogLevel::INFO, "dedup table full: %s answered STORAGE_ERROR", key_text);
            respond_to_client(header, identity, response, false, now_ms);
            return;
        }

        default:
            return;
    }

    // The primary assigns the version from its own clock, which has
    // already merged the request's HLC (docs/DESIGN.md, "Versions").
    storage::Version version{0, local_node_id_};

    if (mutation) {
        version.hlc = clock_.on_local_event();
    }

    kv::OperationResponse response{};

    engine_.execute(message, view(now_ms), version, response);

    ++counters_.requests_executed;

    logf(
        LogLevel::DEBUG,
        "%s %s from %c#%lu -> %s",
        packet::to_string(header.msg_type),
        key_text,
        letter(header.origin_node_id),
        static_cast<unsigned long>(header.request_id),
        kv::to_string(response.result)
    );

    if (response.result == kv::Result::STORAGE_ERROR) {
        ++counters_.storage_errors;
        logf(LogLevel::INFO, "storage error on %s", key_text);
    }

    if (mutation && response.result == kv::Result::OK) {
        note_local_change(response.key, response.key_length, now_ms);

        if (response.assignment.replica != node::INVALID &&
            begin_replication(message, identity, response, version, now_ms)) {
            // Answered when REPL_ACK arrives or replication times out.
            return;
        }

        // Decision D5: committed on the primary only. Anti-entropy
        // copies it once a replica is reachable.
        logf(
            LogLevel::INFO,
            "%s %s stored without replica (%s)",
            packet::to_string(header.msg_type),
            key_text,
            response.assignment.replica == node::INVALID
                ? "no other node alive"
                : "replication could not start"
        );

        response.result = kv::Result::REPLICATION_FAILED;
    }

    respond_to_client(header, identity, response, mutation, now_ms);
}

void Runtime::respond_to_client(
    const protocol::PacketHeader& request_header,
    request::Identity identity,
    const kv::OperationResponse& response,
    bool cache,
    std::uint64_t now_ms
)
{
    packet::Buffer out{};

    if (!kv::ResponseBuilder::build(
            request_header,
            local_node_id_,
            clock_.on_local_event(),
            response,
            out.data,
            sizeof(out.data),
            out.length
        )) {

        logf(LogLevel::INFO, "response build failed");

        if (cache) {
            dedup_.remove(identity);
        }

        return;
    }

    if (cache) {
        dedup_.complete_request(identity, out.data, out.length, now_ms);
    }

#if EDGE_FAULT_INJECTION
    if (faults_.drop_client_responses > 0 &&
        request_header.origin_node_id != local_node_id_) {
        --faults_.drop_client_responses;
        ++counters_.faults_injected;
        logf(
            LogLevel::INFO,
            "FAULT: dropped %s to %c#%lu (result cached)",
            packet::to_string(static_cast<protocol::MessageType>(out.data[1])),
            letter(request_header.origin_node_id),
            static_cast<unsigned long>(request_header.request_id)
        );
        return;
    }
#endif

    deliver(request_header.origin_node_id, out.data, out.length, now_ms);
}

// ------------------------------------------------------------------
// Replication (primary side)
// ------------------------------------------------------------------

bool Runtime::begin_replication(
    const protocol::DecodedMessage& message,
    request::Identity client_identity,
    const kv::OperationResponse& response,
    storage::Version version,
    std::uint64_t now_ms
)
{
    PendingReplication* slot = nullptr;

    for (PendingReplication& pending : replications_) {
        if (!pending.active) {
            slot = &pending;
            break;
        }
    }

    if (slot == nullptr) {
        return false;
    }

    const bool tombstone =
        message.header.msg_type == protocol::MessageType::DEL_REQ;

    const protocol::KeyView key{response.key, response.key_length};

    const protocol::ValueView value =
        tombstone
            ? protocol::ValueView{nullptr, 0}
            : message.payload.set_request.value;

    const request::Identity replication_identity = ids_.next();

    // REPL_REQ: origin = this primary = version origin, hlc = version.
    const packet::Route route{
        replication_identity.request_id,
        local_node_id_,
        local_node_id_,
        response.assignment.replica,
        version.hlc,
        static_cast<std::uint8_t>(
            protocol::FLAG_REPLICATED |
            (tombstone ? protocol::FLAG_TOMBSTONE : 0)
        )
    };

    packet::Buffer out{};

    if (!packet::build_request(
            protocol::MessageType::REPL_REQ,
            route,
            key,
            value,
            out
        )) {
        return false;
    }

    bool initial_send_ok = false;

    if (replication_requests_.submit(
            replication_identity,
            response.assignment.replica,
            out.data,
            out.length,
            now_ms,
            reliable_send,
            this,
            initial_send_ok
        ) != request::Result::OK) {
        return false;
    }

    slot->active = true;
    slot->replication_identity = replication_identity;
    slot->client_identity = client_identity;
    slot->client_header = message.header;
    slot->response = response;
    slot->replica = response.assignment.replica;

    ++counters_.replications_started;

    logf(
        LogLevel::DEBUG,
        "REPL_REQ #%lu -> %c",
        static_cast<unsigned long>(replication_identity.request_id),
        letter(response.assignment.replica)
    );

    return true;
}

void Runtime::handle_replication_ack(
    const protocol::DecodedMessage& message,
    const std::uint8_t* raw,
    std::size_t raw_length,
    std::uint64_t now_ms
)
{
    const request::Identity identity{
        message.header.origin_node_id,
        message.header.request_id
    };

    if (identity.origin != local_node_id_ ||
        replication_requests_.complete(identity, raw, raw_length) !=
            request::Result::OK) {
        // Late or unknown ACK (request already terminal).
        return;
    }

    replication_requests_.release(identity);

    for (PendingReplication& pending : replications_) {
        if (!pending.active || pending.replication_identity != identity) {
            continue;
        }

        const protocol::StatusCode status =
            message.payload.replication_ack.status;

        char key_text[storage::KEY_MAX + 1];
        format_key(
            pending.response.key,
            pending.response.key_length,
            key_text,
            sizeof(key_text)
        );

        if (status == protocol::StatusCode::OK) {
            ++counters_.replications_ok;
            finish_replication(pending, kv::Result::OK, now_ms);
        } else if (status == protocol::StatusCode::STALE_VERSION) {
            /*
             * The replica holds a NEWER version than the one we just
             * assigned (rule for audit 5.2): our write did not win, so
             * it is not reported as OK. Our clock merged the ACK's HLC,
             * which is >= the replica's version, so the client's next
             * attempt gets a version that wins.
             */
            ++counters_.replications_stale;
            logf(
                LogLevel::INFO,
                "replica %c has a newer version of %s: client gets STALE_VERSION",
                letter(pending.replica),
                key_text
            );
            finish_replication(pending, kv::Result::STALE_VERSION, now_ms);
        } else {
            ++counters_.replications_failed;
            logf(
                LogLevel::INFO,
                "replica %c answered %s for %s: under-replicated",
                letter(pending.replica),
                packet::to_string(status),
                key_text
            );
            finish_replication(pending, kv::Result::REPLICATION_FAILED, now_ms);
        }

        return;
    }
}

void Runtime::finish_replication(
    PendingReplication& pending,
    kv::Result client_result,
    std::uint64_t now_ms
)
{
    kv::OperationResponse response = pending.response;
    response.result = client_result;

    const protocol::PacketHeader header = pending.client_header;
    const request::Identity identity = pending.client_identity;

    pending = PendingReplication{};

    respond_to_client(header, identity, response, true, now_ms);
}

void Runtime::tick_replications(std::uint64_t now_ms)
{
    replication_requests_.tick(now_ms);

    for (PendingReplication& pending : replications_) {
        if (!pending.active) {
            continue;
        }

        request::State state = request::State::FREE;

        const request::Result result =
            replication_requests_.get_state(pending.replication_identity, state);

        if (result == request::Result::OK && state == request::State::PENDING) {
            continue;
        }

        replication_requests_.release(pending.replication_identity);

        ++counters_.replications_failed;

        char key_text[storage::KEY_MAX + 1];
        format_key(
            pending.response.key,
            pending.response.key_length,
            key_text,
            sizeof(key_text)
        );

        logf(
            LogLevel::INFO,
            "replica %c did not answer for %s: under-replicated",
            letter(pending.replica),
            key_text
        );

        finish_replication(pending, kv::Result::REPLICATION_FAILED, now_ms);
    }
}

// ------------------------------------------------------------------
// Replica side / anti-entropy input
// ------------------------------------------------------------------

void Runtime::handle_replication_request(
    const protocol::DecodedMessage& message,
    std::uint64_t now_ms
)
{
    const protocol::PacketHeader& header = message.header;

    const request::Identity identity{
        header.origin_node_id,
        header.request_id
    };

    if (!identity.valid()) {
        ++counters_.frames_rx_invalid;
        return;
    }

    const request::DedupResult dedup =
        dedup_.begin_request(identity, now_ms);

    if (dedup == request::DedupResult::DUPLICATE_IN_PROGRESS) {
        ++counters_.dedup_suppressed;
        return;
    }

    if (dedup == request::DedupResult::DUPLICATE_COMPLETED) {
        std::uint8_t cached[protocol::MAX_PACKET_SIZE];
        std::size_t cached_length = 0;

        if (dedup_.get_cached_response(
                identity,
                cached,
                sizeof(cached),
                cached_length,
                now_ms
            ) == request::DedupResult::DUPLICATE_COMPLETED) {

            ++counters_.dedup_replays;
            logf(
                LogLevel::INFO,
                "REPL_REQ %c#%lu duplicate: cached REPL_ACK replayed",
                letter(header.origin_node_id),
                static_cast<unsigned long>(header.request_id)
            );
            send(header.sender_node_id, cached, cached_length);
        }

        return;
    }

    kv::OperationResponse response{};
    bool cache = true;

    if (dedup == request::DedupResult::NEW_REQUEST) {
        engine_.apply_versioned(message, response);
    } else {
        ++counters_.dedup_table_full;
        response.result = kv::Result::STORAGE_ERROR;
        cache = false;
    }

    if (response.result == kv::Result::OK) {
        ++counters_.replicas_applied;
        note_local_change(response.key, response.key_length, now_ms);
    } else if (response.result == kv::Result::STALE_VERSION) {
        ++counters_.replicas_stale;
    } else if (response.result == kv::Result::STORAGE_ERROR) {
        ++counters_.storage_errors;
    }

    char key_text[storage::KEY_MAX + 1];
    format_key(response.key, response.key_length, key_text, sizeof(key_text));

    logf(
        LogLevel::DEBUG,
        "REPL_REQ %s%s from %c -> %s",
        (header.flags & protocol::FLAG_TOMBSTONE) ? "(tombstone) " : "",
        key_text,
        letter(header.sender_node_id),
        kv::to_string(response.result)
    );

    packet::Buffer out{};

    if (!kv::ResponseBuilder::build(
            header,
            local_node_id_,
            clock_.on_local_event(),
            response,
            out.data,
            sizeof(out.data),
            out.length
        )) {
        if (cache) {
            dedup_.remove(identity);
        }
        return;
    }

    if (cache) {
        dedup_.complete_request(identity, out.data, out.length, now_ms);
    }

#if EDGE_FAULT_INJECTION
    if (faults_.drop_repl_acks > 0) {
        --faults_.drop_repl_acks;
        ++counters_.faults_injected;
        logf(
            LogLevel::INFO,
            "FAULT: dropped REPL_ACK #%lu (applied and cached)",
            static_cast<unsigned long>(header.request_id)
        );
        return;
    }
#endif

    send(header.sender_node_id, out.data, out.length);
}

void Runtime::handle_resync(
    const protocol::DecodedMessage& message,
    std::uint64_t now_ms
)
{
    kv::OperationResponse response{};

    engine_.apply_versioned(message, response);

    char key_text[storage::KEY_MAX + 1];
    format_key(response.key, response.key_length, key_text, sizeof(key_text));

    if (response.result == kv::Result::OK) {
        ++counters_.resync_applied;
        note_local_change(response.key, response.key_length, now_ms);

        logf(
            LogLevel::INFO,
            "anti-entropy: %s %s repaired from %c",
            (message.header.flags & protocol::FLAG_TOMBSTONE) ? "tombstone" : "key",
            key_text,
            letter(message.header.sender_node_id)
        );
    } else if (response.result == kv::Result::STALE_VERSION) {
        ++counters_.resync_stale;

        logf(
            LogLevel::DEBUG,
            "anti-entropy: %s from %c rejected as stale",
            key_text,
            letter(message.header.sender_node_id)
        );
    } else {
        if (response.result == kv::Result::STORAGE_ERROR) {
            ++counters_.storage_errors;
        }

        logf(
            LogLevel::INFO,
            "anti-entropy: %s from %c failed (%s)",
            key_text,
            letter(message.header.sender_node_id),
            kv::to_string(response.result)
        );
    }
}

void Runtime::handle_version_table(
    const protocol::DecodedMessage& message,
    std::uint64_t now_ms
)
{
    ae_.on_chunk(
        message.header.sender_node_id,
        message.payload.version_table,
        storage_,
        placement_,
        view(now_ms),
        now_ms
    );
}

void Runtime::note_local_change(
    const std::uint8_t* key,
    std::size_t key_length,
    std::uint64_t now_ms
)
{
    const int slot = storage_.find_slot(storage::KeyView{key, key_length});

    if (slot >= 0) {
        ae_.on_local_change(static_cast<std::size_t>(slot), now_ms);
    }
}

// ------------------------------------------------------------------
// Sending
// ------------------------------------------------------------------

void Runtime::deliver(
    node::Id destination,
    const std::uint8_t* data,
    std::size_t length,
    std::uint64_t now_ms
)
{
    if (destination != local_node_id_) {
        send(destination, data, length);
        return;
    }

    // Local fast path: our own client asked us. No radio.
    protocol::DecodedMessage message{};

    if (!decode_buffer(data, length, message)) {
        return;
    }

    switch (message.header.msg_type) {
        case protocol::MessageType::SET_ACK:
        case protocol::MessageType::GET_RESP:
        case protocol::MessageType::DEL_ACK:
            handle_client_response(message, data, length, now_ms);
            break;

        default:
            break;
    }
}

bool Runtime::send(
    node::Id destination,
    const std::uint8_t* data,
    std::size_t length
)
{
    ++counters_.frames_tx;

    const bool ok = link_.unicast(destination, data, length);

    if (!ok) {
        ++counters_.frames_tx_failed;
    }

    return ok;
}

bool Runtime::reliable_send(
    void* context,
    node::Id destination,
    const std::uint8_t* packet,
    std::size_t packet_length,
    bool retry
)
{
    Runtime* self = static_cast<Runtime*>(context);

    if (self == nullptr || packet == nullptr || packet_length < 2) {
        return false;
    }

    if (!retry) {
#if EDGE_FAULT_INJECTION
        const auto type = static_cast<protocol::MessageType>(packet[1]);
        const bool client_request =
            type == protocol::MessageType::SET_REQ ||
            type == protocol::MessageType::GET_REQ ||
            type == protocol::MessageType::DEL_REQ;

        if (client_request && self->faults_.drop_client_requests > 0) {
            --self->faults_.drop_client_requests;
            ++self->counters_.faults_injected;
            self->logf(LogLevel::INFO, "FAULT: dropped first transmission of %s", packet::to_string(type));
            return true;
        }

        if (type == protocol::MessageType::REPL_REQ &&
            self->faults_.drop_repl_requests > 0) {
            --self->faults_.drop_repl_requests;
            ++self->counters_.faults_injected;
            self->logf(LogLevel::INFO, "FAULT: dropped first transmission of REPL_REQ");
            return true;
        }

        if (client_request && self->faults_.duplicate_client_requests > 0) {
            --self->faults_.duplicate_client_requests;
            ++self->counters_.faults_injected;
            self->logf(LogLevel::INFO, "FAULT: %s sent twice", packet::to_string(type));
            self->send(destination, packet, packet_length);
        }
#endif

        return self->send(destination, packet, packet_length);
    }

    // Retransmission: identical packet, FLAG_RETRY set (rule R4).
    std::uint8_t retry_packet[protocol::MAX_PACKET_SIZE];
    std::size_t retry_length = 0;

    if (request::prepare_retry(
            packet,
            packet_length,
            retry_packet,
            sizeof(retry_packet),
            retry_length
        ) != request::PacketResult::OK) {
        return false;
    }

    self->logf(
        LogLevel::DEBUG,
        "retry %s -> %c",
        packet::to_string(static_cast<protocol::MessageType>(packet[1])),
        letter(destination)
    );

    return self->send(destination, retry_packet, retry_length);
}

// ------------------------------------------------------------------
// Client API
// ------------------------------------------------------------------

ClientStatus Runtime::submit(
    OpType type,
    const std::uint8_t* key,
    std::size_t key_length,
    const std::uint8_t* value,
    std::size_t value_length,
    CompletionFn completion,
    void* context,
    std::uint32_t tag,
    std::uint64_t now_ms
)
{
    if (!started_ || state_ != NodeState::ACTIVE) {
        return ClientStatus::NOT_READY;
    }

    if (key == nullptr || key_length == 0) {
        return ClientStatus::INVALID_REQUEST;
    }

    if (key_length > storage::KEY_MAX) {
        return ClientStatus::KEY_TOO_LARGE;
    }

    if (type != OpType::SET) {
        value_length = 0;
    }

    if (value_length > storage::VALUE_MAX) {
        return ClientStatus::VALUE_TOO_LARGE;
    }

    if (value_length > 0 && value == nullptr) {
        return ClientStatus::INVALID_REQUEST;
    }

    for (ClientOp& op : ops_) {
        if (op.phase != OpPhase::FREE) {
            continue;
        }

        op = ClientOp{};
        op.phase = OpPhase::WAITING;
        op.type = type;
        std::memcpy(op.key, key, key_length);
        op.key_length = static_cast<std::uint8_t>(key_length);

        if (value_length > 0) {
            std::memcpy(op.value, value, value_length);
        }

        op.value_length = static_cast<std::uint8_t>(value_length);
        op.completion = completion;
        op.context = context;
        op.tag = tag;
        op.started_ms = now_ms;
        op.next_attempt_ms = now_ms;

        return ClientStatus::OK;
    }

    return ClientStatus::BUSY;
}

std::size_t Runtime::free_client_slots() const
{
    std::size_t free_slots = 0;

    for (const ClientOp& op : ops_) {
        if (op.phase == OpPhase::FREE) {
            ++free_slots;
        }
    }

    return free_slots;
}

Runtime::ClientOp* Runtime::find_op(request::Identity identity)
{
    for (ClientOp& op : ops_) {
        if ((op.phase == OpPhase::REMOTE || op.phase == OpPhase::LOCAL) &&
            op.identity == identity) {
            return &op;
        }
    }

    return nullptr;
}

void Runtime::start_attempt(ClientOp& op, std::uint64_t now_ms)
{
    if (state_ != NodeState::ACTIVE) {
        finish_op(op, ClientStatus::NOT_READY, node::INVALID, nullptr, 0, now_ms);
        return;
    }

    if (op.attempts >= config::CLIENT_MAX_ATTEMPTS) {
        finish_op(op, op.last_failure, node::INVALID, nullptr, 0, now_ms);
        return;
    }

    ++op.attempts;
    ++counters_.client_attempts;

    placement::Assignment assignment{};

    if (!placement_.locate(op.key, op.key_length, view(now_ms), assignment)) {
        finish_op(op, ClientStatus::NO_ROUTE, node::INVALID, nullptr, 0, now_ms);
        return;
    }

    // After NOT_RESPONSIBLE from the primary we try the other owner
    // once; placement is recomputed on every attempt (rule 5.7).
    node::Id target = assignment.primary;

    if (target == op.avoid && assignment.replica != node::INVALID) {
        target = assignment.replica;
    }

    op.avoid = node::INVALID;
    op.target = target;
    op.identity = ids_.next();
    op.attempt_started_ms = now_ms;

    const packet::Route route{
        op.identity.request_id,
        local_node_id_,
        local_node_id_,
        target,
        clock_.on_local_event(),
        0
    };

    packet::Buffer out{};

    if (!packet::build_request(
            request_type_for(op.type),
            route,
            protocol::KeyView{op.key, op.key_length},
            protocol::ValueView{op.value, op.value_length},
            out
        )) {
        finish_op(op, ClientStatus::INVALID_REQUEST, node::INVALID, nullptr, 0, now_ms);
        return;
    }

    if (target == local_node_id_) {
        // Local fast path: same server code, no radio.
        ++counters_.client_local_fast_path;
        op.phase = OpPhase::LOCAL;

        protocol::DecodedMessage message{};

        if (!decode_buffer(out.data, out.length, message)) {
            finish_op(op, ClientStatus::INVALID_REQUEST, node::INVALID, nullptr, 0, now_ms);
            return;
        }

        handle_client_request(message, now_ms);
        return;
    }

    op.phase = OpPhase::REMOTE;

    bool initial_send_ok = false;

    const request::Result result = client_requests_.submit(
        op.identity,
        target,
        out.data,
        out.length,
        now_ms,
        reliable_send,
        this,
        initial_send_ok
    );

    if (result != request::Result::OK) {
        attempt_failed(op, ClientStatus::BUSY, node::INVALID, false, now_ms);
    }
}

void Runtime::attempt_failed(
    ClientOp& op,
    ClientStatus failure,
    node::Id avoid,
    bool immediate,
    std::uint64_t now_ms
)
{
    op.last_failure = failure;
    op.avoid = avoid;

    if (op.attempts >= config::CLIENT_MAX_ATTEMPTS) {
        finish_op(op, failure, node::INVALID, nullptr, 0, now_ms);
        return;
    }

    op.phase = OpPhase::WAITING;
    op.next_attempt_ms =
        immediate
            ? now_ms
            : now_ms +
                  static_cast<std::uint64_t>(config::CLIENT_ATTEMPT_BACKOFF_MS) *
                      op.attempts;

    char key_text[storage::KEY_MAX + 1];
    format_key(op.key, op.key_length, key_text, sizeof(key_text));

    logf(
        LogLevel::DEBUG,
        "%s %s attempt %u failed (%s), retrying",
        to_string(op.type),
        key_text,
        static_cast<unsigned>(op.attempts),
        to_string(failure)
    );
}

void Runtime::on_op_response(
    ClientOp& op,
    protocol::StatusCode status,
    node::Id responder,
    protocol::ValueView value,
    std::uint64_t now_ms
)
{
    switch (status) {
        case protocol::StatusCode::OK:
            finish_op(op, ClientStatus::OK, responder, value.data, value.len, now_ms);
            return;

        case protocol::StatusCode::NOT_FOUND:
            finish_op(op, ClientStatus::NOT_FOUND, responder, nullptr, 0, now_ms);
            return;

        case protocol::StatusCode::REPLICATION_FAILED:
            finish_op(op, ClientStatus::UNDER_REPLICATED, responder, nullptr, 0, now_ms);
            return;

        case protocol::StatusCode::NOT_RESPONSIBLE:
            attempt_failed(op, ClientStatus::NO_ROUTE, responder, false, now_ms);
            return;

        case protocol::StatusCode::STALE_VERSION:
            // Our clock has merged the newer version's HLC; the next
            // attempt's version wins.
            attempt_failed(op, ClientStatus::STALE_VERSION, node::INVALID, true, now_ms);
            return;

        case protocol::StatusCode::STORAGE_ERROR:
            // Often transient (log full while compaction runs, dedup
            // table full); retried with backoff.
            attempt_failed(op, ClientStatus::STORAGE_ERROR, node::INVALID, false, now_ms);
            return;

        case protocol::StatusCode::KEY_TOO_LARGE:
            finish_op(op, ClientStatus::KEY_TOO_LARGE, responder, nullptr, 0, now_ms);
            return;

        case protocol::StatusCode::VALUE_TOO_LARGE:
            finish_op(op, ClientStatus::VALUE_TOO_LARGE, responder, nullptr, 0, now_ms);
            return;

        case protocol::StatusCode::INVALID_REQUEST:
        case protocol::StatusCode::UNSUPPORTED:
            finish_op(op, ClientStatus::INVALID_REQUEST, responder, nullptr, 0, now_ms);
            return;
    }

    finish_op(op, ClientStatus::INVALID_REQUEST, responder, nullptr, 0, now_ms);
}

void Runtime::handle_client_response(
    const protocol::DecodedMessage& message,
    const std::uint8_t* raw,
    std::size_t raw_length,
    std::uint64_t now_ms
)
{
    const request::Identity identity{
        message.header.origin_node_id,
        message.header.request_id
    };

    if (identity.origin != local_node_id_) {
        return;
    }

    ClientOp* op = find_op(identity);

    if (op == nullptr) {
        // Response to an attempt we already gave up on.
        return;
    }

    if (op->phase == OpPhase::REMOTE) {
        if (client_requests_.complete(identity, raw, raw_length) !=
            request::Result::OK) {
            return;
        }

        client_requests_.release(identity);
    }

    protocol::StatusCode status = protocol::StatusCode::INVALID_REQUEST;
    protocol::ValueView value{nullptr, 0};

    switch (message.header.msg_type) {
        case protocol::MessageType::SET_ACK:
            status = message.payload.set_ack.status;
            break;

        case protocol::MessageType::GET_RESP:
            status = message.payload.get_response.status;
            value = message.payload.get_response.value;
            break;

        case protocol::MessageType::DEL_ACK:
            status = message.payload.delete_ack.status;
            break;

        default:
            return;
    }

    on_op_response(*op, status, message.header.sender_node_id, value, now_ms);
}

void Runtime::finish_op(
    ClientOp& op,
    ClientStatus status,
    node::Id served_by,
    const std::uint8_t* value,
    std::size_t value_length,
    std::uint64_t now_ms
)
{
    if (op.phase == OpPhase::REMOTE) {
        client_requests_.release(op.identity);
    }

    ClientResult& result = op.result;

    result = ClientResult{};
    result.type = op.type;
    result.status = status;
    std::memcpy(result.key, op.key, op.key_length);
    result.key_length = op.key_length;

    if (value != nullptr && value_length > 0 && value_length <= storage::VALUE_MAX) {
        std::memcpy(result.value, value, value_length);
        result.value_length = static_cast<std::uint8_t>(value_length);
    }

    result.served_by = served_by;
    result.attempts = op.attempts;
    result.latency_ms =
        static_cast<std::uint32_t>(now_ms >= op.started_ms ? now_ms - op.started_ms : 0);

    if (status == ClientStatus::OK ||
        status == ClientStatus::NOT_FOUND ||
        status == ClientStatus::UNDER_REPLICATED) {
        ++counters_.client_ops_ok;
    } else {
        ++counters_.client_ops_failed;

        char key_text[storage::KEY_MAX + 1];
        format_key(op.key, op.key_length, key_text, sizeof(key_text));

        logf(
            LogLevel::INFO,
            "%s %s failed after %u attempt(s): %s",
            to_string(op.type),
            key_text,
            static_cast<unsigned>(op.attempts),
            to_string(status)
        );
    }

    op.phase = OpPhase::DONE;
}

void Runtime::tick_client_ops(std::uint64_t now_ms)
{
    client_requests_.tick(now_ms);

    for (ClientOp& op : ops_) {
        switch (op.phase) {
            case OpPhase::WAITING:
                if (now_ms >= op.next_attempt_ms) {
                    start_attempt(op, now_ms);
                }
                break;

            case OpPhase::REMOTE: {
                request::State state = request::State::FREE;

                const request::Result result =
                    client_requests_.get_state(op.identity, state);

                if (result == request::Result::OK &&
                    state == request::State::PENDING) {
                    break;
                }

                client_requests_.release(op.identity);
                attempt_failed(op, ClientStatus::TIMEOUT, node::INVALID, false, now_ms);
                break;
            }

            case OpPhase::LOCAL:
                // Leak guard; the local server path always answers
                // within the replication deadline.
                if (now_ms - op.attempt_started_ms >
                    2u * config::CLIENT_ATTEMPT_DEADLINE_MS) {
                    attempt_failed(op, ClientStatus::TIMEOUT, node::INVALID, false, now_ms);
                }
                break;

            default:
                break;
        }
    }
}

void Runtime::deliver_completions()
{
    for (ClientOp& op : ops_) {
        if (op.phase != OpPhase::DONE) {
            continue;
        }

        const ClientResult result = op.result;
        const CompletionFn completion = op.completion;
        void* const context = op.context;
        const std::uint32_t tag = op.tag;

        // Free the slot first: the completion may submit again.
        op = ClientOp{};

        if (completion != nullptr) {
            completion(context, tag, result);
        }
    }
}

// ------------------------------------------------------------------
// Timers
// ------------------------------------------------------------------

void Runtime::tick(std::uint64_t now_ms)
{
    if (!started_) {
        return;
    }

    tick_state(now_ms);
    tick_view(now_ms);
    tick_heartbeat(now_ms);
    tick_client_ops(now_ms);
    tick_replications(now_ms);
    dedup_.expire(now_ms);
    tick_anti_entropy(now_ms);
    tick_storage(now_ms);
    deliver_completions();
}

void Runtime::tick_state(std::uint64_t now_ms)
{
    const std::uint64_t in_state = now_ms - state_since_ms_;

    switch (state_) {
        case NodeState::BOOTING: {
            if (in_state < config::BOOT_LISTEN_MS) {
                return;
            }

            bool peer_alive = false;

            for (node::Id id = node::MIN; id <= node::MAX; ++id) {
                if (id != local_node_id_ && membership_.is_alive(id, now_ms)) {
                    peer_alive = true;
                }
            }

            // Nobody to catch up from (cold start of the cluster or
            // a lone node): serve right away.
            enter_state(
                peer_alive ? NodeState::CATCHUP : NodeState::ACTIVE,
                now_ms
            );
            return;
        }

        case NodeState::CATCHUP: {
            const std::uint64_t spacing =
                config::CATCHUP_DURATION_MS / config::CATCHUP_TABLE_ROUNDS;

            if (catchup_rounds_sent_ < config::CATCHUP_TABLE_ROUNDS &&
                in_state >= catchup_rounds_sent_ * spacing &&
                !ae_.round_active()) {

                ae_.start_round(storage_, next_sync_id_++);
                ++catchup_rounds_sent_;
            }

            if (in_state >= config::CATCHUP_DURATION_MS) {
                enter_state(NodeState::ACTIVE, now_ms);
            }

            return;
        }

        case NodeState::ACTIVE:
            return;
    }
}

void Runtime::tick_view(std::uint64_t now_ms)
{
    const placement::AliveMask mask = view(now_ms);

    if (mask == last_view_) {
        return;
    }

    ++counters_.membership_changes;

    char text[node::MAX - node::MIN + 2];

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        text[id - node::MIN] =
            (mask & placement::mask_of(id)) ? letter(id) : '-';
    }

    text[node::MAX - node::MIN + 1] = '\0';

    logf(LogLevel::INFO, "view: %s", text);

    last_view_ = mask;
}

void Runtime::tick_heartbeat(std::uint64_t now_ms)
{
    if (state_ != NodeState::ACTIVE) {
        return;
    }

    if (last_heartbeat_ms_ != 0 &&
        now_ms - last_heartbeat_ms_ < config::HEARTBEAT_INTERVAL_MS) {
        return;
    }

    packet::Buffer out{};

    if (!packet::build_heartbeat(local_node_id_, clock_.now(), out)) {
        return;
    }

    last_heartbeat_ms_ = now_ms == 0 ? 1 : now_ms;

    ++counters_.heartbeats_tx;
    ++counters_.frames_tx;

    if (!link_.broadcast(out.data, out.length)) {
        ++counters_.frames_tx_failed;
    }
}

bool Runtime::send_push(const ae::Push& push)
{
    storage::EntryInfo info{};

    if (!storage_.slot_info(push.slot, info)) {
        return false;
    }

    std::uint8_t value[storage::VALUE_MAX];
    std::size_t value_length = 0;

    if (info.state == storage::State::LIVE &&
        storage_.read_slot_value(push.slot, value, sizeof(value), value_length) !=
            storage::Result::OK) {
        return false;
    }

    // RESYNC_DATA carries the version itself: origin_node_id and hlc.
    const packet::Route route{
        0,
        info.version.origin,
        local_node_id_,
        push.peer,
        info.version.hlc,
        info.state == storage::State::TOMBSTONE ? protocol::FLAG_TOMBSTONE : static_cast<std::uint8_t>(0)
    };

    packet::Buffer out{};

    if (!packet::build_request(
            protocol::MessageType::RESYNC_DATA,
            route,
            protocol::KeyView{info.key, info.key_len},
            protocol::ValueView{value, static_cast<std::uint8_t>(value_length)},
            out
        )) {
        return false;
    }

    ++counters_.resync_tx;

    char key_text[storage::KEY_MAX + 1];
    format_key(info.key, info.key_len, key_text, sizeof(key_text));

    logf(LogLevel::DEBUG, "anti-entropy: push %s -> %c", key_text, letter(push.peer));

    return send(push.peer, out.data, out.length);
}

void Runtime::tick_anti_entropy(std::uint64_t now_ms)
{
    if (state_ == NodeState::ACTIVE &&
        !ae_.round_active() &&
        (sync_requested_ || now_ms >= next_ae_round_ms_)) {

        ae_.start_round(storage_, next_sync_id_++);
        next_ae_round_ms_ = now_ms + config::AE_PERIOD_MS;
        sync_requested_ = false;
    }

    if (now_ms >= next_ae_packet_ms_) {
        bool sent = false;
        ae::Push push{};

        // Repairs first, then our own table.
        if (ae_.next_push(push)) {
            send_push(push);
            sent = true;
        } else if (ae_.round_active()) {
            protocol::VersionTablePayload table{};
            protocol::VersionTableEntry entries[protocol::VERSION_TABLE_MAX_ENTRIES]{};

            if (ae_.next_chunk(storage_, table, entries)) {
                packet::Buffer out{};

                if (packet::build_version_table(local_node_id_, clock_.now(), table, out)) {
                    ++counters_.frames_tx;

                    if (!link_.broadcast(out.data, out.length)) {
                        ++counters_.frames_tx_failed;
                    }
                }

                sent = true;
            }
        }

        if (sent) {
            next_ae_packet_ms_ = now_ms + config::AE_PACKET_SPACING_MS;
        }
    }

    if (state_ == NodeState::ACTIVE && now_ms >= next_gc_ms_) {
        next_gc_ms_ = now_ms + config::AE_PERIOD_MS;

        const ae::GcContext context{
            view(now_ms),
            now_ms,
            clock_.physical_now()
        };

        const std::size_t marked =
            ae_.collect_garbage(storage_, placement_, context);

        if (marked > 0) {
            logf(
                LogLevel::INFO,
                "gc: %u key(s) marked for drop (orphans %lu, tombstones %lu total)",
                static_cast<unsigned>(marked),
                static_cast<unsigned long>(ae_.counters().gc_orphans),
                static_cast<unsigned long>(ae_.counters().gc_tombstones)
            );
        }
    }
}

void Runtime::tick_storage(std::uint64_t now_ms)
{
    if (storage_.compaction_active()) {
        bool done = false;
        const storage::Result result = storage_.compaction_step(done);

        if (result != storage::Result::OK) {
            compaction_retry_ms_ = now_ms + 30000;
            logf(
                LogLevel::INFO,
                "compaction aborted (%u); old bank stays authoritative",
                static_cast<unsigned>(result)
            );
            return;
        }

        if (done) {
            const storage::Stats stats = storage_.stats();
            logf(
                LogLevel::INFO,
                "compaction #%lu done: bank %c, %u keys, %u bytes free",
                static_cast<unsigned long>(stats.compactions_completed),
                stats.active_bank == 0 ? 'A' : 'B',
                static_cast<unsigned>(stats.slots_used),
                static_cast<unsigned>(stats.log_free_bytes)
            );
        }

        return;
    }

    if (compaction_requested_ ||
        (storage_.compaction_needed() && now_ms >= compaction_retry_ms_)) {

        compaction_requested_ = false;

        if (storage_.compaction_begin() == storage::Result::OK) {
            logf(
                LogLevel::INFO,
                "compaction started (%u bytes free)",
                static_cast<unsigned>(storage_.free_log_bytes())
            );
        }
    }
}

// ------------------------------------------------------------------
// Operator actions
// ------------------------------------------------------------------

void Runtime::request_sync(std::uint64_t now_ms)
{
    (void)now_ms;
    sync_requested_ = true;
}

void Runtime::request_compaction()
{
    compaction_requested_ = true;
}

bool Runtime::format_storage(std::uint64_t now_ms)
{
    if (storage_.format() != storage::Result::OK) {
        return false;
    }

    ae_.reset();

    // Stop serving until peers pushed our share back.
    if (state_ == NodeState::ACTIVE) {
        enter_state(NodeState::BOOTING, now_ms);
    }

    logf(LogLevel::INFO, "storage formatted");
    return true;
}

bool Runtime::inject_stale_write(
    node::Id destination,
    const std::uint8_t* key,
    std::size_t key_length,
    const std::uint8_t* value,
    std::size_t value_length,
    hlc::Hlc stale_hlc
)
{
#if EDGE_FAULT_INJECTION
    const packet::Route route{
        0,
        local_node_id_,
        local_node_id_,
        destination,
        stale_hlc,
        0
    };

    packet::Buffer out{};

    if (!packet::build_request(
            protocol::MessageType::RESYNC_DATA,
            route,
            protocol::KeyView{key, static_cast<std::uint8_t>(key_length)},
            protocol::ValueView{value, static_cast<std::uint8_t>(value_length)},
            out
        )) {
        return false;
    }

    ++counters_.faults_injected;
    logf(LogLevel::INFO, "FAULT: stale write (hlc %llu) sent to %c",
         static_cast<unsigned long long>(stale_hlc), letter(destination));

    return send(destination, out.data, out.length);
#else
    (void)destination;
    (void)key;
    (void)key_length;
    (void)value;
    (void)value_length;
    (void)stale_hlc;
    return false;
#endif
}

} // namespace edge

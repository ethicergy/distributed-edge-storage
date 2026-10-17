#pragma once

#include <cstddef>
#include <cstdint>

#include "anti_entropy.h"
#include "config.h"
#include "dedup_manager.h"
#include "hlc.h"
#include "kv_engine.h"
#include "membership.h"
#include "node_id.h"
#include "packet_builder.h"
#include "placement.h"
#include "protocol.h"
#include "request_identity.h"
#include "request_manager.h"
#include "storage.h"

/*
 * Node runtime: the single owner of all distributed-store state on
 * one node (HLC, membership, request/dedup tables, KV engine, anti-
 * entropy). Platform independent: the firmware drives it from one
 * FreeRTOS task, the host simulator drives four of them in virtual
 * time. Nothing in here blocks, allocates or knows about ESP-NOW.
 *
 * Every node runs the same code. What a node does with a request is
 * decided only by placement over its current membership view.
 */
namespace edge {

// Radio abstraction (ESP-NOW in the firmware, a simulated network
// on the host).
class Link {
public:
    virtual ~Link() = default;

    virtual bool unicast(
        node::Id destination,
        const std::uint8_t* data,
        std::size_t length
    ) = 0;

    virtual bool broadcast(
        const std::uint8_t* data,
        std::size_t length
    ) = 0;
};

enum class NodeState : std::uint8_t {
    BOOTING = 0, // silent, listening, merging clocks
    CATCHUP,     // announcing our version table, receiving pushes
    ACTIVE       // heartbeating and serving
};

const char* to_string(NodeState state);

enum class OpType : std::uint8_t {
    SET = 0,
    GET,
    DEL
};

const char* to_string(OpType type);

// Outcome of a client operation, as seen by the caller.
enum class ClientStatus : std::uint8_t {
    OK = 0,
    NOT_FOUND,

    // Stored on the primary only; anti-entropy will copy it to the
    // replica (decision D5). The write is committed.
    UNDER_REPLICATED,

    STORAGE_ERROR,
    INVALID_REQUEST,
    KEY_TOO_LARGE,
    VALUE_TOO_LARGE,

    NOT_READY,     // this node is still booting
    BUSY,          // operation table full
    NO_ROUTE,      // every attempt was answered NOT_RESPONSIBLE
    STALE_VERSION, // lost the version race on every attempt
    TIMEOUT        // no answer within the attempts' deadlines
};

const char* to_string(ClientStatus status);

// The write is durable in the cluster (possibly under-replicated).
inline bool write_committed(ClientStatus status)
{
    return status == ClientStatus::OK ||
           status == ClientStatus::UNDER_REPLICATED;
}

struct ClientResult {
    OpType type;
    ClientStatus status;

    std::uint8_t key[storage::KEY_MAX];
    std::uint8_t key_length;

    std::uint8_t value[storage::VALUE_MAX];
    std::uint8_t value_length;

    node::Id served_by;
    std::uint8_t attempts;
    std::uint32_t latency_ms;
};

using CompletionFn = void (*)(
    void* context,
    std::uint32_t tag,
    const ClientResult& result
);

using LogFn = void (*)(void* context, const char* line);

enum class LogLevel : std::uint8_t {
    OFF = 0,
    INFO,  // state changes, failures, repairs, compactions
    DEBUG  // every request
};

struct Counters {
    std::uint32_t frames_rx;
    std::uint32_t frames_rx_invalid;
    std::uint32_t frames_rx_spoofed;
    std::uint32_t frames_tx;
    std::uint32_t frames_tx_failed;

    std::uint32_t heartbeats_tx;
    std::uint32_t heartbeats_rx;
    std::uint32_t heartbeats_from[node::MAX - node::MIN + 1];

    std::uint32_t requests_executed;
    std::uint32_t requests_rejected_not_ready;
    std::uint32_t dedup_replays;
    std::uint32_t dedup_suppressed;
    std::uint32_t dedup_table_full;

    std::uint32_t replications_started;
    std::uint32_t replications_ok;
    std::uint32_t replications_failed;
    std::uint32_t replications_stale;
    std::uint32_t replicas_applied;
    std::uint32_t replicas_stale;

    std::uint32_t resync_tx;
    std::uint32_t resync_applied;
    std::uint32_t resync_stale;

    std::uint32_t client_ops_ok;
    std::uint32_t client_ops_failed;
    std::uint32_t client_attempts;
    std::uint32_t client_local_fast_path;

    std::uint32_t storage_errors;
    std::uint32_t membership_changes;

    std::uint32_t faults_injected;
};

// Fault injection for the reliability tests (H4, H5, H10). Only
// active in builds with EDGE_FAULT_INJECTION=1.
struct Faults {
    std::uint8_t drop_client_requests;  // first transmission only
    std::uint8_t drop_client_responses; // SET_ACK / GET_RESP / DEL_ACK
    std::uint8_t drop_repl_requests;    // first transmission only
    std::uint8_t drop_repl_acks;
    std::uint8_t duplicate_client_requests;
    // Random loss of every outgoing frame (unicast and broadcast), in
    // percent; stays until cleared. Measures loss tolerance on boards.
    std::uint8_t tx_loss_percent;
};

class Runtime {
public:
    Runtime(
        node::Id local_node_id,
        storage::StorageManager& storage,
        Link& link,
        hlc::PhysicalNowFn physical_now
    );

    /*
     * Opens storage (reformatting it if it is corrupted and
     * config::STORAGE_AUTO_FORMAT_ON_CORRUPTION allows), seeds the
     * HLC from the highest persisted version and enters BOOTING.
     */
    bool begin(std::uint64_t now_ms, std::uint32_t random_seed);

    // One received radio frame. link_source is the sender as
    // identified by the link layer (MAC table), or node::INVALID.
    void on_frame(
        const std::uint8_t* data,
        std::size_t length,
        node::Id link_source,
        std::uint64_t now_ms
    );

    // Timers: heartbeats, retries, deadlines, anti-entropy, GC,
    // compaction steps. Call often (every few ms).
    void tick(std::uint64_t now_ms);

    /*
     * Client API used by every request source (console, sensors,
     * Pi bridge). Routes by placement: runs locally when this node
     * is the primary, otherwise sends a reliable request. On
     * NOT_RESPONSIBLE, STALE_VERSION or an attempt deadline it makes
     * a new attempt (new request identity) after recomputing
     * placement. The completion is always called exactly once for an
     * accepted operation, never from inside submit().
     *
     * Returns OK when accepted, or the reason it was refused.
     */
    ClientStatus submit(
        OpType type,
        const std::uint8_t* key,
        std::size_t key_length,
        const std::uint8_t* value,
        std::size_t value_length,
        CompletionFn completion,
        void* context,
        std::uint32_t tag,
        std::uint64_t now_ms
    );

    std::size_t free_client_slots() const;

    // ---------------- introspection ----------------

    node::Id local_node_id() const { return local_node_id_; }
    NodeState state() const { return state_; }
    placement::AliveMask view(std::uint64_t now_ms) const;

    const membership::Manager& membership() const { return membership_; }
    const placement::Resolver& placement() const { return placement_; }
    storage::StorageManager& storage() { return storage_; }
    const hlc::Clock& clock() const { return clock_; }

    const Counters& counters() const { return counters_; }
    const ae::Counters& ae_counters() const { return ae_.counters(); }

    std::size_t dedup_active() const { return dedup_.active_count(); }
    std::size_t client_pending() const { return client_requests_.pending_count(); }
    std::size_t replication_pending() const { return replication_requests_.pending_count(); }

    void set_log(LogFn log, void* context, LogLevel level);
    void set_log_level(LogLevel level) { log_level_ = level; }
    LogLevel log_level() const { return log_level_; }

    // Start an anti-entropy round now (console `sync`).
    void request_sync(std::uint64_t now_ms);

    // Start a compaction now (console `compact`).
    void request_compaction();

    // Erase local storage; anti-entropy repopulates it.
    bool format_storage(std::uint64_t now_ms);

    Faults& faults() { return faults_; }

    // Send a RESYNC_DATA carrying an outdated version of key to a
    // node (stale-version test H10). Fault-injection builds only.
    bool inject_stale_write(
        node::Id destination,
        const std::uint8_t* key,
        std::size_t key_length,
        const std::uint8_t* value,
        std::size_t value_length,
        hlc::Hlc stale_hlc
    );

private:
    // ---------------- client operations ----------------

    enum class OpPhase : std::uint8_t {
        FREE = 0,
        WAITING,        // next attempt scheduled
        REMOTE,         // request in client_requests_
        LOCAL,          // being served by our own server path
        DONE            // completion pending delivery
    };

    struct ClientOp {
        OpPhase phase = OpPhase::FREE;
        OpType type = OpType::GET;

        std::uint8_t key[storage::KEY_MAX]{};
        std::uint8_t key_length = 0;
        std::uint8_t value[storage::VALUE_MAX]{};
        std::uint8_t value_length = 0;

        CompletionFn completion = nullptr;
        void* context = nullptr;
        std::uint32_t tag = 0;

        std::uint8_t attempts = 0;
        std::uint64_t started_ms = 0;
        std::uint64_t next_attempt_ms = 0;
        std::uint64_t attempt_started_ms = 0;

        request::Identity identity{};
        node::Id target = node::INVALID;
        node::Id avoid = node::INVALID;

        ClientStatus last_failure = ClientStatus::TIMEOUT;
        ClientResult result{};
    };

    // ---------------- replication on the primary ----------------

    struct PendingReplication {
        bool active = false;
        request::Identity replication_identity{};
        request::Identity client_identity{};
        protocol::PacketHeader client_header{};
        kv::OperationResponse response{};
        node::Id replica = node::INVALID;
    };

    // ---------------- message handling ----------------

    void handle_client_request(
        const protocol::DecodedMessage& message,
        std::uint64_t now_ms
    );

    void handle_client_response(
        const protocol::DecodedMessage& message,
        const std::uint8_t* raw,
        std::size_t raw_length,
        std::uint64_t now_ms
    );

    void handle_replication_request(
        const protocol::DecodedMessage& message,
        std::uint64_t now_ms
    );

    void handle_replication_ack(
        const protocol::DecodedMessage& message,
        const std::uint8_t* raw,
        std::size_t raw_length,
        std::uint64_t now_ms
    );

    void handle_resync(
        const protocol::DecodedMessage& message,
        std::uint64_t now_ms
    );

    void handle_version_table(
        const protocol::DecodedMessage& message,
        std::uint64_t now_ms
    );

    // Finish a client request on the server side: build, cache in
    // dedup, deliver to the origin (radio or local).
    void respond_to_client(
        const protocol::PacketHeader& request_header,
        request::Identity identity,
        const kv::OperationResponse& response,
        bool cache,
        std::uint64_t now_ms
    );

    bool begin_replication(
        const protocol::DecodedMessage& message,
        request::Identity client_identity,
        const kv::OperationResponse& response,
        storage::Version version,
        std::uint64_t now_ms
    );

    void finish_replication(
        PendingReplication& pending,
        kv::Result client_result,
        std::uint64_t now_ms
    );

    // Deliver a packet to a node; local delivery when it is us.
    void deliver(
        node::Id destination,
        const std::uint8_t* data,
        std::size_t length,
        std::uint64_t now_ms
    );

    bool send(
        node::Id destination,
        const std::uint8_t* data,
        std::size_t length
    );

    static bool reliable_send(
        void* context,
        node::Id destination,
        const std::uint8_t* packet,
        std::size_t packet_length,
        bool retry
    );

    // ---------------- client internals ----------------

    void start_attempt(ClientOp& op, std::uint64_t now_ms);

    void attempt_failed(
        ClientOp& op,
        ClientStatus failure,
        node::Id avoid,
        bool immediate,
        std::uint64_t now_ms
    );

    void finish_op(
        ClientOp& op,
        ClientStatus status,
        node::Id served_by,
        const std::uint8_t* value,
        std::size_t value_length,
        std::uint64_t now_ms
    );

    void on_op_response(
        ClientOp& op,
        protocol::StatusCode status,
        node::Id responder,
        protocol::ValueView value,
        std::uint64_t now_ms
    );

    ClientOp* find_op(request::Identity identity);

    void tick_client_ops(std::uint64_t now_ms);
    void deliver_completions();

    // ---------------- timers ----------------

    void tick_state(std::uint64_t now_ms);
    void tick_heartbeat(std::uint64_t now_ms);
    void tick_replications(std::uint64_t now_ms);
    void tick_anti_entropy(std::uint64_t now_ms);
    void tick_storage(std::uint64_t now_ms);
    void tick_view(std::uint64_t now_ms);

    void enter_state(NodeState state, std::uint64_t now_ms);

    void note_local_change(
        const std::uint8_t* key,
        std::size_t key_length,
        std::uint64_t now_ms
    );

    bool send_push(const ae::Push& push);

    void logf(LogLevel level, const char* format, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;

    // ---------------- state ----------------

    node::Id local_node_id_;
    storage::StorageManager& storage_;
    Link& link_;

    hlc::Clock clock_;
    membership::Manager membership_;
    placement::Resolver placement_;
    kv::Engine engine_;

    request::Generator ids_;
    request::Manager client_requests_;
    request::Manager replication_requests_;
    request::DedupManager dedup_;

    ae::Tracker ae_;

    NodeState state_ = NodeState::BOOTING;
    std::uint64_t state_since_ms_ = 0;
    std::uint64_t last_heartbeat_ms_ = 0;
    std::uint64_t next_ae_round_ms_ = 0;
    std::uint64_t next_ae_packet_ms_ = 0;
    std::uint64_t next_gc_ms_ = 0;
    std::uint64_t compaction_retry_ms_ = 0;
    std::uint8_t catchup_rounds_sent_ = 0;
    std::uint32_t next_sync_id_ = 1;
    placement::AliveMask last_view_ = 0;
    bool compaction_requested_ = false;
    bool sync_requested_ = false;
    bool started_ = false;

    ClientOp ops_[config::MAX_CLIENT_OPS]{};
    PendingReplication replications_[config::MAX_PENDING_REPLICATIONS]{};

    Counters counters_{};
    Faults faults_{};
    std::uint32_t loss_rng_ = 1;

    // Fault injection: drop this outgoing frame (tx_loss_percent).
    bool lose_frame();
    bool broadcast(const std::uint8_t* data, std::size_t length);

    LogFn log_ = nullptr;
    void* log_context_ = nullptr;
    LogLevel log_level_ = LogLevel::INFO;
};

} // namespace edge

#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "node_id.h"
#include "placement.h"
#include "protocol.h"
#include "storage.h"

/*
 * Anti-entropy (push based, non-ACKed by design).
 *
 * Every node periodically broadcasts its version table in chunks of
 * up to 5 entries (VERSION_TABLE). A receiver compares each entry
 * with its own copy:
 *
 *   receiver newer   -> push RESYNC_DATA to the sender, if the sender
 *                       owns the key (placement with the sender
 *                       counted as alive);
 *   receiver older   -> nothing; the sender pushes when it sees our
 *                       table;
 *   key missing in a COMPLETE table (all chunks of one sync_id
 *   arrived)         -> push it if the sender owns the key.
 *
 * Lost packets are repaired by the next round. Pushes go to any
 * owner, so a node that becomes the new replica after a membership
 * change receives existing keys (hand-off), and a node catching up
 * after a reboot receives what it missed.
 *
 * Complete tables also feed garbage collection (decision D8):
 *  - orphan copy : we are not an owner and every current owner
 *                  reported a version >= ours -> drop locally;
 *  - tombstone   : older than TOMBSTONE_GC_MIN_AGE_MS, all four
 *                  nodes alive, and every other node reported either
 *                  no copy or a version >= ours -> drop locally.
 * Drops are physical (no record written); the slot is reclaimed by
 * the next compaction.
 */
namespace ae {

struct Push {
    std::uint8_t slot;
    node::Id peer;
};

struct Counters {
    std::uint32_t rounds_started;
    std::uint32_t chunks_sent;
    std::uint32_t chunks_received;
    std::uint32_t tables_completed;
    std::uint32_t pushes_queued;
    std::uint32_t pushes_dropped_queue_full;
    std::uint32_t gc_orphans;
    std::uint32_t gc_tombstones;
};

struct GcContext {
    placement::AliveMask view;
    std::uint64_t now_ms;
    std::uint64_t hlc_physical_now_ms;
};

class Tracker {
public:
    explicit Tracker(node::Id local_node_id);

    void reset();

    // ---------------- outgoing table ----------------

    // Snapshot the current keys and start a broadcast round.
    void start_round(
        const storage::StorageManager& storage,
        std::uint32_t sync_id
    );

    bool round_active() const;

    // Next chunk of the active round. false when the round is over.
    bool next_chunk(
        const storage::StorageManager& storage,
        protocol::VersionTablePayload& table,
        protocol::VersionTableEntry (&entries)[protocol::VERSION_TABLE_MAX_ENTRIES]
    );

    // ---------------- incoming table ----------------

    void on_chunk(
        node::Id sender,
        const protocol::VersionTablePayload& table,
        const storage::StorageManager& storage,
        const placement::Resolver& placement,
        placement::AliveMask view,
        std::uint64_t now_ms
    );

    bool next_push(Push& push);

    std::size_t pending_pushes() const;

    // Must be called whenever a key's local version changes.
    void on_local_change(std::size_t slot, std::uint64_t now_ms);

    // ---------------- garbage collection ----------------

    // Marks keys for drop; returns the number marked.
    std::size_t collect_garbage(
        storage::StorageManager& storage,
        const placement::Resolver& placement,
        const GcContext& context
    );

    const Counters& counters() const;

    // Peer has a complete, fresh report (for diagnostics).
    bool has_fresh_report(node::Id peer, std::uint64_t now_ms) const;

private:
    struct Report {
        // Being assembled
        std::uint32_t sync_id = 0;
        std::uint16_t chunk_count = 0;
        std::uint32_t received_chunks = 0;
        std::uint64_t started_ms = 0;
        std::uint64_t seen = 0;
        std::uint64_t peer_ge = 0;   // peer version >= ours
        std::uint64_t peer_older = 0; // peer version < ours
        bool assembling = false;

        // Last complete report
        bool complete = false;
        std::uint64_t complete_started_ms = 0;
        std::uint64_t complete_at_ms = 0;
        std::uint64_t complete_seen = 0;
        std::uint64_t complete_ge = 0;
        std::uint64_t complete_older = 0;
    };

    static constexpr std::size_t MAX_CHUNKS =
        (storage::MAX_LOGICAL_KEYS +
         protocol::VERSION_TABLE_MAX_ENTRIES - 1) /
        protocol::VERSION_TABLE_MAX_ENTRIES;

    static_assert(MAX_CHUNKS <= 32, "chunk bitmap is 32-bit");

    static std::size_t peer_index(node::Id id);

    void queue_push(std::uint8_t slot, node::Id peer);

    bool report_confirms(
        node::Id peer,
        std::size_t slot,
        bool allow_absent,
        std::uint64_t now_ms
    ) const;

    node::Id local_node_id_;

    // Outgoing round
    std::uint8_t round_slots_[storage::MAX_LOGICAL_KEYS]{};
    std::size_t round_slot_count_ = 0;
    std::uint16_t round_chunk_count_ = 0;
    std::uint16_t round_next_chunk_ = 0;
    std::uint32_t round_sync_id_ = 0;
    bool round_active_ = false;

    std::uint8_t entry_keys_[protocol::VERSION_TABLE_MAX_ENTRIES][storage::KEY_MAX]{};

    Report reports_[node::MAX - node::MIN + 1]{};

    std::uint64_t last_change_ms_[storage::MAX_LOGICAL_KEYS]{};

    Push pushes_[config::AE_PUSH_QUEUE_DEPTH]{};
    std::size_t push_head_ = 0;
    std::size_t push_count_ = 0;

    Counters counters_{};
};

} // namespace ae

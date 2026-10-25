#include "anti_entropy.h"

#include <cstring>

#include "hlc.h"

namespace ae {

namespace {

constexpr std::uint64_t bit(std::size_t slot)
{
    return 1ULL << slot;
}

placement::AliveMask with(placement::AliveMask view, node::Id id)
{
    return static_cast<placement::AliveMask>(
        view | placement::mask_of(id)
    );
}

bool key_owned_by(
    const placement::Resolver& placement,
    const storage::EntryInfo& info,
    placement::AliveMask view,
    node::Id peer
)
{
    placement::Assignment assignment{};

    if (!placement.locate(info.key, info.key_len, view, assignment)) {
        return false;
    }

    return placement::is_owner(assignment, peer);
}

} // namespace

Tracker::Tracker(node::Id local_node_id)
    : local_node_id_(local_node_id)
{
}

void Tracker::reset()
{
    round_active_ = false;
    round_slot_count_ = 0;
    push_head_ = 0;
    push_count_ = 0;

    for (Report& report : reports_) {
        report = Report{};
    }

    for (std::uint64_t& change : last_change_ms_) {
        change = 0;
    }
}

std::size_t Tracker::peer_index(node::Id id)
{
    return static_cast<std::size_t>(id - node::MIN);
}

// --------------------------------------------------------------
// Outgoing
// --------------------------------------------------------------

void Tracker::start_round(
    const storage::StorageManager& storage,
    std::uint32_t sync_id
)
{
    round_slot_count_ = 0;

    for (std::size_t slot = 0;
         slot < storage::StorageManager::slot_capacity();
         ++slot) {

        storage::EntryInfo info{};

        if (storage.slot_info(slot, info)) {
            round_slots_[round_slot_count_++] =
                static_cast<std::uint8_t>(slot);
        }
    }

    round_chunk_count_ = static_cast<std::uint16_t>(
        (round_slot_count_ + protocol::VERSION_TABLE_MAX_ENTRIES - 1) /
        protocol::VERSION_TABLE_MAX_ENTRIES
    );

    // An empty table is still announced: one chunk, zero entries.
    // Peers then know we hold nothing and push what we own.
    if (round_chunk_count_ == 0) {
        round_chunk_count_ = 1;
    }

    round_next_chunk_ = 0;
    round_sync_id_ = sync_id;
    round_active_ = true;

    ++counters_.rounds_started;
}

bool Tracker::round_active() const
{
    return round_active_;
}

bool Tracker::next_chunk(
    const storage::StorageManager& storage,
    protocol::VersionTablePayload& table,
    protocol::VersionTableEntry (&entries)[protocol::VERSION_TABLE_MAX_ENTRIES]
)
{
    if (!round_active_ || round_next_chunk_ >= round_chunk_count_) {
        round_active_ = false;
        return false;
    }

    const std::size_t first =
        static_cast<std::size_t>(round_next_chunk_) *
        protocol::VERSION_TABLE_MAX_ENTRIES;

    std::uint8_t count = 0;

    for (std::size_t i = first;
         i < round_slot_count_ &&
         i < first + protocol::VERSION_TABLE_MAX_ENTRIES;
         ++i) {

        storage::EntryInfo info{};

        // A key deleted from the index since the snapshot is
        // skipped; the chunk is just shorter.
        if (!storage.slot_info(round_slots_[i], info)) {
            continue;
        }

        std::memcpy(entry_keys_[count], info.key, info.key_len);

        entries[count] = protocol::VersionTableEntry{
            info.state == storage::State::TOMBSTONE
                ? protocol::VERSION_ENTRY_FLAG_TOMBSTONE
                : static_cast<std::uint8_t>(0),
            protocol::KeyView{entry_keys_[count], info.key_len},
            info.version.hlc,
            info.version.origin
        };

        ++count;
    }

    table = protocol::VersionTablePayload{
        round_sync_id_,
        round_next_chunk_,
        round_chunk_count_,
        count,
        entries
    };

    ++round_next_chunk_;

    if (round_next_chunk_ >= round_chunk_count_) {
        round_active_ = false;
    }

    ++counters_.chunks_sent;
    return true;
}

// --------------------------------------------------------------
// Incoming
// --------------------------------------------------------------

void Tracker::on_local_change(std::size_t slot, std::uint64_t now_ms)
{
    if (slot < storage::MAX_LOGICAL_KEYS) {
        last_change_ms_[slot] = now_ms;
    }
}

void Tracker::queue_push(std::uint8_t slot, node::Id peer)
{
    for (std::size_t i = 0; i < push_count_; ++i) {
        const Push& queued =
            pushes_[(push_head_ + i) % config::AE_PUSH_QUEUE_DEPTH];

        if (queued.slot == slot && queued.peer == peer) {
            return;
        }
    }

    if (push_count_ >= config::AE_PUSH_QUEUE_DEPTH) {
        ++counters_.pushes_dropped_queue_full;
        return;
    }

    pushes_[(push_head_ + push_count_) % config::AE_PUSH_QUEUE_DEPTH] =
        Push{slot, peer};

    ++push_count_;
    ++counters_.pushes_queued;
}

bool Tracker::next_push(Push& push)
{
    if (push_count_ == 0) {
        return false;
    }

    push = pushes_[push_head_];
    push_head_ = (push_head_ + 1) % config::AE_PUSH_QUEUE_DEPTH;
    --push_count_;

    return true;
}

std::size_t Tracker::pending_pushes() const
{
    return push_count_;
}

void Tracker::on_chunk(
    node::Id sender,
    const protocol::VersionTablePayload& table,
    const storage::StorageManager& storage,
    const placement::Resolver& placement,
    placement::AliveMask view,
    std::uint64_t now_ms
)
{
    if (sender < node::MIN ||
        sender > node::MAX ||
        sender == local_node_id_ ||
        table.chunk_count == 0 ||
        table.chunk_count > MAX_CHUNKS ||
        table.chunk_index >= table.chunk_count) {
        return;
    }

    ++counters_.chunks_received;

    // The sender is evidently alive; ownership is judged as if it
    // were in our view (it may still be catching up after a reboot
    // and not heartbeating yet).
    const placement::AliveMask owners_view = with(view, sender);

    Report& report = reports_[peer_index(sender)];

    if (!report.assembling ||
        report.sync_id != table.sync_id ||
        report.chunk_count != table.chunk_count) {

        report.assembling = true;
        report.sync_id = table.sync_id;
        report.chunk_count = table.chunk_count;
        report.received_chunks = 0;
        report.started_ms = now_ms;
        report.seen = 0;
        report.peer_ge = 0;
        report.peer_older = 0;
    }

    report.received_chunks |= 1u << table.chunk_index;

    for (std::size_t i = 0; i < table.entry_count; ++i) {
        const protocol::VersionTableEntry& entry = table.entries[i];

        const int slot = storage.find_slot(
            storage::KeyView{entry.key.data, entry.key.len}
        );

        if (slot < 0) {
            continue;
        }

        storage::EntryInfo info{};

        if (!storage.slot_info(static_cast<std::size_t>(slot), info)) {
            continue;
        }

        report.seen |= bit(static_cast<std::size_t>(slot));

        const int comparison = storage::compare_versions(
            info.version,
            storage::Version{entry.version_hlc, entry.version_origin_id}
        );

        if (comparison > 0) {
            report.peer_older |= bit(static_cast<std::size_t>(slot));

            if (key_owned_by(placement, info, owners_view, sender)) {
                queue_push(static_cast<std::uint8_t>(slot), sender);
            }
        } else {
            report.peer_ge |= bit(static_cast<std::size_t>(slot));
        }
    }

    const std::uint32_t all_chunks =
        table.chunk_count >= 32
            ? 0xFFFFFFFFu
            : ((1u << table.chunk_count) - 1u);

    if (report.received_chunks != all_chunks) {
        return;
    }

    // Complete table: the sender lacks every key it did not list.
    for (std::size_t slot = 0;
         slot < storage::StorageManager::slot_capacity();
         ++slot) {

        if ((report.seen & bit(slot)) != 0) {
            continue;
        }

        storage::EntryInfo info{};

        if (!storage.slot_info(slot, info)) {
            continue;
        }

        // GC marked this entry only after every peer confirmed it, so a
        // peer that lacks it has already dropped it: pushing it back
        // would undo that peer's GC (BRINGUP_LOG problem 11). A peer
        // with an older version is still pushed to, above.
        if (storage.drop_pending(slot)) {
            continue;
        }

        if (key_owned_by(placement, info, owners_view, sender)) {
            queue_push(static_cast<std::uint8_t>(slot), sender);
        }
    }

    report.complete = true;
    report.complete_started_ms = report.started_ms;
    report.complete_at_ms = now_ms;
    report.complete_seen = report.seen;
    report.complete_ge = report.peer_ge;
    report.complete_older = report.peer_older;
    report.assembling = false;

    ++counters_.tables_completed;
}

// --------------------------------------------------------------
// Garbage collection
// --------------------------------------------------------------

bool Tracker::has_fresh_report(node::Id peer, std::uint64_t now_ms) const
{
    if (peer < node::MIN || peer > node::MAX || peer == local_node_id_) {
        return false;
    }

    const Report& report = reports_[peer_index(peer)];

    return report.complete &&
           now_ms >= report.complete_at_ms &&
           now_ms - report.complete_at_ms <= config::AE_REPORT_MAX_AGE_MS;
}

bool Tracker::report_confirms(
    node::Id peer,
    std::size_t slot,
    bool allow_absent,
    std::uint64_t now_ms
) const
{
    if (!has_fresh_report(peer, now_ms)) {
        return false;
    }

    const Report& report = reports_[peer_index(peer)];

    // Comparisons in the report must postdate our last change of
    // this key, or they compared against an older local version.
    if (report.complete_started_ms <= last_change_ms_[slot]) {
        return false;
    }

    if ((report.complete_ge & bit(slot)) != 0) {
        return true;
    }

    return allow_absent &&
           (report.complete_seen & bit(slot)) == 0;
}

std::size_t Tracker::collect_garbage(
    storage::StorageManager& storage,
    const placement::Resolver& placement,
    const GcContext& context
)
{
    std::size_t marked = 0;

    constexpr placement::AliveMask ALL_NODES =
        static_cast<placement::AliveMask>(
            (1u << (node::MAX - node::MIN + 1)) - 1u
        );

    for (std::size_t slot = 0;
         slot < storage::StorageManager::slot_capacity();
         ++slot) {

        storage::EntryInfo info{};

        if (!storage.slot_info(slot, info) || storage.drop_pending(slot)) {
            continue;
        }

        placement::Assignment assignment{};

        if (!placement.locate(info.key, info.key_len, context.view, assignment)) {
            continue;
        }

        // Orphan: someone else owns it and both owners have it.
        if (!placement::is_owner(assignment, local_node_id_)) {
            const bool primary_ok =
                report_confirms(assignment.primary, slot, false, context.now_ms);

            const bool replica_ok =
                assignment.replica == node::INVALID ||
                report_confirms(assignment.replica, slot, false, context.now_ms);

            if (primary_ok && replica_ok &&
                storage.mark_drop(slot) == storage::Result::OK) {
                ++marked;
                ++counters_.gc_orphans;
            }

            continue;
        }

        // Tombstone: old enough and confirmed by every other node.
        if (info.state != storage::State::TOMBSTONE ||
            context.view != ALL_NODES) {
            continue;
        }

        const std::uint64_t written_ms = hlc::physical(info.version.hlc);

        if (context.hlc_physical_now_ms < written_ms ||
            context.hlc_physical_now_ms - written_ms <
                config::TOMBSTONE_GC_MIN_AGE_MS) {
            continue;
        }

        bool everyone_confirms = true;

        for (node::Id peer = node::MIN; peer <= node::MAX; ++peer) {
            if (peer == local_node_id_) {
                continue;
            }

            if (!report_confirms(peer, slot, true, context.now_ms)) {
                everyone_confirms = false;
                break;
            }
        }

        if (everyone_confirms &&
            storage.mark_drop(slot) == storage::Result::OK) {
            ++marked;
            ++counters_.gc_tombstones;
        }
    }

    return marked;
}

const Counters& Tracker::counters() const
{
    return counters_;
}

} // namespace ae

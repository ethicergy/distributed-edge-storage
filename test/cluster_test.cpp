/*
 * Whole-system tests: four real node runtimes on fake EEPROMs and a
 * simulated radio, in virtual time. Each scenario mirrors a row of
 * the hardware test matrix (docs/HARDWARE_TEST_PLAN.md).
 *
 * Run one scenario verbosely:  build/host/test/cluster_test H7 -v
 */

#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <iostream>
#include <string>
#include <vector>

#include "cluster.h"
#include "sensor.h"
#include "test_support.h"

using edge::ClientStatus;
using sim::Cluster;

namespace {

bool g_verbose = false;
Cluster* g_current = nullptr;

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n",              \
                         __FILE__, __LINE__, #condition);                  \
            if (g_current != nullptr && !g_verbose) {                      \
                g_current->dump_logs();                                    \
            }                                                              \
            std::exit(1);                                                  \
        }                                                                  \
    } while (0)

struct Scope {
    explicit Scope(Cluster& cluster) { g_current = &cluster; }
    ~Scope() { g_current = nullptr; }
};

const node::Id ALL[] = {node::A, node::B, node::C, node::D};

// ------------------------------------------------------------------
// H1 + H2: everyone sees everyone; set/get/del from every node for
// keys owned by every node.
// ------------------------------------------------------------------
void h2_basic_operations_from_every_node()
{
    Cluster cluster(11, g_verbose);
    Scope scope(cluster);

    CHECK(cluster.start_all());

    for (node::Id from : ALL) {
        for (node::Id primary : ALL) {
            const std::string key =
                cluster.key_owned_by(std::string("h2-") + sim::letter_name(from) + "-", primary);
            const std::string value = std::string("v-from-") + sim::letter_name(from);

            const sim::OpOutcome set = cluster.set(from, key, value);
            CHECK(set.completed);
            CHECK(set.status == ClientStatus::OK);
            CHECK(set.served_by == primary);

            // Written to the primary AND the replica.
            const placement::Assignment owners = cluster.owners(key);
            CHECK(cluster.local_value(owners.primary, key) == value);
            CHECK(cluster.local_value(owners.replica, key) == value);

            for (node::Id reader : ALL) {
                const sim::OpOutcome get = cluster.get(reader, key);
                CHECK(get.status == ClientStatus::OK);
                CHECK(get.value == value);
            }

            const sim::OpOutcome del = cluster.del(from, key);
            CHECK(del.status == ClientStatus::OK);

            // Tombstone replicated.
            storage::Version version{};
            storage::State state{};
            CHECK(cluster.local_version(owners.replica, key, version, state));
            CHECK(state == storage::State::TOMBSTONE);

            const sim::OpOutcome gone = cluster.get(from, key);
            CHECK(gone.status == ClientStatus::NOT_FOUND);
        }
    }

    // H1: every node heard every other node.
    for (node::Id id : ALL) {
        CHECK(cluster.runtime(id).view(cluster.now()) == 0x0F);
        CHECK(cluster.runtime(id).counters().heartbeats_rx > 0);
    }
}

// ------------------------------------------------------------------
// H3: a key whose primary is the requesting node is served locally
// without a client radio round trip.
// ------------------------------------------------------------------
void h3_local_fast_path()
{
    Cluster cluster(12, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("local-", node::C);

    const std::uint32_t before = cluster.runtime(node::C).counters().client_local_fast_path;

    const sim::OpOutcome set = cluster.set(node::C, key, "here");
    CHECK(set.status == ClientStatus::OK);
    CHECK(set.served_by == node::C);

    const sim::OpOutcome get = cluster.get(node::C, key);
    CHECK(get.status == ClientStatus::OK && get.value == "here");

    CHECK(cluster.runtime(node::C).counters().client_local_fast_path == before + 2);
    CHECK(cluster.runtime(node::C).client_pending() == 0);
}

// ------------------------------------------------------------------
// H4: lost request, lost client ACK, lost REPL_ACK -> retransmission,
// each executed exactly once.
// ------------------------------------------------------------------
void h4_retries_execute_once()
{
    Cluster cluster(13, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("retry-", node::A, node::B);

    // (a) First transmission of the request is lost.
    cluster.runtime(node::C).faults().drop_client_requests = 1;
    std::uint32_t executed = cluster.runtime(node::A).counters().requests_executed;

    sim::OpOutcome out = cluster.set(node::C, key, "one");
    CHECK(out.status == ClientStatus::OK);
    CHECK(out.attempts == 1);
    CHECK(cluster.runtime(node::A).counters().requests_executed == executed + 1);

    // (b) SET_ACK lost: the retry is answered from the dedup cache.
    cluster.runtime(node::A).faults().drop_client_responses = 1;
    executed = cluster.runtime(node::A).counters().requests_executed;
    std::uint32_t replays = cluster.runtime(node::A).counters().dedup_replays;

    out = cluster.set(node::C, key, "two");
    CHECK(out.status == ClientStatus::OK);
    CHECK(out.attempts == 1);
    CHECK(cluster.runtime(node::A).counters().requests_executed == executed + 1);
    CHECK(cluster.runtime(node::A).counters().dedup_replays == replays + 1);

    // (c) REPL_ACK lost: the primary retransmits REPL_REQ; the replica
    // applies once and replays its cached ACK.
    cluster.runtime(node::B).faults().drop_repl_acks = 1;
    const std::uint32_t applied = cluster.runtime(node::B).counters().replicas_applied;
    replays = cluster.runtime(node::B).counters().dedup_replays;

    out = cluster.set(node::C, key, "three");
    CHECK(out.status == ClientStatus::OK);
    CHECK(cluster.runtime(node::B).counters().replicas_applied == applied + 1);
    CHECK(cluster.runtime(node::B).counters().dedup_replays == replays + 1);
    CHECK(cluster.local_value(node::B, key) == "three");

    // (d) First REPL_REQ lost.
    cluster.runtime(node::A).faults().drop_repl_requests = 1;
    out = cluster.set(node::C, key, "four");
    CHECK(out.status == ClientStatus::OK);
    CHECK(cluster.local_value(node::B, key) == "four");
}

// ------------------------------------------------------------------
// H5: the same request delivered twice -> one execution.
// ------------------------------------------------------------------
void h5_duplicate_request()
{
    Cluster cluster(14, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("dup-", node::D);

    cluster.runtime(node::B).faults().duplicate_client_requests = 1;
    const std::uint32_t executed = cluster.runtime(node::D).counters().requests_executed;
    const edge::Counters before = cluster.runtime(node::D).counters();

    const sim::OpOutcome out = cluster.set(node::B, key, "once");
    CHECK(out.status == ClientStatus::OK);

    const edge::Counters after = cluster.runtime(node::D).counters();
    CHECK(after.requests_executed == executed + 1);
    CHECK(after.dedup_replays + after.dedup_suppressed ==
          before.dedup_replays + before.dedup_suppressed + 1);
}

// ------------------------------------------------------------------
// H6: replica dies during a write -> UNDER_REPLICATED (D5) before the
// client's attempt deadline; after it returns, anti-entropy repairs.
// ------------------------------------------------------------------
void h6_replica_failure()
{
    Cluster cluster(15, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("replica-down-", node::B, node::C);

    CHECK(cluster.set(node::A, key, "before").status == ClientStatus::OK);

    // Replica C dies; B still believes C is alive for ~2 s.
    cluster.power_off(node::C);

    const sim::OpOutcome out = cluster.set(node::A, key, "during");
    CHECK(out.completed);
    CHECK(out.status == ClientStatus::UNDER_REPLICATED);
    CHECK(out.attempts == 1);
    CHECK(out.latency_ms < config::CLIENT_ATTEMPT_DEADLINE_MS);
    CHECK(cluster.local_value(node::B, key) == "during");

    // Once membership notices, writes pick a new replica: OK again.
    cluster.run_for(config::MEMBERSHIP_TIMEOUT_MS + 500);
    const placement::Assignment degraded =
        cluster.owners(key, 0x0F & ~placement::mask_of(node::C));
    CHECK(degraded.primary == node::B);

    const sim::OpOutcome later = cluster.set(node::A, key, "after");
    CHECK(later.status == ClientStatus::OK);
    CHECK(cluster.local_value(degraded.replica, key) == "after");

    // C returns with the old value and is repaired.
    CHECK(cluster.local_value(node::C, key) == "before");
    cluster.power_on(node::C);
    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));
    CHECK(cluster.run_until([&] { return cluster.local_value(node::C, key) == "after"; },
                            3 * config::AE_PERIOD_MS));
}

// ------------------------------------------------------------------
// H7 + H8: primary fails -> old replica serves (failover by
// placement); primary returns -> catch-up, and no read ever returns
// the stale value.
// ------------------------------------------------------------------
void h7_h8_primary_failover_and_rejoin()
{
    Cluster cluster(16, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("failover-", node::A, node::D);

    CHECK(cluster.set(node::B, key, "v1").status == ClientStatus::OK);

    cluster.power_off(node::A);
    cluster.run_for(config::MEMBERSHIP_TIMEOUT_MS + 500);

    // H7: D (old replica) is now primary and has the data.
    sim::OpOutcome get = cluster.get(node::B, key);
    CHECK(get.status == ClientStatus::OK);
    CHECK(get.value == "v1");
    CHECK(get.served_by == node::D);

    const sim::OpOutcome set = cluster.set(node::C, key, "v2");
    CHECK(set.status == ClientStatus::OK);
    CHECK(set.served_by == node::D);

    // H8: A comes back holding v1. Read continuously during its
    // whole boot/catch-up/rejoin; v1 must never be returned.
    cluster.power_on(node::A);

    int reads = 0;
    bool saw_stale = false;
    const std::uint64_t until = cluster.now() + 15000;

    while (cluster.now() < until) {
        const sim::OpOutcome read = cluster.get(node::C, key);

        if (read.status == ClientStatus::OK) {
            ++reads;
            if (read.value != "v2") {
                saw_stale = true;
            }
        }

        cluster.run_for(37);
    }

    CHECK(!saw_stale);
    CHECK(reads > 50);
    CHECK(cluster.converged());
    CHECK(cluster.local_value(node::A, key) == "v2");

    get = cluster.get(node::B, key);
    CHECK(get.served_by == node::A);
    CHECK(get.value == "v2");
}

// ------------------------------------------------------------------
// H9: delete while an owner is down -> no resurrection after rejoin.
// ------------------------------------------------------------------
void h9_delete_while_down()
{
    Cluster cluster(17, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key_r = cluster.key_owned_by("zombie-r-", node::B, node::A);
    const std::string key_p = cluster.key_owned_by("zombie-p-", node::A, node::C);

    CHECK(cluster.set(node::D, key_r, "alive").status == ClientStatus::OK);
    CHECK(cluster.set(node::D, key_p, "alive").status == ClientStatus::OK);

    // A is replica of key_r and primary of key_p.
    cluster.power_off(node::A);
    cluster.run_for(config::MEMBERSHIP_TIMEOUT_MS + 500);

    CHECK(edge::write_committed(cluster.del(node::D, key_r).status));
    CHECK(edge::write_committed(cluster.del(node::D, key_p).status));

    cluster.power_on(node::A);
    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));

    // Through several anti-entropy rounds the keys stay deleted.
    for (int round = 0; round < 4; ++round) {
        cluster.run_for(config::AE_PERIOD_MS);

        for (node::Id reader : ALL) {
            CHECK(cluster.get(reader, key_r).status == ClientStatus::NOT_FOUND);
            CHECK(cluster.get(reader, key_p).status == ClientStatus::NOT_FOUND);
        }
    }

    for (node::Id id : ALL) {
        CHECK(cluster.local_value(id, key_r) == "<none>");
        CHECK(cluster.local_value(id, key_p) == "<none>");
    }
}

// ------------------------------------------------------------------
// H10: an old write arriving late is rejected.
// ------------------------------------------------------------------
void h10_stale_write_rejected()
{
    Cluster cluster(18, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("stale-", node::C, node::B);
    CHECK(cluster.set(node::A, key, "current").status == ClientStatus::OK);

    const std::uint32_t stale_before = cluster.runtime(node::C).counters().resync_stale;

    CHECK(cluster.runtime(node::D).inject_stale_write(
        node::C,
        reinterpret_cast<const std::uint8_t*>(key.data()),
        key.size(),
        reinterpret_cast<const std::uint8_t*>("ancient"),
        7,
        hlc::make(1, 0)
    ));

    cluster.run_for(50);

    CHECK(cluster.runtime(node::C).counters().resync_stale == stale_before + 1);
    CHECK(cluster.local_value(node::C, key) == "current");
    CHECK(cluster.get(node::A, key).value == "current");
}

// ------------------------------------------------------------------
// Audit 5.2: the replica holds a NEWER version than the primary's new
// write. The client must not get a plain OK for a write that lost;
// it gets STALE_VERSION on that attempt, makes a new attempt, and
// the final value is the client's on both owners.
// ------------------------------------------------------------------
void replica_newer_than_primary()
{
    Cluster cluster(19, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("race-", node::B, node::D);
    CHECK(cluster.set(node::A, key, "base").status == ClientStatus::OK);

    // Plant a version far in the future on the replica only.
    const hlc::Hlc future = hlc::make(sim::Cluster::physical_now(node::B) + 600000, 0);

    CHECK(cluster.runtime(node::C).inject_stale_write(
        node::D,
        reinterpret_cast<const std::uint8_t*>(key.data()),
        key.size(),
        reinterpret_cast<const std::uint8_t*>("future"),
        6,
        future
    ));
    cluster.run_for(20);
    CHECK(cluster.local_value(node::D, key) == "future");

    const std::uint32_t stale_before = cluster.runtime(node::B).counters().replications_stale;

    const sim::OpOutcome out = cluster.set(node::A, key, "client-wins");
    CHECK(out.status == ClientStatus::OK);
    CHECK(out.attempts == 2);
    CHECK(cluster.runtime(node::B).counters().replications_stale == stale_before + 1);

    CHECK(cluster.local_value(node::B, key) == "client-wins");
    CHECK(cluster.local_value(node::D, key) == "client-wins");
}

// ------------------------------------------------------------------
// H11: anti-entropy rebuilds a replica that lost its whole EEPROM.
// ------------------------------------------------------------------
void h11_anti_entropy_rebuilds_wiped_node()
{
    Cluster cluster(20, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    std::vector<std::string> keys;

    for (int i = 0; i < 30; ++i) {
        const std::string key = "ae-" + std::to_string(i);
        CHECK(cluster.set(static_cast<node::Id>(node::MIN + i % 4), key, "value-" + std::to_string(i)).status == ClientStatus::OK);
        keys.push_back(key);
    }

    cluster.power_off(node::B);
    cluster.node(node::B).eeprom.erase();
    cluster.power_on(node::B);

    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));

    auto b_complete = [&] {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const placement::Assignment owners = cluster.owners(keys[i]);
            if (placement::is_owner(owners, node::B) &&
                cluster.local_value(node::B, keys[i]) != "value-" + std::to_string(i)) {
                return false;
            }
        }
        return true;
    };

    CHECK(cluster.run_until(b_complete, 3 * config::AE_PERIOD_MS));

    // Every owner of every key holds it at the same version.
    for (const std::string& key : keys) {
        const placement::Assignment owners = cluster.owners(key);
        storage::Version vp{}, vr{};
        storage::State sp{}, sr{};
        CHECK(cluster.local_version(owners.primary, key, vp, sp));
        CHECK(cluster.local_version(owners.replica, key, vr, sr));
        CHECK(storage::compare_versions(vp, vr) == 0);
    }
}

// ------------------------------------------------------------------
// H11 (console variant): `format yes` on a live node. It stops
// serving, catches up from its peers and serves again.
// ------------------------------------------------------------------
void h11_format_live_node()
{
    Cluster cluster(26, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("fmt-", node::C, node::B);
    CHECK(cluster.set(node::A, key, "kept").status == ClientStatus::OK);

    CHECK(cluster.runtime(node::C).format_storage(cluster.now()));
    CHECK(cluster.runtime(node::C).state() == edge::NodeState::BOOTING);
    CHECK(cluster.local_value(node::C, key) == "<none>");

    // Meanwhile the replica serves the key.
    const sim::OpOutcome during = cluster.get(node::A, key);
    CHECK(during.status == ClientStatus::OK && during.value == "kept");

    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));
    CHECK(cluster.local_value(node::C, key) == "kept");

    const sim::OpOutcome after = cluster.get(node::A, key);
    CHECK(after.served_by == node::C && after.value == "kept");
}

// ------------------------------------------------------------------
// H12: power cycle of the whole cluster, and a crash in the middle
// of a compaction.
// ------------------------------------------------------------------
void h12_power_cycle_persistence()
{
    Cluster cluster(21, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    for (int i = 0; i < 20; ++i) {
        CHECK(cluster.set(node::A, "persist-" + std::to_string(i), "p" + std::to_string(i)).status == ClientStatus::OK);
    }

    CHECK(edge::write_committed(cluster.del(node::A, "persist-3").status));

    for (node::Id id : ALL) {
        cluster.power_off(id);
    }

    cluster.run_for(5000);
    CHECK(cluster.start_all());

    for (int i = 0; i < 20; ++i) {
        const sim::OpOutcome get = cluster.get(node::C, "persist-" + std::to_string(i));

        if (i == 3) {
            CHECK(get.status == ClientStatus::NOT_FOUND);
        } else {
            CHECK(get.status == ClientStatus::OK && get.value == "p" + std::to_string(i));
        }
    }

    // Drive node D's log towards compaction, then cut power while
    // it compacts.
    const std::string hot = cluster.key_owned_by("hot-", node::D);
    const std::string big(150, 'x');
    int writes = 0;

    while (!cluster.runtime(node::D).storage().compaction_active() && writes < 400) {
        CHECK(edge::write_committed(cluster.set(node::B, hot, big + std::to_string(writes)).status));
        ++writes;

        // Roughly the pace real EEPROM writes allow (~20 writes/s).
        cluster.run_for(40);
    }

    CHECK(cluster.runtime(node::D).storage().compaction_active());
    const std::string last = big + std::to_string(writes - 1);

    cluster.arm_power_loss(node::D, 6000);
    CHECK(cluster.run_until([&] { return !cluster.powered(node::D); }, 20000));

    cluster.power_on(node::D);
    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));

    CHECK(cluster.local_value(node::D, hot) == last);

    for (int i = 0; i < 20; ++i) {
        if (i == 3) {
            continue;
        }

        CHECK(cluster.get(node::D, "persist-" + std::to_string(i)).value == "p" + std::to_string(i));
    }
}

// ------------------------------------------------------------------
// Split brain: a partitioned primary and the rest of the cluster
// both accept writes for the same key. After healing, last-writer-
// wins converges every copy to the higher version.
// ------------------------------------------------------------------
void split_brain_converges()
{
    Cluster cluster(22, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string key = cluster.key_owned_by("split-", node::C, node::A);
    CHECK(cluster.set(node::B, key, "start").status == ClientStatus::OK);

    for (node::Id id : ALL) {
        if (id != node::C) {
            cluster.network().set_link(node::C, id, false);
        }
    }

    cluster.run_for(config::MEMBERSHIP_TIMEOUT_MS + 500);

    const sim::OpOutcome isolated = cluster.set(node::C, key, "from-island");
    CHECK(isolated.status == ClientStatus::UNDER_REPLICATED);

    cluster.run_for(5);
    const sim::OpOutcome majority = cluster.set(node::B, key, "from-majority");
    CHECK(majority.status == ClientStatus::OK);

    for (node::Id id : ALL) {
        if (id != node::C) {
            cluster.network().set_link(node::C, id, true);
        }
    }

    CHECK(cluster.run_until([&] { return cluster.converged(); }, 10000));

    auto converged_value = [&] {
        const std::string c = cluster.local_value(node::C, key);
        const std::string a = cluster.local_value(node::A, key);
        return c == a && (c == "from-island" || c == "from-majority");
    };

    CHECK(cluster.run_until(converged_value, 4 * config::AE_PERIOD_MS));

    storage::Version vc{}, va{};
    storage::State sc{}, sa{};
    CHECK(cluster.local_version(node::C, key, vc, sc));
    CHECK(cluster.local_version(node::A, key, va, sa));
    CHECK(storage::compare_versions(vc, va) == 0);
}

// ------------------------------------------------------------------
// Lossy radio: 15 % frame loss on every link. Every write commits
// and every read returns the last committed value.
// ------------------------------------------------------------------
void lossy_radio()
{
    Cluster cluster(23, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    cluster.network().set_loss(0.15);

    for (int i = 0; i < 40; ++i) {
        const std::string key = "lossy-" + std::to_string(i % 10);
        const std::string value = "v" + std::to_string(i);
        const node::Id from = static_cast<node::Id>(node::MIN + (i * 3) % 4);

        const sim::OpOutcome set = cluster.set(from, key, value);
        CHECK(set.completed);
        CHECK(edge::write_committed(set.status));

        const sim::OpOutcome get = cluster.get(static_cast<node::Id>(node::MIN + i % 4), key);
        CHECK(get.status == ClientStatus::OK);
        CHECK(get.value == value);
    }
}

// ------------------------------------------------------------------
// Garbage collection (D8): a tombstone confirmed by all nodes and
// older than TOMBSTONE_GC_MIN_AGE_MS disappears everywhere; an
// orphan copy left on a temporary replica is dropped after hand-back.
// ------------------------------------------------------------------
void garbage_collection()
{
    Cluster cluster(24, g_verbose);
    Scope scope(cluster);
    CHECK(cluster.start_all());

    const std::string dead = cluster.key_owned_by("gc-dead-", node::A, node::B);
    CHECK(cluster.set(node::C, dead, "x").status == ClientStatus::OK);
    CHECK(cluster.del(node::C, dead).status == ClientStatus::OK);

    // Orphan: key owned by (D, A); while A is down the replica moves
    // elsewhere; when A returns the temporary replica holds an orphan.
    const std::string moved = cluster.key_owned_by("gc-move-", node::D, node::A);
    cluster.power_off(node::A);
    cluster.run_for(config::MEMBERSHIP_TIMEOUT_MS + 500);
    CHECK(cluster.set(node::C, moved, "m").status == ClientStatus::OK);

    const placement::Assignment temporary =
        cluster.owners(moved, 0x0F & ~placement::mask_of(node::A));
    CHECK(temporary.replica != node::A);
    CHECK(cluster.local_value(temporary.replica, moved) == "m");

    cluster.power_on(node::A);
    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));

    auto orphan_gone = [&] {
        return cluster.runtime(temporary.replica).storage().find_slot(
                   storage::KeyView{reinterpret_cast<const std::uint8_t*>(moved.data()), moved.size()}) < 0 ||
               cluster.runtime(temporary.replica).storage().drop_pending(
                   static_cast<std::size_t>(cluster.runtime(temporary.replica).storage().find_slot(
                       storage::KeyView{reinterpret_cast<const std::uint8_t*>(moved.data()), moved.size()})));
    };

    CHECK(cluster.run_until(orphan_gone, 6 * config::AE_PERIOD_MS));
    CHECK(cluster.local_value(node::A, moved) == "m");
    CHECK(cluster.local_value(node::D, moved) == "m");

    // Tombstone GC after the minimum age.
    auto tombstone_dropped_everywhere = [&] {
        for (node::Id id : ALL) {
            storage::StorageManager& store = cluster.runtime(id).storage();
            const int slot = store.find_slot(
                storage::KeyView{reinterpret_cast<const std::uint8_t*>(dead.data()), dead.size()});

            if (slot >= 0 && !store.drop_pending(static_cast<std::size_t>(slot))) {
                return false;
            }
        }
        return true;
    };

    cluster.run_for(config::TOMBSTONE_GC_MIN_AGE_MS / 2);
    CHECK(!tombstone_dropped_everywhere());

    CHECK(cluster.run_until(tombstone_dropped_everywhere,
                            config::TOMBSTONE_GC_MIN_AGE_MS + 6 * config::AE_PERIOD_MS));

    // Compaction physically frees the slots; nothing comes back.
    for (node::Id id : ALL) {
        cluster.runtime(id).request_compaction();
    }

    cluster.run_for(3 * config::AE_PERIOD_MS);

    for (node::Id id : ALL) {
        CHECK(cluster.runtime(id).storage().find_slot(
                  storage::KeyView{reinterpret_cast<const std::uint8_t*>(dead.data()), dead.size()}) < 0);
        CHECK(cluster.get(id, dead).status == ClientStatus::NOT_FOUND);
    }

    CHECK(cluster.get(node::B, moved).value == "m");
}

} // namespace

// ------------------------------------------------------------------
// H13: two hours of sensor traffic (all four nodes sampling at the
// configured period) plus user writes, with a node power-cycled every 20 minutes.
// No NO_SPACE / storage errors, compactions run, data intact, the
// sensor sequence numbers survive reboots.
// ------------------------------------------------------------------
void h13_sensor_soak()
{
    Cluster cluster(25, g_verbose);
    Scope scope(cluster);

    cluster.enable_sensors(config::SENSOR_PERIOD_MS);
    CHECK(cluster.start_all());

    std::mt19937 rng(99);
    std::string user_values[20];

    std::uint32_t sampler_failed = 0;
    std::uint32_t sampler_ok = 0;
    std::uint32_t sampler_degraded = 0;
    std::uint32_t storage_errors = 0;
    std::uint32_t compactions = 0;

    auto harvest = [&](node::Id id) {
        sim::SimNode& n = cluster.node(id);
        if (n.sampler) {
            sampler_failed += n.sampler->stats().writes_failed;
            sampler_ok += n.sampler->stats().writes_ok;
            sampler_degraded += n.sampler->stats().writes_under_replicated;
        }
        if (n.runtime) {
            storage_errors += n.runtime->counters().storage_errors;
            compactions += n.storage->stats().compactions_completed;
        }
    };

    const std::uint64_t duration = 2ULL * 3600ULL * 1000ULL;
    const std::uint64_t start = cluster.now();
    std::uint64_t next_user_write = start + 30000;
    std::uint64_t next_reboot = start + 20ULL * 60ULL * 1000ULL;
    int reboots = 0;

    while (cluster.now() - start < duration) {
        if (cluster.now() >= next_user_write) {
            next_user_write += 30000;
            const int index = static_cast<int>(rng() % 20);
            const node::Id from = static_cast<node::Id>(node::MIN + rng() % 4);
            const std::string value = "u" + std::to_string(cluster.now());

            if (cluster.powered(from) && cluster.runtime(from).state() == edge::NodeState::ACTIVE) {
                const sim::OpOutcome out = cluster.set(from, "user/" + std::to_string(index), value);
                CHECK(edge::write_committed(out.status));
                user_values[index] = value;
            }
        }

        if (cluster.now() >= next_reboot) {
            next_reboot += 20ULL * 60ULL * 1000ULL;
            const node::Id victim = static_cast<node::Id>(node::MIN + reboots % 4);
            harvest(victim);
            cluster.power_off(victim);
            cluster.run_for(3000);
            cluster.power_on(victim);
            ++reboots;
        }

        cluster.run_for(100);
    }

    CHECK(cluster.run_until([&] { return cluster.converged(); }, 15000));

    std::uint32_t final_compactions[4] = {};

    for (node::Id id : ALL) {
        final_compactions[id - node::MIN] = cluster.runtime(id).storage().stats().compactions_completed;
        harvest(id);
    }

    std::uint64_t worst_page_cycles = 0;

    for (node::Id id : ALL) {
        const std::uint64_t cycles = cluster.node(id).eeprom.max_page_cycles();
        worst_page_cycles = cycles > worst_page_cycles ? cycles : worst_page_cycles;
    }

    // AT24C256: 1,000,000 write cycles per page.
    const double hours = static_cast<double>(duration) / 3600000.0;
    const double years = 1e6 / (static_cast<double>(worst_page_cycles) / hours) / (24.0 * 365.0);

    std::printf("    soak: %d reboots, sampler ok %u / degraded %u / failed %u, compactions %u, storage errors %u\n",
                reboots, sampler_ok, sampler_degraded, sampler_failed, compactions, storage_errors);
    std::printf("    wear: hottest EEPROM page %llu cycles in %.1f h -> %.1f years to 1M cycles\n",
                static_cast<unsigned long long>(worst_page_cycles), hours, years);

    CHECK(reboots == 5);
    CHECK(storage_errors == 0);
    CHECK(compactions >= 4);

    // At the default period the hottest page lasts > 8 years.
    if (config::SENSOR_PERIOD_MS >= 30000) {
        CHECK(years > 8.0);
    }
    // Two keys per sample per node.
    const std::uint32_t expected_writes =
        static_cast<std::uint32_t>(4 * 2 * duration / config::SENSOR_PERIOD_MS);

    CHECK(sampler_ok > expected_writes * 9 / 10);
    CHECK(sampler_failed == 0);

    for (node::Id id : ALL) {
        // The node that rebooted last may not have compacted since.
        (void)final_compactions;

        char key[storage::KEY_MAX + 1];
        sensor::latest_key(id, sensor::kind_for_node(id), key, sizeof(key));

        const sim::OpOutcome get = cluster.get(static_cast<node::Id>(node::MIN + (id % 4)), key);
        CHECK(get.status == ClientStatus::OK);

        sensor::Reading reading{};
        CHECK(sensor::decode(reinterpret_cast<const std::uint8_t*>(get.value.data()), get.value.size(), reading));

        // Sequence continued across reboots.
        CHECK(reading.sequence > duration / config::SENSOR_PERIOD_MS * 9 / 10);
    }

    for (int i = 0; i < 20; ++i) {
        if (user_values[i].empty()) {
            continue;
        }

        const sim::OpOutcome get = cluster.get(node::A, "user/" + std::to_string(i));
        CHECK(get.status == ClientStatus::OK);
        CHECK(get.value == user_values[i]);
    }
}

int main(int argc, char** argv)
{
    std::string only;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        } else {
            only = argv[i];
        }
    }

    struct Scenario {
        const char* name;
        void (*run)();
    };

    const Scenario scenarios[] = {
        {"H2", h2_basic_operations_from_every_node},
        {"H3", h3_local_fast_path},
        {"H4", h4_retries_execute_once},
        {"H5", h5_duplicate_request},
        {"H6", h6_replica_failure},
        {"H7H8", h7_h8_primary_failover_and_rejoin},
        {"H9", h9_delete_while_down},
        {"H10", h10_stale_write_rejected},
        {"RACE", replica_newer_than_primary},
        {"H11", h11_anti_entropy_rebuilds_wiped_node},
        {"H11F", h11_format_live_node},
        {"H12", h12_power_cycle_persistence},
        {"SPLIT", split_brain_converges},
        {"LOSS", lossy_radio},
        {"GC", garbage_collection},
        {"H13", h13_sensor_soak},
    };

    for (const Scenario& scenario : scenarios) {
        if (!only.empty() && only != scenario.name) {
            continue;
        }

        scenario.run();
        std::printf("  %-4s PASS\n", scenario.name);
    }

    std::cout << "Cluster tests: PASS\n";
    return 0;
}

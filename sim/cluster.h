#pragma once

/*
 * Host simulation of the 4-node cluster.
 *
 * Runs the real node runtime (src/node_runtime.cpp) and the real
 * storage code on four fake EEPROMs, connected by a simulated radio
 * with latency, loss and partitions, in virtual milliseconds. Nodes
 * can be powered off, rebooted, or crashed in the middle of an
 * EEPROM write. Used by test/cluster_test.cpp and tools/edge_sim.cpp.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "fake_eeprom.h"
#include "node_runtime.h"
#include "sensor.h"
#include "storage.h"

namespace sim {

constexpr std::size_t NODE_COUNT = node::MAX - node::MIN + 1;

class Cluster;

class Network {
public:
    explicit Network(Cluster& cluster, std::uint32_t seed);

    void send(
        node::Id source,
        node::Id destination,
        const std::uint8_t* data,
        std::size_t length
    );

    // Deliver frames whose time has come.
    void deliver_due(std::uint64_t now_ms);

    // Probability that one frame on one link is lost (0..1).
    void set_loss(double loss) { loss_ = loss; }

    // Cut / restore the link between a and b (both directions).
    void set_link(node::Id a, node::Id b, bool up);

    // Drop the next n frames of a message type sent by `source`.
    void drop_next(node::Id source, std::uint8_t message_type, int count);

    std::uint64_t frames_sent() const { return frames_sent_; }
    std::uint64_t frames_lost() const { return frames_lost_; }

private:
    struct InFlight {
        std::uint64_t deliver_at;
        node::Id source;
        node::Id destination;
        std::vector<std::uint8_t> data;
    };

    struct DropRule {
        node::Id source;
        std::uint8_t message_type;
        int remaining;
    };

    bool link_up(node::Id a, node::Id b) const;

    Cluster& cluster_;
    std::mt19937 rng_;
    double loss_ = 0.0;
    bool links_[NODE_COUNT][NODE_COUNT];
    std::vector<InFlight> in_flight_;
    std::vector<DropRule> drop_rules_;
    std::uint64_t frames_sent_ = 0;
    std::uint64_t frames_lost_ = 0;
};

class SimLink final : public edge::Link {
public:
    SimLink(Network& network, node::Id self)
        : network_(network), self_(self)
    {
    }

    bool unicast(node::Id destination, const std::uint8_t* data, std::size_t length) override;
    bool broadcast(const std::uint8_t* data, std::size_t length) override;

private:
    Network& network_;
    node::Id self_;
};

struct SimNode {
    node::Id id = node::INVALID;
    FakeEeprom eeprom;
    std::unique_ptr<SimLink> link;
    std::unique_ptr<storage::StorageManager> storage;
    std::unique_ptr<edge::Runtime> runtime;
    std::unique_ptr<sensor::Source> source;
    std::unique_ptr<sensor::Sampler> sampler;
    bool powered = false;
    std::uint64_t boot_at_ms = 0;
    std::vector<std::string> log;
};

struct OpOutcome {
    bool completed = false;
    edge::ClientStatus status = edge::ClientStatus::TIMEOUT;
    std::string value;
    node::Id served_by = node::INVALID;
    std::uint8_t attempts = 0;
    std::uint32_t latency_ms = 0;
};

class Cluster {
public:
    explicit Cluster(std::uint32_t seed = 1, bool verbose = false);
    ~Cluster();

    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;

    std::uint64_t now() const { return now_ms_; }

    void power_on(node::Id id);
    void power_off(node::Id id);
    void reboot(node::Id id);

    // Power fails after `bytes` more EEPROM bytes have been written.
    void arm_power_loss(node::Id id, std::size_t bytes);

    void step(std::uint64_t ms = 1);
    void run_for(std::uint64_t ms);
    bool run_until(const std::function<bool()>& condition, std::uint64_t timeout_ms);

    // Run a simulated sensor sampler on every node (applies to nodes
    // booted afterwards, and to reboots).
    void enable_sensors(std::uint32_t period_ms);

    // Optional: the sensor source of a node at boot (tests plug in the
    // real-sensor logic over fake inputs). nullptr: SimulatedSource.
    std::function<std::unique_ptr<sensor::Source>(node::Id)> source_factory;

    // Called after every node tick (tools hook bridges in here).
    std::function<void(node::Id, std::uint64_t)> on_node_tick;

    // Boot all four nodes and wait until they are ACTIVE and see
    // each other.
    bool start_all(std::uint64_t timeout_ms = 15000);

    // True when every powered node is ACTIVE and its view equals the
    // set of powered nodes.
    bool converged() const;

    SimNode& node(node::Id id);
    edge::Runtime& runtime(node::Id id);
    bool powered(node::Id id) const;

    Network& network() { return network_; }

    // Synchronous client operation from node `from`.
    OpOutcome set(node::Id from, const std::string& key, const std::string& value, std::uint64_t timeout_ms = 30000);
    OpOutcome get(node::Id from, const std::string& key, std::uint64_t timeout_ms = 30000);
    OpOutcome del(node::Id from, const std::string& key, std::uint64_t timeout_ms = 30000);

    OpOutcome run_op(node::Id from, edge::OpType type, const std::string& key, const std::string& value, std::uint64_t timeout_ms);

    // Direct look into a node's local storage ("<none>" if absent or
    // tombstone).
    std::string local_value(node::Id id, const std::string& key);
    bool local_version(node::Id id, const std::string& key, storage::Version& version, storage::State& state);

    placement::Assignment owners(const std::string& key, placement::AliveMask alive = 0x0F) const;

    // Find a key (prefix + counter) whose primary/replica match.
    std::string key_owned_by(const std::string& prefix, node::Id primary, node::Id replica = node::INVALID) const;

    void deliver(node::Id destination, node::Id source, const std::uint8_t* data, std::size_t length);

    void dump_logs() const;

    static std::uint64_t physical_now(node::Id id);

private:
    void boot(SimNode& node);

    std::uint64_t now_ms_ = 0;
    bool verbose_;
    std::uint32_t sensor_period_ms_ = 0;
    Network network_;
    SimNode nodes_[NODE_COUNT];
    placement::Resolver placement_;
};

const char* letter_name(node::Id id);

} // namespace sim

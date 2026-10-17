#include "cluster.h"

#include <cstdio>
#include <cstring>

namespace sim {

namespace {

// The runtime's HLC takes a plain function pointer, so the per-node
// uptime clocks are selected through a small global table.
Cluster* g_cluster = nullptr;
std::uint64_t g_boot_at[NODE_COUNT] = {};

template <node::Id ID>
std::uint64_t node_clock()
{
    if (g_cluster == nullptr) {
        return 0;
    }

    return g_cluster->now() - g_boot_at[ID - node::MIN];
}

hlc::PhysicalNowFn clock_for(node::Id id)
{
    switch (id) {
        case node::A: return node_clock<node::A>;
        case node::B: return node_clock<node::B>;
        case node::C: return node_clock<node::C>;
        default: return node_clock<node::D>;
    }
}

std::size_t index_of(node::Id id)
{
    return static_cast<std::size_t>(id - node::MIN);
}

void capture_log(void* context, const char* line);

struct LogContext {
    Cluster* cluster;
    SimNode* node;
    bool verbose;
};

LogContext g_log_contexts[NODE_COUNT];

void capture_log(void* context, const char* line)
{
    auto* log = static_cast<LogContext*>(context);

    char stamped[256];
    std::snprintf(
        stamped,
        sizeof(stamped),
        "%8.3f %s",
        static_cast<double>(log->cluster->now()) / 1000.0,
        line
    );

    log->node->log.emplace_back(stamped);

    if (log->verbose) {
        std::printf("%s\n", stamped);
    }
}

} // namespace

const char* letter_name(node::Id id)
{
    static const char* const NAMES[] = {"?", "A", "B", "C", "D"};
    return id <= node::MAX ? NAMES[id] : "?";
}

// ------------------------------------------------------------------
// Network
// ------------------------------------------------------------------

Network::Network(Cluster& cluster, std::uint32_t seed)
    : cluster_(cluster), rng_(seed)
{
    for (auto& row : links_) {
        for (bool& link : row) {
            link = true;
        }
    }
}

bool Network::link_up(node::Id a, node::Id b) const
{
    return links_[index_of(a)][index_of(b)];
}

void Network::set_link(node::Id a, node::Id b, bool up)
{
    links_[index_of(a)][index_of(b)] = up;
    links_[index_of(b)][index_of(a)] = up;
}

void Network::drop_next(node::Id source, std::uint8_t message_type, int count)
{
    drop_rules_.push_back(DropRule{source, message_type, count});
}

void Network::send(
    node::Id source,
    node::Id destination,
    const std::uint8_t* data,
    std::size_t length
)
{
    std::uniform_int_distribution<int> latency(1, 4);
    std::uniform_real_distribution<double> chance(0.0, 1.0);

    for (node::Id target = node::MIN; target <= node::MAX; ++target) {
        if (target == source) {
            continue;
        }

        if (destination != node::BROADCAST && destination != target) {
            continue;
        }

        ++frames_sent_;

        if (!link_up(source, target) || !cluster_.powered(target)) {
            ++frames_lost_;
            continue;
        }

        bool dropped = false;

        for (DropRule& rule : drop_rules_) {
            if (rule.remaining > 0 &&
                rule.source == source &&
                length > 1 &&
                data[1] == rule.message_type) {
                --rule.remaining;
                dropped = true;
                break;
            }
        }

        if (dropped || (loss_ > 0.0 && chance(rng_) < loss_)) {
            ++frames_lost_;
            continue;
        }

        in_flight_.push_back(InFlight{
            cluster_.now() + static_cast<std::uint64_t>(latency(rng_)),
            source,
            target,
            std::vector<std::uint8_t>(data, data + length)
        });
    }
}

void Network::deliver_due(std::uint64_t now_ms)
{
    // Frames may be added while delivering; take the due ones first.
    std::vector<InFlight> due;

    for (std::size_t i = 0; i < in_flight_.size();) {
        if (in_flight_[i].deliver_at <= now_ms) {
            due.push_back(std::move(in_flight_[i]));
            in_flight_.erase(in_flight_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    for (const InFlight& frame : due) {
        cluster_.deliver(
            frame.destination,
            frame.source,
            frame.data.data(),
            frame.data.size()
        );
    }
}

bool SimLink::unicast(node::Id destination, const std::uint8_t* data, std::size_t length)
{
    network_.send(self_, destination, data, length);
    return true;
}

bool SimLink::broadcast(const std::uint8_t* data, std::size_t length)
{
    network_.send(self_, node::BROADCAST, data, length);
    return true;
}

// ------------------------------------------------------------------
// Cluster
// ------------------------------------------------------------------

Cluster::Cluster(std::uint32_t seed, bool verbose)
    : verbose_(verbose), network_(*this, seed)
{
    g_cluster = this;

    // Boards were powered at different times: uptime clocks differ.
    now_ms_ = 50000;

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        SimNode& node = nodes_[index_of(id)];
        node.id = id;
        node.link = std::make_unique<SimLink>(network_, id);
    }

    placement_.begin();
}

Cluster::~Cluster()
{
    if (g_cluster == this) {
        g_cluster = nullptr;
    }
}

std::uint64_t Cluster::physical_now(node::Id id)
{
    return clock_for(id)();
}

SimNode& Cluster::node(node::Id id)
{
    return nodes_[index_of(id)];
}

edge::Runtime& Cluster::runtime(node::Id id)
{
    return *nodes_[index_of(id)].runtime;
}

bool Cluster::powered(node::Id id) const
{
    return nodes_[index_of(id)].powered;
}

void Cluster::boot(SimNode& node)
{
    node.eeprom.power_cycle();
    node.powered = true;

    // Different boot moments -> different uptime clocks per node.
    node.boot_at_ms = now_ms_ - static_cast<std::uint64_t>(node.id) * 1111u;
    g_boot_at[index_of(node.id)] = node.boot_at_ms;

    node.runtime.reset();
    node.storage = std::make_unique<storage::StorageManager>(node.eeprom);
    node.runtime = std::make_unique<edge::Runtime>(
        node.id,
        *node.storage,
        *node.link,
        clock_for(node.id)
    );

    LogContext& context = g_log_contexts[index_of(node.id)];
    context = LogContext{this, &node, verbose_};

    node.runtime->set_log(capture_log, &context, edge::LogLevel::DEBUG);

    char line[64];
    std::snprintf(line, sizeof(line), "[%s] --- power on ---", letter_name(node.id));
    capture_log(&context, line);

    if (!node.runtime->begin(now_ms_, 0x1234u + node.id * 7919u + static_cast<std::uint32_t>(now_ms_))) {
        std::snprintf(line, sizeof(line), "[%s] runtime begin FAILED", letter_name(node.id));
        capture_log(&context, line);
    }

    node.sampler.reset();
    node.source.reset();

    if (sensor_period_ms_ != 0) {
        node.source = std::make_unique<sensor::SimulatedSource>(
            sensor::kind_for_node(node.id),
            0xC0FFEEu + node.id
        );
        node.sampler = std::make_unique<sensor::Sampler>(*node.runtime, *node.source);
        node.sampler->set_period(sensor_period_ms_);
        node.sampler->begin();
    }
}

void Cluster::enable_sensors(std::uint32_t period_ms)
{
    sensor_period_ms_ = period_ms;
}

void Cluster::power_on(node::Id id)
{
    SimNode& node = nodes_[index_of(id)];

    if (!node.powered) {
        boot(node);
    }
}

void Cluster::power_off(node::Id id)
{
    SimNode& node = nodes_[index_of(id)];

    if (!node.powered) {
        return;
    }

    char line[64];
    std::snprintf(line, sizeof(line), "[%s] --- power off ---", letter_name(id));
    capture_log(&g_log_contexts[index_of(id)], line);

    node.powered = false;
    node.sampler.reset();
    node.source.reset();
    node.runtime.reset();
    node.storage.reset();
}

void Cluster::reboot(node::Id id)
{
    power_off(id);
    power_on(id);
}

void Cluster::arm_power_loss(node::Id id, std::size_t bytes)
{
    nodes_[index_of(id)].eeprom.arm_power_loss(bytes);
}

void Cluster::deliver(node::Id destination, node::Id source, const std::uint8_t* data, std::size_t length)
{
    SimNode& node = nodes_[index_of(destination)];

    if (!node.powered || !node.runtime) {
        return;
    }

    node.runtime->on_frame(data, length, source, now_ms_);
}

void Cluster::step(std::uint64_t ms)
{
    for (std::uint64_t i = 0; i < ms; ++i) {
        ++now_ms_;

        network_.deliver_due(now_ms_);

        for (SimNode& node : nodes_) {
            if (!node.powered || !node.runtime) {
                continue;
            }

            node.runtime->tick(now_ms_);

            if (node.sampler) {
                node.sampler->tick(now_ms_);
            }

            if (on_node_tick) {
                on_node_tick(node.id, now_ms_);
            }

            // An EEPROM brown-out takes the whole node down.
            if (!node.eeprom.powered()) {
                power_off(node.id);
            }
        }
    }
}

void Cluster::run_for(std::uint64_t ms)
{
    step(ms);
}

bool Cluster::run_until(const std::function<bool()>& condition, std::uint64_t timeout_ms)
{
    const std::uint64_t deadline = now_ms_ + timeout_ms;

    while (now_ms_ < deadline) {
        if (condition()) {
            return true;
        }

        step(1);
    }

    return condition();
}

bool Cluster::converged() const
{
    placement::AliveMask powered_mask = 0;

    for (const SimNode& node : nodes_) {
        if (node.powered) {
            powered_mask = static_cast<placement::AliveMask>(powered_mask | placement::mask_of(node.id));
        }
    }

    for (const SimNode& node : nodes_) {
        if (!node.powered) {
            continue;
        }

        if (!node.runtime || node.runtime->state() != edge::NodeState::ACTIVE) {
            return false;
        }

        if (node.runtime->view(now_ms_) != powered_mask) {
            return false;
        }
    }

    return true;
}

bool Cluster::start_all(std::uint64_t timeout_ms)
{
    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        power_on(id);
    }

    return run_until([this] { return converged(); }, timeout_ms);
}

namespace {

struct Waiter {
    OpOutcome outcome;
};

void on_complete(void* context, std::uint32_t, const edge::ClientResult& result)
{
    auto* waiter = static_cast<Waiter*>(context);

    waiter->outcome.completed = true;
    waiter->outcome.status = result.status;
    waiter->outcome.value.assign(reinterpret_cast<const char*>(result.value), result.value_length);
    waiter->outcome.served_by = result.served_by;
    waiter->outcome.attempts = result.attempts;
    waiter->outcome.latency_ms = result.latency_ms;
}

} // namespace

OpOutcome Cluster::run_op(
    node::Id from,
    edge::OpType type,
    const std::string& key,
    const std::string& value,
    std::uint64_t timeout_ms
)
{
    Waiter waiter;

    SimNode& node = nodes_[index_of(from)];

    if (!node.powered || !node.runtime) {
        waiter.outcome.status = edge::ClientStatus::NOT_READY;
        return waiter.outcome;
    }

    const edge::ClientStatus accepted = node.runtime->submit(
        type,
        reinterpret_cast<const std::uint8_t*>(key.data()),
        key.size(),
        reinterpret_cast<const std::uint8_t*>(value.data()),
        value.size(),
        on_complete,
        &waiter,
        0,
        now_ms_
    );

    if (accepted != edge::ClientStatus::OK) {
        waiter.outcome.status = accepted;
        return waiter.outcome;
    }

    run_until([&waiter] { return waiter.outcome.completed; }, timeout_ms);

    return waiter.outcome;
}

OpOutcome Cluster::set(node::Id from, const std::string& key, const std::string& value, std::uint64_t timeout_ms)
{
    return run_op(from, edge::OpType::SET, key, value, timeout_ms);
}

OpOutcome Cluster::get(node::Id from, const std::string& key, std::uint64_t timeout_ms)
{
    return run_op(from, edge::OpType::GET, key, std::string(), timeout_ms);
}

OpOutcome Cluster::del(node::Id from, const std::string& key, std::uint64_t timeout_ms)
{
    return run_op(from, edge::OpType::DEL, key, std::string(), timeout_ms);
}

std::string Cluster::local_value(node::Id id, const std::string& key)
{
    SimNode& node = nodes_[index_of(id)];

    if (!node.storage || !node.storage->ready()) {
        // Read the persisted state with a temporary manager.
        FakeEeprom copy;
        copy.copy_from(node.eeprom);
        storage::StorageManager reader(copy);

        if (reader.begin() != storage::Result::OK) {
            return "<unreadable>";
        }

        std::uint8_t buffer[storage::VALUE_MAX];
        std::size_t length = 0;

        if (reader.get(storage::KeyView{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()},
                       buffer, sizeof(buffer), length) != storage::Result::OK) {
            return "<none>";
        }

        return std::string(reinterpret_cast<char*>(buffer), length);
    }

    std::uint8_t buffer[storage::VALUE_MAX];
    std::size_t length = 0;

    if (node.storage->get(
            storage::KeyView{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()},
            buffer,
            sizeof(buffer),
            length
        ) != storage::Result::OK) {
        return "<none>";
    }

    return std::string(reinterpret_cast<char*>(buffer), length);
}

bool Cluster::local_version(node::Id id, const std::string& key, storage::Version& version, storage::State& state)
{
    SimNode& node = nodes_[index_of(id)];

    if (!node.storage) {
        return false;
    }

    return node.storage->get_version(
        storage::KeyView{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()},
        version,
        state
    ) == storage::Result::OK;
}

placement::Assignment Cluster::owners(const std::string& key, placement::AliveMask alive) const
{
    placement::Assignment assignment{};
    placement_.locate(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(), alive, assignment);
    return assignment;
}

std::string Cluster::key_owned_by(const std::string& prefix, node::Id primary, node::Id replica) const
{
    for (int i = 0; i < 100000; ++i) {
        const std::string key = prefix + std::to_string(i);
        const placement::Assignment assignment = owners(key);

        if (assignment.primary == primary &&
            (replica == node::INVALID || assignment.replica == replica)) {
            return key;
        }
    }

    return std::string();
}

void Cluster::dump_logs() const
{
    for (const SimNode& node : nodes_) {
        std::printf("----- log of node %s -----\n", letter_name(node.id));

        for (const std::string& line : node.log) {
            std::printf("%s\n", line.c_str());
        }
    }
}

} // namespace sim

/*
 * kv::Engine: execute() on the primary and apply_versioned() for
 * replication / anti-entropy data, including every rejection branch.
 */

#include <cstring>
#include <iostream>
#include <string>

#include "../sim/fake_eeprom.h"
#include "kv_engine.h"
#include "placement.h"
#include "test_support.h"

namespace {

struct Fixture {
    sim::FakeEeprom device;
    storage::StorageManager store{device};
    placement::Resolver placement;

    Fixture()
    {
        REQUIRE(store.begin() == storage::Result::OK);
        REQUIRE(placement.begin());
    }

    std::string key_with_primary(node::Id primary, placement::AliveMask alive = 0x0F)
    {
        for (int i = 0; i < 10000; ++i) {
            const std::string key = "k" + std::to_string(i);
            placement::Assignment a{};
            placement.locate(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(), alive, a);

            if (a.primary == primary) {
                return key;
            }
        }

        return "";
    }
};

protocol::DecodedMessage message(
    protocol::MessageType type,
    const std::string& key,
    const std::string& value = "",
    std::uint8_t flags = 0,
    protocol::Hlc hlc = 0,
    node::Id origin = node::B
)
{
    protocol::DecodedMessage m{};
    m.header.protocol_version = protocol::PROTOCOL_VERSION;
    m.header.msg_type = type;
    m.header.flags = flags;
    m.header.header_len = protocol::HEADER_SIZE;
    m.header.request_id = 1;
    m.header.origin_node_id = origin;
    m.header.sender_node_id = origin;
    m.header.destination_node_id = node::A;
    m.header.hlc = hlc;

    const protocol::KeyView k{reinterpret_cast<const std::uint8_t*>(key.data()), static_cast<std::uint8_t>(key.size())};
    const protocol::ValueView v{reinterpret_cast<const std::uint8_t*>(value.data()), static_cast<std::uint8_t>(value.size())};

    switch (type) {
        case protocol::MessageType::SET_REQ: m.payload.set_request = {k, v}; break;
        case protocol::MessageType::GET_REQ: m.payload.get_request = {k}; break;
        case protocol::MessageType::DEL_REQ: m.payload.delete_request = {k}; break;
        case protocol::MessageType::REPL_REQ: m.payload.replication_request = {k, v}; break;
        case protocol::MessageType::RESYNC_DATA: m.payload.resync_data = {k, v}; break;
        default: break;
    }

    return m;
}

void test_not_ready_before_begin()
{
    Fixture f;
    kv::Engine engine(node::A, f.store, f.placement);
    kv::OperationResponse r{};

    REQUIRE(engine.execute(message(protocol::MessageType::GET_REQ, "x"), 0x0F, {1, node::A}, r) == kv::Result::NOT_READY);
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::RESYNC_DATA, "x"), r) == kv::Result::NOT_READY);

    kv::Engine bad(node::INVALID, f.store, f.placement);
    REQUIRE(!bad.begin());
}

void test_execute_on_primary()
{
    Fixture f;
    kv::Engine engine(node::A, f.store, f.placement);
    REQUIRE(engine.begin());

    const std::string mine = f.key_with_primary(node::A);
    const std::string theirs = f.key_with_primary(node::C);
    kv::OperationResponse r{};

    // SET / GET / DEL with the version supplied by the caller.
    REQUIRE(engine.execute(message(protocol::MessageType::SET_REQ, mine, "v1"), 0x0F, {100, node::A}, r) == kv::Result::OK);
    REQUIRE(r.assignment.primary == node::A);
    REQUIRE(r.assignment.replica != node::INVALID && r.assignment.replica != node::A);
    REQUIRE(std::string(reinterpret_cast<char*>(r.key), r.key_length) == mine);

    REQUIRE(engine.execute(message(protocol::MessageType::GET_REQ, mine), 0x0F, {0, node::A}, r) == kv::Result::OK);
    REQUIRE(std::string(reinterpret_cast<char*>(r.value), r.value_length) == "v1");

    // A mutation with an older version is stale.
    REQUIRE(engine.execute(message(protocol::MessageType::SET_REQ, mine, "old"), 0x0F, {50, node::A}, r) == kv::Result::STALE_VERSION);

    REQUIRE(engine.execute(message(protocol::MessageType::DEL_REQ, mine), 0x0F, {200, node::A}, r) == kv::Result::OK);
    REQUIRE(engine.execute(message(protocol::MessageType::GET_REQ, mine), 0x0F, {0, node::A}, r) == kv::Result::NOT_FOUND);
    REQUIRE(r.value_length == 0);

    // Not the primary in this view.
    REQUIRE(engine.execute(message(protocol::MessageType::GET_REQ, theirs), 0x0F, {0, node::A}, r) == kv::Result::NOT_RESPONSIBLE);

    // ...but primary once C is considered dead (failover).
    const placement::AliveMask without_c = 0x0F & ~placement::mask_of(node::C);
    placement::Assignment a{};
    f.placement.locate(reinterpret_cast<const std::uint8_t*>(theirs.data()), theirs.size(), without_c, a);
    const kv::Result failover = engine.execute(message(protocol::MessageType::GET_REQ, theirs), without_c, {0, node::A}, r);
    REQUIRE(failover == (a.primary == node::A ? kv::Result::NOT_FOUND : kv::Result::NOT_RESPONSIBLE));

    // Rejections.
    REQUIRE(engine.execute(message(protocol::MessageType::REPL_REQ, mine), 0x0F, {1, node::A}, r) == kv::Result::INVALID_REQUEST);
    REQUIRE(engine.execute(message(protocol::MessageType::GET_REQ, ""), 0x0F, {1, node::A}, r) == kv::Result::INVALID_REQUEST);

    const std::string long_key(storage::KEY_MAX + 1, 'k');
    REQUIRE(engine.execute(message(protocol::MessageType::GET_REQ, long_key), 0x0F, {1, node::A}, r) == kv::Result::KEY_TOO_LARGE);

    const std::string long_value(storage::VALUE_MAX + 1, 'v');
    REQUIRE(engine.execute(message(protocol::MessageType::SET_REQ, mine, long_value.substr(0, 255)), 0x0F, {300, node::A}, r) == kv::Result::VALUE_TOO_LARGE);
}

void test_apply_versioned()
{
    Fixture f;
    kv::Engine engine(node::C, f.store, f.placement);
    REQUIRE(engine.begin());
    kv::OperationResponse r{};

    // REPL_REQ must carry FLAG_REPLICATED.
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::REPL_REQ, "key", "v", 0, 10, node::A), r) == kv::Result::INVALID_REQUEST);

    REQUIRE(engine.apply_versioned(message(protocol::MessageType::REPL_REQ, "key", "v", protocol::FLAG_REPLICATED, 10, node::A), r) == kv::Result::OK);

    storage::Version version{};
    storage::State state{};
    REQUIRE(f.store.get_version(storage::KeyView{reinterpret_cast<const std::uint8_t*>("key"), 3}, version, state) == storage::Result::OK);
    REQUIRE(version.hlc == 10 && version.origin == node::A && state == storage::State::LIVE);

    // Duplicate / older data is stale.
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::REPL_REQ, "key", "v", protocol::FLAG_REPLICATED, 10, node::A), r) == kv::Result::STALE_VERSION);
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::RESYNC_DATA, "key", "old", 0, 9, node::D), r) == kv::Result::STALE_VERSION);

    // Tombstone replication; a tombstone must not carry a value.
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::REPL_REQ, "key", "x",
                                           protocol::FLAG_REPLICATED | protocol::FLAG_TOMBSTONE, 20, node::A), r) == kv::Result::INVALID_REQUEST);
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::REPL_REQ, "key", "",
                                           protocol::FLAG_REPLICATED | protocol::FLAG_TOMBSTONE, 20, node::A), r) == kv::Result::OK);
    REQUIRE(f.store.get_version(storage::KeyView{reinterpret_cast<const std::uint8_t*>("key"), 3}, version, state) == storage::Result::OK);
    REQUIRE(state == storage::State::TOMBSTONE);

    // RESYNC_DATA needs no flag and no ownership; newer wins.
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::RESYNC_DATA, "key", "newer", 0, 30, node::B), r) == kv::Result::OK);

    std::uint8_t value[16];
    std::size_t length = 0;
    REQUIRE(f.store.get(storage::KeyView{reinterpret_cast<const std::uint8_t*>("key"), 3}, value, sizeof(value), length) == storage::Result::OK);
    REQUIRE(std::string(reinterpret_cast<char*>(value), length) == "newer");

    // Wrong message types and malformed keys.
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::SET_REQ, "key", "v"), r) == kv::Result::INVALID_REQUEST);
    REQUIRE(engine.apply_versioned(message(protocol::MessageType::RESYNC_DATA, "", "v", 0, 40), r) == kv::Result::INVALID_REQUEST);

    // Storage capacity errors surface as STORAGE_ERROR.
    for (int i = 0; i < 70; ++i) {
        const std::string key = "fill" + std::to_string(i);
        const kv::Result result = engine.apply_versioned(message(protocol::MessageType::RESYNC_DATA, key, "x", 0, 100 + i), r);

        if (i < static_cast<int>(storage::MAX_LOGICAL_KEYS) - 1) {
            REQUIRE(result == kv::Result::OK);
        } else {
            REQUIRE(result == kv::Result::STORAGE_ERROR);
        }
    }

    REQUIRE(std::string(kv::to_string(kv::Result::REPLICATION_FAILED)) == "REPLICATION_FAILED");
}

} // namespace

int main()
{
    test_not_ready_before_begin();
    test_execute_on_primary();
    test_apply_versioned();

    std::cout << "KV engine tests: PASS\n";
    return 0;
}

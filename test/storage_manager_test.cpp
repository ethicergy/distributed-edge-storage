/*
 * StorageManager against the in-RAM EEPROM.
 *
 * Covers append/recover, stale and tombstone rules, capacity limits,
 * automatic + incremental compaction, garbage-collection drops, and
 * power loss at every byte of an append and throughout a compaction.
 */

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "../sim/fake_eeprom.h"
#include "storage.h"
#include "storage_record.h"
#include "test_support.h"

using storage::KeyView;
using storage::Result;
using storage::State;
using storage::StorageManager;
using storage::ValueView;
using storage::Version;

namespace {

KeyView key_of(const std::string& text)
{
    return KeyView{
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size()
    };
}

ValueView value_of(const std::string& text)
{
    return ValueView{
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size()
    };
}

Version v(std::uint64_t hlc, node::Id origin = node::A)
{
    return Version{hlc, origin};
}

std::string read_string(StorageManager& store, const std::string& key)
{
    std::uint8_t buffer[storage::VALUE_MAX]{};
    std::size_t length = 0;

    const Result result = store.get(
        key_of(key),
        buffer,
        sizeof(buffer),
        length
    );

    if (result == Result::NOT_FOUND) {
        return "<none>";
    }

    if (result != Result::OK) {
        return "<error>";
    }

    return std::string(reinterpret_cast<char*>(buffer), length);
}

Version version_of(StorageManager& store, const std::string& key)
{
    Version version{};
    State state{};

    if (store.get_version(key_of(key), version, state) != Result::OK) {
        return Version{0, node::INVALID};
    }

    return version;
}

bool same(Version lhs, Version rhs)
{
    return storage::compare_versions(lhs, rhs) == 0;
}

void test_fresh_set_get_and_reboot()
{
    sim::FakeEeprom device;

    {
        StorageManager store(device);
        REQUIRE(store.begin() == Result::OK);
        REQUIRE(store.stats().boot_kind == storage::BootKind::FRESH_FORMAT);

        REQUIRE(store.set(key_of("alpha"), value_of("one"), v(10)) == Result::OK);
        REQUIRE(store.set(key_of("beta"), value_of("two"), v(11)) == Result::OK);
        REQUIRE(store.set(key_of("alpha"), value_of("three"), v(12)) == Result::OK);

        REQUIRE(read_string(store, "alpha") == "three");
        REQUIRE(read_string(store, "beta") == "two");
        REQUIRE(read_string(store, "gamma") == "<none>");
    }

    StorageManager reopened(device);
    REQUIRE(reopened.begin() == Result::OK);
    REQUIRE(reopened.stats().boot_kind == storage::BootKind::RECOVERED);
    REQUIRE(read_string(reopened, "alpha") == "three");
    REQUIRE(read_string(reopened, "beta") == "two");
    REQUIRE(same(version_of(reopened, "alpha"), v(12)));

    storage::Hlc max_hlc = 0;
    REQUIRE(reopened.max_persisted_hlc(max_hlc) == Result::OK);
    REQUIRE(max_hlc == 12);
}

void test_version_rules()
{
    sim::FakeEeprom device;
    StorageManager store(device);
    REQUIRE(store.begin() == Result::OK);

    REQUIRE(store.set(key_of("k"), value_of("new"), v(100, node::B)) == Result::OK);

    // Older and equal versions are rejected.
    REQUIRE(store.set(key_of("k"), value_of("old"), v(99, node::D)) == Result::STALE_VERSION);
    REQUIRE(store.set(key_of("k"), value_of("same"), v(100, node::B)) == Result::STALE_VERSION);

    // Same HLC: origin breaks the tie.
    REQUIRE(store.set(key_of("k"), value_of("tie"), v(100, node::A)) == Result::STALE_VERSION);
    REQUIRE(store.set(key_of("k"), value_of("tie-win"), v(100, node::C)) == Result::OK);
    REQUIRE(read_string(store, "k") == "tie-win");

    // Tombstone hides the value and blocks older writes.
    REQUIRE(store.remove(key_of("k"), v(200)) == Result::OK);
    REQUIRE(read_string(store, "k") == "<none>");
    REQUIRE(store.set(key_of("k"), value_of("zombie"), v(150)) == Result::STALE_VERSION);
    REQUIRE(read_string(store, "k") == "<none>");

    Version version{};
    State state{};
    REQUIRE(store.get_version(key_of("k"), version, state) == Result::OK);
    REQUIRE(state == State::TOMBSTONE);
    REQUIRE(same(version, v(200)));

    // A newer write after the tombstone revives the key.
    REQUIRE(store.set(key_of("k"), value_of("reborn"), v(300)) == Result::OK);
    REQUIRE(read_string(store, "k") == "reborn");

    // Argument validation.
    std::string long_key(storage::KEY_MAX + 1, 'x');
    std::string long_value(storage::VALUE_MAX + 1, 'y');
    REQUIRE(store.set(key_of(long_key), value_of("v"), v(400)) == Result::KEY_TOO_LARGE);
    REQUIRE(store.set(key_of("k"), value_of(long_value), v(400)) == Result::VALUE_TOO_LARGE);
    REQUIRE(store.set(key_of(""), value_of("v"), v(400)) == Result::INVALID_ARGUMENT);
    REQUIRE(store.apply(key_of("k"), value_of("x"), v(400), State::TOMBSTONE) == Result::INVALID_ARGUMENT);
}

void test_index_full()
{
    sim::FakeEeprom device;
    StorageManager store(device);
    REQUIRE(store.begin() == Result::OK);

    for (std::size_t i = 0; i < storage::MAX_LOGICAL_KEYS; ++i) {
        const std::string key = "key" + std::to_string(i);
        REQUIRE(store.set(key_of(key), value_of("v"), v(i + 1)) == Result::OK);
    }

    REQUIRE(store.set(key_of("one-too-many"), value_of("v"), v(1000)) == Result::INDEX_FULL);

    // Existing keys can still be overwritten.
    REQUIRE(store.set(key_of("key0"), value_of("w"), v(1001)) == Result::OK);
    REQUIRE(store.stats().slots_used == storage::MAX_LOGICAL_KEYS);
}

void test_no_space_triggers_compaction()
{
    sim::FakeEeprom device;
    StorageManager store(device);
    REQUIRE(store.begin() == Result::OK);

    const std::string big(150, 'b');
    std::uint64_t hlc = 1;
    bool saw_no_space = false;

    for (int i = 0; i < 400; ++i) {
        const Result result =
            store.set(key_of("hot"), value_of(big), v(hlc++));

        if (result == Result::NO_SPACE) {
            saw_no_space = true;
            REQUIRE(store.compaction_active());
            break;
        }

        REQUIRE(result == Result::OK);
    }

    REQUIRE(saw_no_space);

    bool done = false;
    while (!done) {
        REQUIRE(store.compaction_step(done) == Result::OK);
    }

    REQUIRE(store.stats().compactions_completed == 1);
    REQUIRE(store.set(key_of("hot"), value_of("after"), v(hlc++)) == Result::OK);
    REQUIRE(read_string(store, "hot") == "after");
}

void test_compaction_trigger_and_reboot()
{
    sim::FakeEeprom device;

    {
        StorageManager store(device);
        REQUIRE(store.begin() == Result::OK);

        std::uint64_t hlc = 1;

        for (int round = 0; round < 3; ++round) {
            while (!store.compaction_needed()) {
                for (int k = 0; k < 10; ++k) {
                    const std::string key = "s" + std::to_string(k);
                    const std::string value =
                        "round" + std::to_string(round) + "-" + std::to_string(hlc);
                    REQUIRE(store.set(key_of(key), value_of(value), v(hlc++)) == Result::OK);
                }
            }

            REQUIRE(store.compact() == Result::OK);
            REQUIRE(!store.compaction_needed());
        }

        REQUIRE(store.remove(key_of("s3"), v(hlc++)) == Result::OK);

        const storage::Stats stats = store.stats();
        REQUIRE(stats.compactions_completed == 3);
        REQUIRE(stats.live_keys == 9);
        REQUIRE(stats.tombstones == 1);
    }

    StorageManager reopened(device);
    REQUIRE(reopened.begin() == Result::OK);
    REQUIRE(reopened.stats().live_keys == 9);
    REQUIRE(reopened.stats().tombstones == 1);
    REQUIRE(read_string(reopened, "s3") == "<none>");
    REQUIRE(read_string(reopened, "s0").rfind("round2-", 0) == 0);
}

void test_writes_during_incremental_compaction()
{
    sim::FakeEeprom device;
    std::uint64_t hlc = 1;

    {
        StorageManager store(device);
        REQUIRE(store.begin() == Result::OK);

        for (int k = 0; k < 20; ++k) {
            REQUIRE(store.set(key_of("k" + std::to_string(k)), value_of("initial"), v(hlc++)) == Result::OK);
        }

        REQUIRE(store.compaction_begin() == Result::OK);

        bool done = false;
        int step = 0;

        while (!done) {
            REQUIRE(store.compaction_step(done) == Result::OK);

            // Interleave: overwrite, delete, and create keys.
            if (step % 3 == 0) {
                // k19 is never rewritten here; it is deleted below.
                const std::string key = "k" + std::to_string(step % 19);
                REQUIRE(store.set(key_of(key), value_of("during-" + std::to_string(step)), v(hlc++)) == Result::OK);
            }

            if (step == 40) {
                REQUIRE(store.remove(key_of("k19"), v(hlc++)) == Result::OK);
            }

            if (step == 50) {
                REQUIRE(store.set(key_of("brand-new"), value_of("fresh"), v(hlc++)) == Result::OK);
            }

            ++step;
        }

        REQUIRE(store.stats().compactions_completed == 1);
        REQUIRE(read_string(store, "brand-new") == "fresh");
        REQUIRE(read_string(store, "k19") == "<none>");
    }

    // Everything written during the compaction survives a reboot,
    // i.e. it really is in the new bank.
    StorageManager before_reboot(device);
    REQUIRE(before_reboot.begin() == Result::OK);
    REQUIRE(read_string(before_reboot, "brand-new") == "fresh");
    REQUIRE(read_string(before_reboot, "k19") == "<none>");
    REQUIRE(before_reboot.stats().slots_used == 21);
}

void test_drop_and_revive()
{
    sim::FakeEeprom device;

    {
        StorageManager store(device);
        REQUIRE(store.begin() == Result::OK);

        REQUIRE(store.set(key_of("orphan"), value_of("x"), v(1)) == Result::OK);
        REQUIRE(store.set(key_of("keeper"), value_of("y"), v(2)) == Result::OK);
        REQUIRE(store.set(key_of("revived"), value_of("z"), v(3)) == Result::OK);

        REQUIRE(store.mark_drop(static_cast<std::size_t>(store.find_slot(key_of("orphan")))) == Result::OK);
        REQUIRE(store.mark_drop(static_cast<std::size_t>(store.find_slot(key_of("revived")))) == Result::OK);

        // A newer version cancels the drop.
        REQUIRE(store.set(key_of("revived"), value_of("zz"), v(4)) == Result::OK);
        REQUIRE(!store.drop_pending(static_cast<std::size_t>(store.find_slot(key_of("revived")))));

        REQUIRE(store.stats().slots_pending_drop == 1);
        REQUIRE(store.compact() == Result::OK);

        REQUIRE(store.find_slot(key_of("orphan")) < 0);
        REQUIRE(store.stats().keys_dropped == 1);
        REQUIRE(store.stats().slots_used == 2);
    }

    StorageManager reopened(device);
    REQUIRE(reopened.begin() == Result::OK);
    REQUIRE(reopened.find_slot(key_of("orphan")) < 0);
    REQUIRE(read_string(reopened, "keeper") == "y");
    REQUIRE(read_string(reopened, "revived") == "zz");
}

void test_mid_log_corruption_detected()
{
    sim::FakeEeprom device;

    {
        StorageManager store(device);
        REQUIRE(store.begin() == Result::OK);

        for (int i = 0; i < 10; ++i) {
            REQUIRE(store.set(key_of("k" + std::to_string(i)), value_of("value"), v(i + 1)) == Result::OK);
        }
    }

    // Flip a byte inside the second record of bank A.
    device.raw()[storage::metadata::BANK_A_BASE + storage::metadata::BANK_RECORD_OFFSET + 40] ^= 0x5A;

    StorageManager reopened(device);
    REQUIRE(reopened.begin() == Result::CORRUPTED);

    // format() always recovers to an empty, usable store.
    REQUIRE(reopened.format() == Result::OK);
    REQUIRE(reopened.stats().slots_used == 0);
    REQUIRE(reopened.set(key_of("after"), value_of("ok"), v(1)) == Result::OK);

    StorageManager again(device);
    REQUIRE(again.begin() == Result::OK);
    REQUIRE(read_string(again, "after") == "ok");
}

/*
 * Power loss at every byte of one append. After the reboot the key
 * has either the old or the new version (never garbage), the store
 * accepts further writes, and those survive another reboot.
 */
void test_power_loss_during_append()
{
    sim::FakeEeprom golden;

    {
        StorageManager store(golden);
        REQUIRE(store.begin() == Result::OK);
        REQUIRE(store.set(key_of("a"), value_of("old-a"), v(1)) == Result::OK);
        REQUIRE(store.set(key_of("b"), value_of("old-b"), v(2)) == Result::OK);
    }

    // Measure how many bytes the append writes.
    std::uint64_t append_bytes = 0;
    {
        sim::FakeEeprom probe;
        probe.copy_from(golden);
        StorageManager store(probe);
        REQUIRE(store.begin() == Result::OK);
        const std::uint64_t before = probe.bytes_written();
        REQUIRE(store.set(key_of("a"), value_of("new-a-value"), v(3)) == Result::OK);
        append_bytes = probe.bytes_written() - before;
    }

    REQUIRE(append_bytes > 20);

    int torn_tails = 0;

    for (std::uint64_t budget = 0; budget <= append_bytes; ++budget) {
        sim::FakeEeprom device;
        device.copy_from(golden);

        {
            StorageManager store(device);
            REQUIRE(store.begin() == Result::OK);
            device.arm_power_loss(static_cast<std::size_t>(budget));
            const Result result =
                store.set(key_of("a"), value_of("new-a-value"), v(3));
            REQUIRE(result == Result::OK || !device.powered());
        }

        device.power_cycle();

        StorageManager rebooted(device);
        REQUIRE(rebooted.begin() == Result::OK);

        if (rebooted.stats().boot_kind == storage::BootKind::RECOVERED_TORN_TAIL) {
            ++torn_tails;
        }

        const std::string a = read_string(rebooted, "a");
        REQUIRE(a == "old-a" || a == "new-a-value");
        REQUIRE(read_string(rebooted, "b") == "old-b");

        if (a == "old-a") {
            REQUIRE(same(version_of(rebooted, "a"), v(1)));
        } else {
            REQUIRE(same(version_of(rebooted, "a"), v(3)));
        }

        // The store keeps working after recovery...
        REQUIRE(rebooted.set(key_of("c"), value_of("short"), v(10)) == Result::OK);
        REQUIRE(rebooted.set(key_of("a"), value_of("final"), v(11)) == Result::OK);

        // ...and those writes survive another reboot.
        StorageManager again(device);
        REQUIRE(again.begin() == Result::OK);
        REQUIRE(read_string(again, "a") == "final");
        REQUIRE(read_string(again, "b") == "old-b");
        REQUIRE(read_string(again, "c") == "short");
    }

    REQUIRE(torn_tails > 0);
}

/*
 * Power loss throughout a compaction (every few bytes of roughly
 * 20 KB of writes). After the reboot every key is at its latest
 * version, from whichever bank is authoritative.
 */
void test_power_loss_during_compaction()
{
    sim::FakeEeprom golden;
    std::uint64_t hlc = 1;

    struct Expected {
        std::string key;
        std::string value; // "<none>" for a tombstone
    };

    Expected expected[24];

    {
        StorageManager store(golden);
        REQUIRE(store.begin() == Result::OK);

        for (int k = 0; k < 24; ++k) {
            for (int rev = 0; rev < 3; ++rev) {
                const std::string key = "key-" + std::to_string(k);
                const std::string value = "v" + std::to_string(rev) + "-" + std::string(static_cast<std::size_t>(k * 3), 'x');
                REQUIRE(store.set(key_of(key), value_of(value), v(hlc++)) == Result::OK);
                expected[k] = Expected{key, value};
            }
        }

        for (int k = 0; k < 24; k += 5) {
            REQUIRE(store.remove(key_of(expected[k].key), v(hlc++)) == Result::OK);
            expected[k].value = "<none>";
        }
    }

    std::uint64_t compaction_bytes = 0;
    {
        sim::FakeEeprom probe;
        probe.copy_from(golden);
        StorageManager store(probe);
        REQUIRE(store.begin() == Result::OK);
        const std::uint64_t before = probe.bytes_written();
        REQUIRE(store.compact() == Result::OK);
        compaction_bytes = probe.bytes_written() - before;
    }

    REQUIRE(compaction_bytes > 16000);

    int new_bank = 0;
    int old_bank = 0;

    // Every byte of the copy/finalize part, a stride over the clear.
    for (std::uint64_t budget = 0; budget <= compaction_bytes;) {
        sim::FakeEeprom device;
        device.copy_from(golden);

        std::uint8_t bank_before = 0;

        {
            StorageManager store(device);
            REQUIRE(store.begin() == Result::OK);
            bank_before = store.stats().active_bank;
            device.arm_power_loss(static_cast<std::size_t>(budget));
            const Result result = store.compact();
            REQUIRE(result == Result::OK || !device.powered());
        }

        device.power_cycle();

        StorageManager rebooted(device);
        REQUIRE(rebooted.begin() == Result::OK);

        if (rebooted.stats().active_bank == bank_before) {
            ++old_bank;
        } else {
            ++new_bank;
        }

        for (const Expected& item : expected) {
            REQUIRE(read_string(rebooted, item.key) == item.value);
        }

        REQUIRE(rebooted.stats().slots_used == 24);

        // A compaction after the crash completes normally.
        REQUIRE(rebooted.compact() == Result::OK);

        for (const Expected& item : expected) {
            REQUIRE(read_string(rebooted, item.key) == item.value);
        }

        const bool in_clear_phase =
            budget < storage::metadata::BANK_RECORD_AREA_END - 256;

        const std::uint64_t next = budget + (in_clear_phase ? 61 : 3);

        // Always include the run where the final commit byte lands.
        budget = (budget < compaction_bytes && next > compaction_bytes)
            ? compaction_bytes
            : next;
    }

    REQUIRE(new_bank > 0);
    REQUIRE(old_bank > 0);
}

void test_iteration_api()
{
    sim::FakeEeprom device;
    StorageManager store(device);
    REQUIRE(store.begin() == Result::OK);

    REQUIRE(store.set(key_of("x"), value_of("1"), v(5, node::C)) == Result::OK);
    REQUIRE(store.remove(key_of("y"), v(6, node::D)) == Result::OK);

    int seen = 0;

    for (std::size_t slot = 0; slot < StorageManager::slot_capacity(); ++slot) {
        storage::EntryInfo info{};

        if (!store.slot_info(slot, info)) {
            continue;
        }

        ++seen;
        const std::string key(reinterpret_cast<char*>(info.key), info.key_len);

        if (key == "x") {
            REQUIRE(info.state == State::LIVE);
            REQUIRE(info.version.hlc == 5 && info.version.origin == node::C);

            std::uint8_t buffer[8]{};
            std::size_t length = 0;
            REQUIRE(store.read_slot_value(slot, buffer, sizeof(buffer), length) == Result::OK);
            REQUIRE(length == 1 && buffer[0] == '1');
        } else {
            REQUIRE(key == "y");
            REQUIRE(info.state == State::TOMBSTONE);
        }
    }

    REQUIRE(seen == 2);
}

} // namespace

int main()
{
    test_fresh_set_get_and_reboot();
    test_version_rules();
    test_index_full();
    test_no_space_triggers_compaction();
    test_compaction_trigger_and_reboot();
    test_writes_during_incremental_compaction();
    test_drop_and_revive();
    test_mid_log_corruption_detected();
    test_power_loss_during_append();
    test_power_loss_during_compaction();
    test_iteration_api();

    std::cout << "Storage manager tests: PASS\n";
    return 0;
}

#include "console.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hash_ring.h"
#include "consistent_hash.h"

#ifndef EDGE_FAULT_INJECTION
#define EDGE_FAULT_INJECTION 0
#endif

namespace console {

namespace {

char letter(node::Id id)
{
    return (id >= node::MIN && id <= node::MAX)
        ? static_cast<char>('A' + (id - node::MIN))
        : '-';
}

node::Id parse_node(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return node::INVALID;
    }

    const char c = text[0];

    if (c >= 'A' && c <= 'D') return static_cast<node::Id>(node::MIN + (c - 'A'));
    if (c >= 'a' && c <= 'd') return static_cast<node::Id>(node::MIN + (c - 'a'));
    if (c >= '1' && c <= '4') return static_cast<node::Id>(c - '0');

    return node::INVALID;
}

// Splits "word rest..." in place; returns rest (never null).
char* split(char* text)
{
    while (*text == ' ') ++text;

    char* rest = text;

    while (*rest != '\0' && *rest != ' ') ++rest;

    if (*rest == ' ') {
        *rest++ = '\0';
        while (*rest == ' ') ++rest;
    }

    return rest;
}

void view_text(placement::AliveMask mask, char (&out)[5])
{
    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        out[id - node::MIN] = (mask & placement::mask_of(id)) ? letter(id) : '-';
    }

    out[4] = '\0';
}

void format_version(const storage::Version& version, char* out, std::size_t capacity)
{
    std::snprintf(
        out,
        capacity,
        "%llu.%u@%c",
        static_cast<unsigned long long>(hlc::physical(version.hlc)),
        static_cast<unsigned>(hlc::logical(version.hlc)),
        letter(version.origin)
    );
}

bool printable(const std::uint8_t* data, std::size_t length)
{
    for (std::size_t i = 0; i < length; ++i) {
        if (data[i] < 32 || data[i] > 126) {
            return false;
        }
    }

    return true;
}

} // namespace

Console::Console(
    edge::Runtime& runtime,
    sensor::Sampler* sampler,
    WriteFn write,
    void* write_context,
    Hooks hooks
)
    : runtime_(runtime),
      sampler_(sampler),
      write_(write),
      write_context_(write_context),
      hooks_(hooks)
{
}

void Console::printf(const char* format, ...)
{
    if (write_ == nullptr) {
        return;
    }

    char buffer[320];

    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    write_(write_context_, buffer);
}

void Console::print_banner()
{
    printf(
        "\nedge-kv node %c  (4-node ESP-NOW KV store, RF=2)\n"
        "type 'help' for commands\n",
        letter(runtime_.local_node_id())
    );
}

void Console::on_char(char c, std::uint64_t now_ms)
{
    if (c == '\r' || c == '\n') {
        if (line_length_ == 0) {
            return;
        }

        line_[line_length_] = '\0';
        line_length_ = 0;
        execute(line_, now_ms);
        return;
    }

    if (c == '\b' || c == 127) {
        if (line_length_ > 0) {
            --line_length_;
        }
        return;
    }

    if (line_length_ + 1 < sizeof(line_) && c >= 32 && c < 127) {
        line_[line_length_++] = c;
    }
}

void Console::execute(const char* input, std::uint64_t now_ms)
{
    char line[sizeof(line_)];
    std::snprintf(line, sizeof(line), "%s", input);

    char* command = line;
    char* args = split(command);

    if (command[0] == '\0') {
        return;
    }

    printf("> %s%s%s\n", command, args[0] ? " " : "", args);

    if (std::strcmp(command, "help") == 0 || std::strcmp(command, "?") == 0) {
        cmd_help();
    } else if (std::strcmp(command, "status") == 0) {
        cmd_status(now_ms);
    } else if (std::strcmp(command, "members") == 0) {
        cmd_members(now_ms);
    } else if (std::strcmp(command, "ring") == 0) {
        cmd_ring(args, now_ms);
    } else if (std::strcmp(command, "set") == 0) {
        cmd_kv(edge::OpType::SET, args, now_ms);
    } else if (std::strcmp(command, "get") == 0) {
        cmd_kv(edge::OpType::GET, args, now_ms);
    } else if (std::strcmp(command, "del") == 0) {
        cmd_kv(edge::OpType::DEL, args, now_ms);
    } else if (std::strcmp(command, "sensors") == 0) {
        cmd_sensors(now_ms);
    } else if (std::strcmp(command, "sensor") == 0) {
        cmd_sensor(args);
    } else if (std::strcmp(command, "dump") == 0) {
        cmd_dump();
    } else if (std::strcmp(command, "stats") == 0) {
        cmd_stats();
    } else if (std::strcmp(command, "sync") == 0) {
        runtime_.request_sync(now_ms);
        printf("anti-entropy round requested\n");
    } else if (std::strcmp(command, "compact") == 0) {
        runtime_.request_compaction();
        printf("compaction requested\n");
    } else if (std::strcmp(command, "log") == 0) {
        cmd_log(args);
    } else if (std::strcmp(command, "fault") == 0) {
        cmd_fault(args);
    } else if (std::strcmp(command, "stale") == 0) {
        cmd_stale(args);
    } else if (std::strcmp(command, "bench") == 0) {
        cmd_bench(args, now_ms);
    } else if (std::strcmp(command, "format") == 0) {
        if (std::strcmp(args, "yes") != 0) {
            printf("erases this node's EEPROM; type 'format yes'\n");
        } else {
            printf(runtime_.format_storage(now_ms)
                       ? "formatted; peers will push this node's keys back\n"
                       : "format FAILED\n");
        }
    } else if (std::strcmp(command, "reboot") == 0) {
        if (hooks_.reboot != nullptr) {
            printf("rebooting\n");
            hooks_.reboot(hooks_.context);
        } else {
            printf("reboot not available\n");
        }
    } else {
        printf("unknown command '%s' (try 'help')\n", command);
    }
}

void Console::cmd_help()
{
    if (write_ == nullptr) {
        return;
    }

    // Written directly: the text is longer than printf's buffer.
    write_(
        write_context_,
        "status                node state, clock, view, storage summary\n"
        "members               liveness of every node (H1)\n"
        "ring <key>            primary/replica of a key\n"
        "set <key> <value>     distributed SET (value = rest of line)\n"
        "get <key>             distributed GET\n"
        "del <key>             distributed DELETE\n"
        "sensors               read every node's latest sensor value\n"
        "sensor on|off|period <ms>   control this node's sampler\n"
        "dump                  this node's local storage (keys, versions)\n"
        "stats                 counters\n"
        "sync                  start an anti-entropy round now\n"
        "compact               start a storage compaction now\n"
        "bench <n>             n sequential SETs, latency summary\n"
        "log off|info|debug    log verbosity\n"
        "format yes            erase this node's storage\n"
        "reboot                restart this node\n"
    );

#if EDGE_FAULT_INJECTION
    write_(
        write_context_,
        "fault <kind> [n]      kinds: drop-req drop-ack drop-repl drop-repl-ack dup\n"
        "fault loss <percent>  drop that share of every frame sent (until 'fault clear')\n"
        "stale <key> <node>    send an outdated version of key to node (H10)\n"
    );
#endif
}

void Console::cmd_status(std::uint64_t now_ms)
{
    static const char* const BOOT_KINDS[] = {
        "(not started)", "fresh format", "recovered", "recovered (torn tail discarded)"
    };

    char view[5];
    view_text(runtime_.view(now_ms), view);

    const hlc::Hlc now = runtime_.clock().now();
    const storage::Stats stats = runtime_.storage().stats();

    printf(
        "node %c  state %s  view %s  uptime %llus\n"
        "hlc %llu.%u\n"
        "storage %s: %u live, %u tombstones, %u/%u keys, log %u/%u bytes, bank %c, %lu compactions%s\n"
        "pending: %u client, %u replication, %u dedup entries, %u free op slots\n",
        letter(runtime_.local_node_id()),
        edge::to_string(runtime_.state()),
        view,
        static_cast<unsigned long long>(now_ms / 1000),
        static_cast<unsigned long long>(hlc::physical(now)),
        static_cast<unsigned>(hlc::logical(now)),
        BOOT_KINDS[static_cast<std::uint8_t>(stats.boot_kind) & 3u],
        static_cast<unsigned>(stats.live_keys),
        static_cast<unsigned>(stats.tombstones),
        static_cast<unsigned>(stats.slots_used),
        static_cast<unsigned>(storage::MAX_LOGICAL_KEYS),
        static_cast<unsigned>(stats.log_used_bytes),
        static_cast<unsigned>(stats.log_capacity_bytes),
        stats.active_bank == 0 ? 'A' : 'B',
        static_cast<unsigned long>(stats.compactions_completed),
        stats.compaction_active ? " (compacting)" : "",
        static_cast<unsigned>(runtime_.client_pending()),
        static_cast<unsigned>(runtime_.replication_pending()),
        static_cast<unsigned>(runtime_.dedup_active()),
        static_cast<unsigned>(runtime_.free_client_slots())
    );

    if (sampler_ != nullptr) {
        const sensor::SamplerStats& s = sampler_->stats();
        const sensor::Kind kind = sampler_->source().kind();
        char last[64];

        sensor::format_value(kind, s.last_value_milli, s.last_aux, s.last_flags, last, sizeof(last));

        printf(
            "sensor %s (%s): %s, %lu samples, last %s seq %lu, %lu ok, %lu under-replicated, %lu failed, %lu read failures\n",
            sensor::kind_key(kind),
            sampler_->source().simulated() ? "simulated" : "hardware",
            sampler_->enabled() ? "on" : "off",
            static_cast<unsigned long>(s.samples),
            s.samples > 0 ? last : "-",
            static_cast<unsigned long>(s.last_sequence),
            static_cast<unsigned long>(s.writes_ok),
            static_cast<unsigned long>(s.writes_under_replicated),
            static_cast<unsigned long>(s.writes_failed),
            static_cast<unsigned long>(sampler_->source().failures())
        );
    }
}

void Console::cmd_members(std::uint64_t now_ms)
{
    const membership::Manager& members = runtime_.membership();
    const edge::Counters& counters = runtime_.counters();

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        if (id == runtime_.local_node_id()) {
            printf("  %c  self    %s\n", letter(id), edge::to_string(runtime_.state()));
            continue;
        }

        if (!members.seen(id)) {
            printf("  %c  never heard\n", letter(id));
            continue;
        }

        printf(
            "  %c  %-5s   last heard %llu ms ago, %lu heartbeats\n",
            letter(id),
            members.is_alive(id, now_ms) ? "alive" : "DEAD",
            static_cast<unsigned long long>(now_ms - members.last_heard(id)),
            static_cast<unsigned long>(counters.heartbeats_from[id - node::MIN])
        );
    }
}

void Console::cmd_ring(const char* key, std::uint64_t now_ms)
{
    if (key[0] == '\0') {
        printf("usage: ring <key>\n");
        return;
    }

    const std::size_t length = std::strlen(key);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(key);

    placement::Assignment full{};
    placement::Assignment current{};

    runtime_.placement().locate(bytes, length, static_cast<placement::AliveMask>(0x0F), full);
    const bool ok = runtime_.placement().locate(bytes, length, runtime_.view(now_ms), current);

    printf(
        "key '%s' hash %08lx\n  all nodes up : primary %c, replica %c\n  current view : primary %c, replica %c\n",
        key,
        static_cast<unsigned long>(consistent_hash::fnv1a(bytes, length)),
        letter(full.primary),
        letter(full.replica),
        ok ? letter(current.primary) : '-',
        ok ? letter(current.replica) : '-'
    );
}

void Console::cmd_kv(edge::OpType type, const char* input, std::uint64_t now_ms)
{
    char args[sizeof(line_)];
    std::snprintf(args, sizeof(args), "%s", input);

    char* key = args;
    char* value = split(key);

    if (key[0] == '\0' || (type == edge::OpType::SET && value[0] == '\0')) {
        printf(type == edge::OpType::SET ? "usage: set <key> <value>\n" : "usage: %s <key>\n",
               type == edge::OpType::GET ? "get" : "del");
        return;
    }

    const edge::ClientStatus accepted = runtime_.submit(
        type,
        reinterpret_cast<const std::uint8_t*>(key),
        std::strlen(key),
        reinterpret_cast<const std::uint8_t*>(value),
        type == edge::OpType::SET ? std::strlen(value) : 0,
        on_complete,
        this,
        TAG_COMMAND,
        now_ms
    );

    if (accepted != edge::ClientStatus::OK) {
        printf("%s %s: refused (%s)\n", edge::to_string(type), key, edge::to_string(accepted));
    }
}

void Console::cmd_sensors(std::uint64_t now_ms)
{
    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        char key[storage::KEY_MAX + 1];
        const std::size_t length =
            sensor::latest_key(id, sensor::kind_for_node(id), key, sizeof(key));

        const edge::ClientStatus accepted = runtime_.submit(
            edge::OpType::GET,
            reinterpret_cast<const std::uint8_t*>(key),
            length,
            nullptr,
            0,
            on_complete,
            this,
            TAG_SENSOR_BASE + id,
            now_ms
        );

        if (accepted != edge::ClientStatus::OK) {
            printf("%s: refused (%s)\n", key, edge::to_string(accepted));
        }
    }
}

void Console::cmd_sensor(const char* input)
{
    if (sampler_ == nullptr) {
        printf("no sampler on this node\n");
        return;
    }

    char args[64];
    std::snprintf(args, sizeof(args), "%s", input);
    char* value = split(args);

    if (std::strcmp(args, "on") == 0) {
        sampler_->set_enabled(true);
    } else if (std::strcmp(args, "off") == 0) {
        sampler_->set_enabled(false);
    } else if (std::strcmp(args, "period") == 0 && value[0] != '\0') {
        const long period = std::strtol(value, nullptr, 10);

        if (period < 200) {
            printf("period must be >= 200 ms\n");
            return;
        }

        sampler_->set_period(static_cast<std::uint32_t>(period));
    } else {
        printf("usage: sensor on|off|period <ms>\n");
        return;
    }

    printf("sampler %s\n", sampler_->enabled() ? "on" : "off");
}

void Console::print_value(
    const std::uint8_t* key,
    std::size_t key_length,
    const std::uint8_t* value,
    std::size_t value_length
)
{
    sensor::Reading reading{};

    if (sensor::decode(value, value_length, reading)) {
        char text[64];

        sensor::format_value(reading.kind, reading.value_milli, reading.aux, reading.flags, text, sizeof(text));

        printf(
            "%.*s = %s  (seq %lu, sampled at hlc %llu%s)\n",
            static_cast<int>(key_length),
            reinterpret_cast<const char*>(key),
            text,
            static_cast<unsigned long>(reading.sequence),
            static_cast<unsigned long long>(hlc::physical(reading.hlc)),
            (reading.flags & sensor::READING_FLAG_SIMULATED) ? ", simulated" : ""
        );
        return;
    }

    if (printable(value, value_length)) {
        printf(
            "%.*s = \"%.*s\"\n",
            static_cast<int>(key_length),
            reinterpret_cast<const char*>(key),
            static_cast<int>(value_length),
            reinterpret_cast<const char*>(value)
        );
        return;
    }

    char hex[2 * 32 + 4];
    std::size_t used = 0;

    for (std::size_t i = 0; i < value_length && i < 32; ++i) {
        used += static_cast<std::size_t>(std::snprintf(hex + used, sizeof(hex) - used, "%02x", value[i]));
    }

    printf(
        "%.*s = 0x%s%s (%u bytes)\n",
        static_cast<int>(key_length),
        reinterpret_cast<const char*>(key),
        hex,
        value_length > 32 ? "..." : "",
        static_cast<unsigned>(value_length)
    );
}

void Console::on_complete(void* context, std::uint32_t tag, const edge::ClientResult& result)
{
    static_cast<Console*>(context)->handle_result(tag, result);
}

void Console::handle_result(std::uint32_t tag, const edge::ClientResult& result)
{
    if (tag == TAG_BENCH) {
        ++bench_.done;
        bench_.waiting = false;

        if (edge::write_committed(result.status)) {
            ++bench_.ok;
        } else {
            ++bench_.failed;
        }

        bench_.latency_sum += result.latency_ms;

        if (bench_.done == 1 || result.latency_ms < bench_.latency_min) {
            bench_.latency_min = result.latency_ms;
        }

        if (result.latency_ms > bench_.latency_max) {
            bench_.latency_max = result.latency_ms;
        }

        return;
    }

    if (result.type == edge::OpType::GET && result.status == edge::ClientStatus::OK) {
        print_value(result.key, result.key_length, result.value, result.value_length);
        printf("   (served by %c, %u attempt(s), %lu ms)\n",
               letter(result.served_by),
               static_cast<unsigned>(result.attempts),
               static_cast<unsigned long>(result.latency_ms));
        return;
    }

    printf(
        "%s %.*s: %s (served by %c, %u attempt(s), %lu ms)\n",
        edge::to_string(result.type),
        static_cast<int>(result.key_length),
        reinterpret_cast<const char*>(result.key),
        edge::to_string(result.status),
        letter(result.served_by),
        static_cast<unsigned>(result.attempts),
        static_cast<unsigned long>(result.latency_ms)
    );
}

void Console::cmd_dump()
{
    storage::StorageManager& store = runtime_.storage();
    std::size_t shown = 0;

    for (std::size_t slot = 0; slot < storage::StorageManager::slot_capacity(); ++slot) {
        storage::EntryInfo info{};

        if (!store.slot_info(slot, info)) {
            continue;
        }

        ++shown;

        char version[48];
        format_version(info.version, version, sizeof(version));

        if (info.state == storage::State::TOMBSTONE) {
            printf("  %-24.*s TOMBSTONE  v=%s%s\n",
                   static_cast<int>(info.key_len),
                   reinterpret_cast<const char*>(info.key),
                   version,
                   store.drop_pending(slot) ? "  (drop pending)" : "");
            continue;
        }

        std::uint8_t value[storage::VALUE_MAX];
        std::size_t length = 0;

        if (store.read_slot_value(slot, value, sizeof(value), length) != storage::Result::OK) {
            printf("  %-24.*s READ ERROR\n", static_cast<int>(info.key_len), reinterpret_cast<const char*>(info.key));
            continue;
        }

        printf("  v=%s%s  ", version, store.drop_pending(slot) ? " (drop pending)" : "");
        print_value(info.key, info.key_len, value, length);
    }

    const storage::Stats stats = store.stats();

    printf("%u key(s); log %u/%u bytes used\n",
           static_cast<unsigned>(shown),
           static_cast<unsigned>(stats.log_used_bytes),
           static_cast<unsigned>(stats.log_capacity_bytes));
}

void Console::cmd_stats()
{
    const edge::Counters& c = runtime_.counters();
    const ae::Counters& a = runtime_.ae_counters();
    const storage::Stats s = runtime_.storage().stats();

    // One printf per line: the whole block is longer than printf's
    // buffer and would be cut off.
    printf("radio   rx %lu (invalid %lu, spoofed %lu)  tx %lu (failed %lu)\n",
        (unsigned long)c.frames_rx, (unsigned long)c.frames_rx_invalid, (unsigned long)c.frames_rx_spoofed,
        (unsigned long)c.frames_tx, (unsigned long)c.frames_tx_failed);
    printf("hb      tx %lu  rx %lu   membership changes %lu\n",
        (unsigned long)c.heartbeats_tx, (unsigned long)c.heartbeats_rx, (unsigned long)c.membership_changes);
    printf("server  executed %lu  not-ready %lu  dedup replays %lu  suppressed %lu  table-full %lu\n",
        (unsigned long)c.requests_executed, (unsigned long)c.requests_rejected_not_ready,
        (unsigned long)c.dedup_replays, (unsigned long)c.dedup_suppressed, (unsigned long)c.dedup_table_full);
    printf("repl    started %lu  ok %lu  failed %lu  stale %lu   as replica: applied %lu stale %lu\n",
        (unsigned long)c.replications_started, (unsigned long)c.replications_ok,
        (unsigned long)c.replications_failed, (unsigned long)c.replications_stale,
        (unsigned long)c.replicas_applied, (unsigned long)c.replicas_stale);
    printf("ae      rounds %lu  chunks tx %lu rx %lu  tables %lu  pushes %lu  resync tx %lu applied %lu stale %lu\n",
        (unsigned long)a.rounds_started, (unsigned long)a.chunks_sent, (unsigned long)a.chunks_received,
        (unsigned long)a.tables_completed, (unsigned long)a.pushes_queued,
        (unsigned long)c.resync_tx, (unsigned long)c.resync_applied, (unsigned long)c.resync_stale);
    printf("gc      orphans %lu  tombstones %lu  keys dropped %lu\n",
        (unsigned long)a.gc_orphans, (unsigned long)a.gc_tombstones, (unsigned long)s.keys_dropped);
    printf("client  ok %lu  failed %lu  attempts %lu  local %lu\n",
        (unsigned long)c.client_ops_ok, (unsigned long)c.client_ops_failed,
        (unsigned long)c.client_attempts, (unsigned long)c.client_local_fast_path);
    printf("storage errors %lu  compactions %lu (aborted %lu)  faults injected %lu\n",
        (unsigned long)c.storage_errors, (unsigned long)s.compactions_completed,
        (unsigned long)s.compactions_aborted, (unsigned long)c.faults_injected);

    if (hooks_.print_platform_stats != nullptr) {
        hooks_.print_platform_stats(hooks_.context, write_, write_context_);
    }
}

void Console::cmd_log(const char* level)
{
    if (std::strcmp(level, "off") == 0) {
        runtime_.set_log_level(edge::LogLevel::OFF);
    } else if (std::strcmp(level, "info") == 0) {
        runtime_.set_log_level(edge::LogLevel::INFO);
    } else if (std::strcmp(level, "debug") == 0) {
        runtime_.set_log_level(edge::LogLevel::DEBUG);
    } else {
        printf("usage: log off|info|debug\n");
        return;
    }

    printf("log level set\n");
}

void Console::cmd_fault(const char* input)
{
#if EDGE_FAULT_INJECTION
    char args[64];
    std::snprintf(args, sizeof(args), "%s", input);
    char* count_text = split(args);

    const long parsed = count_text[0] ? std::strtol(count_text, nullptr, 10) : 1;
    const std::uint8_t count = static_cast<std::uint8_t>(parsed < 0 ? 0 : (parsed > 255 ? 255 : parsed));

    edge::Faults& faults = runtime_.faults();

    if (std::strcmp(args, "drop-req") == 0) {
        faults.drop_client_requests = count;
    } else if (std::strcmp(args, "drop-ack") == 0) {
        faults.drop_client_responses = count;
    } else if (std::strcmp(args, "drop-repl") == 0) {
        faults.drop_repl_requests = count;
    } else if (std::strcmp(args, "drop-repl-ack") == 0) {
        faults.drop_repl_acks = count;
    } else if (std::strcmp(args, "dup") == 0) {
        faults.duplicate_client_requests = count;
    } else if (std::strcmp(args, "loss") == 0 && count_text[0] != '\0' && parsed >= 0 && parsed <= 100) {
        faults.tx_loss_percent = count;
        printf("frame loss %u %% on every frame this node sends\n", static_cast<unsigned>(count));
        return;
    } else if (std::strcmp(args, "clear") == 0) {
        faults = edge::Faults{};
    } else {
        printf("usage: fault drop-req|drop-ack|drop-repl|drop-repl-ack|dup|clear [n] | fault loss <percent>\n");
        return;
    }

    printf("fault armed\n");
#else
    (void)input;
    printf("fault injection not compiled in (build with -DEDGE_FAULT_INJECTION=1)\n");
#endif
}

void Console::cmd_stale(const char* input)
{
#if EDGE_FAULT_INJECTION
    char args[64];
    std::snprintf(args, sizeof(args), "%s", input);
    char* node_text = split(args);
    const node::Id target = parse_node(node_text);

    if (args[0] == '\0' || target == node::INVALID) {
        printf("usage: stale <key> <node A-D>\n");
        return;
    }

    const char* value = "STALE-VALUE";

    if (runtime_.inject_stale_write(
            target,
            reinterpret_cast<const std::uint8_t*>(args),
            std::strlen(args),
            reinterpret_cast<const std::uint8_t*>(value),
            std::strlen(value),
            hlc::make(1, 0)
        )) {
        printf("stale version of '%s' sent to %c; it must be rejected (check 'stats' there)\n",
               args, letter(target));
    } else {
        printf("send failed\n");
    }
#else
    (void)input;
    (void)parse_node;
    printf("fault injection not compiled in (build with -DEDGE_FAULT_INJECTION=1)\n");
#endif
}

void Console::cmd_bench(const char* args, std::uint64_t now_ms)
{
    const long count = std::strtol(args, nullptr, 10);

    if (count <= 0 || count > 10000) {
        printf("usage: bench <1..10000>\n");
        return;
    }

    bench_ = Bench{};
    bench_.active = true;
    bench_.total = static_cast<std::uint32_t>(count);
    bench_.started_ms = now_ms;

    printf("bench: %ld sequential SETs on keys bench/0..7\n", count);
}

void Console::tick(std::uint64_t now_ms)
{
    if (!bench_.active || bench_.waiting) {
        return;
    }

    if (bench_.issued == bench_.total) {
        if (bench_.done == bench_.total) {
            const std::uint64_t elapsed = now_ms - bench_.started_ms;

            printf(
                "bench: %lu ok, %lu failed in %llu ms; latency min %lu avg %lu max %lu ms\n",
                static_cast<unsigned long>(bench_.ok),
                static_cast<unsigned long>(bench_.failed),
                static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long>(bench_.latency_min),
                static_cast<unsigned long>(bench_.done ? bench_.latency_sum / bench_.done : 0),
                static_cast<unsigned long>(bench_.latency_max)
            );

            bench_.active = false;
        }

        return;
    }

    char key[16];
    char value[24];
    const int key_length = std::snprintf(key, sizeof(key), "bench/%lu", static_cast<unsigned long>(bench_.issued % 8));
    const int value_length = std::snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(bench_.issued));

    if (runtime_.submit(
            edge::OpType::SET,
            reinterpret_cast<const std::uint8_t*>(key),
            static_cast<std::size_t>(key_length),
            reinterpret_cast<const std::uint8_t*>(value),
            static_cast<std::size_t>(value_length),
            on_complete,
            this,
            TAG_BENCH,
            now_ms
        ) == edge::ClientStatus::OK) {
        ++bench_.issued;
        bench_.waiting = true;
    }
}

} // namespace console

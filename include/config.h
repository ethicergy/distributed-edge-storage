#pragma once

/*
 * Single home for every timing and capacity parameter (rule R8).
 *
 * Values are starting points chosen from the protocol design and
 * from the host cluster simulation. Retune them from hardware
 * measurements (docs/HARDWARE_TEST_PLAN.md, "Tuning"), and keep the
 * static_asserts at the bottom of this file green: they encode the
 * ordering rules between the timeouts (audit item 5.4).
 */

#include <cstddef>
#include <cstdint>

namespace config {

// ---------------------------------------------------------------
// Membership
// ---------------------------------------------------------------

constexpr std::uint32_t HEARTBEAT_INTERVAL_MS = 500;

// A peer is dead when nothing was heard for this long.
constexpr std::uint32_t MEMBERSHIP_TIMEOUT_MS = 2000;

// ---------------------------------------------------------------
// Boot / rejoin gate (decision D6)
//
// BOOTING : silent (no heartbeats), listen for peers, merge HLC.
// CATCHUP : broadcast our version table so peers push newer data.
// ACTIVE  : heartbeat, serve as primary/replica.
// ---------------------------------------------------------------

constexpr std::uint32_t BOOT_LISTEN_MS =
    MEMBERSHIP_TIMEOUT_MS + HEARTBEAT_INTERVAL_MS;

constexpr std::uint32_t CATCHUP_DURATION_MS = 3000;

// Number of full version-table broadcasts during CATCHUP.
constexpr std::uint8_t CATCHUP_TABLE_ROUNDS = 2;

// ---------------------------------------------------------------
// Request reliability: client -> primary
// ---------------------------------------------------------------

constexpr std::uint32_t CLIENT_INITIAL_TIMEOUT_MS = 600;
constexpr std::uint32_t CLIENT_MAX_RETRY_TIMEOUT_MS = 1200;
constexpr std::uint8_t CLIENT_MAX_RETRANSMISSIONS = 3;
constexpr std::uint32_t CLIENT_ATTEMPT_DEADLINE_MS = 3000;

// A logical client operation may make several attempts (new
// request identity each time) after NOT_RESPONSIBLE, STALE_VERSION
// or an attempt deadline.
constexpr std::uint8_t CLIENT_MAX_ATTEMPTS = 5;
// Backoff before attempt n is n * this; the sum over all attempts
// (4 s) outlasts a membership timeout, so a client that hit a node
// which just died or rebooted gets a fresh view before giving up.
constexpr std::uint32_t CLIENT_ATTEMPT_BACKOFF_MS = 400;

// ---------------------------------------------------------------
// Request reliability: primary -> replica (REPL_REQ)
// ---------------------------------------------------------------

constexpr std::uint32_t REPL_INITIAL_TIMEOUT_MS = 300;
constexpr std::uint32_t REPL_MAX_RETRY_TIMEOUT_MS = 600;
constexpr std::uint8_t REPL_MAX_RETRANSMISSIONS = 3;
constexpr std::uint32_t REPL_DEADLINE_MS = 1500;

// Worst-case time the primary spends on its own work for one
// request (decode, EEPROM append with verify, response build).
constexpr std::uint32_t PRIMARY_PROCESSING_BUDGET_MS = 500;

// ---------------------------------------------------------------
// Dedup
// ---------------------------------------------------------------

constexpr std::uint32_t DEDUP_RETENTION_MS = 6000;

// Under table pressure, a COMPLETED entry this old may be reused:
// no sender retransmits after its attempt deadline.
constexpr std::uint32_t DEDUP_EVICT_AFTER_MS = CLIENT_ATTEMPT_DEADLINE_MS + 250;

// An IN_PROGRESS entry older than this is reclaimed (leak guard).
constexpr std::uint32_t DEDUP_IN_PROGRESS_TIMEOUT_MS =
    REPL_DEADLINE_MS + PRIMARY_PROCESSING_BUDGET_MS + 1000;

// ---------------------------------------------------------------
// Capacities
// ---------------------------------------------------------------

constexpr std::size_t MAX_PENDING_REQUESTS = 12;
constexpr std::size_t MAX_DEDUP_ENTRIES = 64;
constexpr std::size_t MAX_CLIENT_OPS = 8;
constexpr std::size_t MAX_PENDING_REPLICATIONS = 8;

// ---------------------------------------------------------------
// Anti-entropy
// ---------------------------------------------------------------

constexpr std::uint32_t AE_PERIOD_MS = 10000;

// Gap between two anti-entropy packets (table chunks or pushes),
// so a sync round never floods the radio.
constexpr std::uint32_t AE_PACKET_SPACING_MS = 40;

constexpr std::size_t AE_PUSH_QUEUE_DEPTH = 24;

// A peer's version-table report is used for garbage collection
// only while it is this fresh.
constexpr std::uint32_t AE_REPORT_MAX_AGE_MS = 3 * AE_PERIOD_MS;

// Tombstones are kept at least this long (HLC physical ms) and
// until every node confirmed it (decision D8).
constexpr std::uint64_t TOMBSTONE_GC_MIN_AGE_MS = 10ULL * 60ULL * 1000ULL;

// ---------------------------------------------------------------
// Storage lifecycle
// ---------------------------------------------------------------

// Start a background compaction when the active bank has less
// free log space than this.
constexpr std::size_t COMPACTION_TRIGGER_FREE_BYTES = 3072;

// Also compact early when GC has dropped keys and the index is
// this full (percent).
constexpr std::size_t COMPACTION_INDEX_PRESSURE_PERCENT = 75;

// EEPROM pages cleared per compaction step (64 bytes each, about
// 5 ms per page on the AT24C256).
constexpr std::size_t COMPACTION_CLEAR_PAGES_PER_STEP = 2;

// A storage that fails recovery with CORRUPTED is reformatted and
// repopulated from the replicas by anti-entropy.
constexpr bool STORAGE_AUTO_FORMAT_ON_CORRUPTION = true;

// ---------------------------------------------------------------
// Sensors (simulated sources until the real sensors are wired)
// ---------------------------------------------------------------

// EEPROM wear scales with the write rate. Measured in the 2 h soak
// (test/cluster_test.cpp H13, hottest page): 10 s -> ~3 years to the
// AT24C256's 1M-cycle rating, 30 s -> ~10 years. The console command
// `sensor period <ms>` changes it at run time (demos).
constexpr std::uint32_t SENSOR_PERIOD_MS = 30000;
constexpr std::uint8_t SENSOR_HISTORY_SLOTS = 4;

// Nodes that use their real sensor: bit (id - 1), A = 0x01 ... D = 0x08.
// 0 = every node simulated. Set a node's bit only after its module
// was checked on its own on a board (docs/BRINGUP.md, Phase 4).
constexpr std::uint8_t REAL_SENSOR_NODES = 0x0F;  // all four real (Sat 24): A BH1750, B AM312, C LM393 AO, D DHT11

// A real sensor that fails this many reads in a row is replaced by
// the simulated source (readings flagged simulated) until it reads
// again.
constexpr std::uint8_t SENSOR_FALLBACK_AFTER_FAILURES = 3;

// Sensor pins, ESP32-C3 SuperMini (see sensor_pin_ok below).
// The bridge node A gets the BH1750, whose driver never disables
// interrupts; the DHT11 (interrupts off ~5 ms per read) is on D.
constexpr std::uint8_t PIR_PIN = 4;        // node B, AM312 OUT
constexpr std::uint8_t SOUND_AO_PIN = 0;   // node C, LM393 AO (ADC1_CH0)
constexpr std::uint8_t DHT11_PIN = 4;      // node D, DHT11 module DATA
// Node A's BH1750 shares the EEPROM bus (EEPROM_SDA_PIN/SCL_PIN), ADDR low.
constexpr std::uint8_t BH1750_I2C_ADDRESS = 0x23;

// DHT11: at most 1 Hz. Read by its own task, never by the node task.
constexpr std::uint32_t DHT11_READ_INTERVAL_MS = 2000;
// No good DHT11 read for this long: readings are flagged stale (last
// good value) or error (none since boot). Never replaced by simulated
// values.
constexpr std::uint32_t DHT_STALE_MS = 10000;

// AM312: OUT stays high ~2 s per detection, so polling every 100 ms
// catches each one. After power-on it needs to settle; until then
// readings are flagged settling. Measure on the bench and adjust.
constexpr std::uint32_t PIR_POLL_INTERVAL_MS = 100;
constexpr std::uint32_t PIR_SETTLE_MS = 30000;

// LM393 sound module, AO only (node C, Sat 24; DO not connected). A
// task samples AO every SOUND_SAMPLE_INTERVAL_MS (1 kHz) and takes the
// peak-to-peak swing of each block of SOUND_BLOCK_SAMPLES samples; a
// block is active when its swing exceeds SOUND_ACTIVE_SWING_MV.
constexpr std::uint32_t SOUND_SAMPLE_INTERVAL_MS = 1;
constexpr std::uint32_t SOUND_BLOCK_SAMPLES = 10;
constexpr std::uint32_t SOUND_BLOCK_MS = SOUND_SAMPLE_INTERVAL_MS * SOUND_BLOCK_SAMPLES;
// Tune on the bench (BRINGUP 4.6): above the quiet swing `status`
// shows, below a clap's at 30 cm.
constexpr std::uint16_t SOUND_ACTIVE_SWING_MV = 40;
// ADC1 attenuation, as Arduino's adc_attenuation_t: 0 = 0 dB (~0-750 mV),
// 1 = 2.5 dB (~0-1050), 2 = 6 dB (~0-1300), 3 = 11 dB (~0-2500). AO rests
// below 0.6 V on C (NS, Sat 24): 0 dB, >= 150 mV headroom above rest.
constexpr std::uint8_t SOUND_ADC_ATTENUATION = 0;
// Crossings closer together than this are one sound event (a clap
// rings for tens of ms, in and out of the threshold).
constexpr std::uint32_t SOUND_EVENT_GAP_MS = 150;

// ---------------------------------------------------------------
// Raspberry Pi bridge (decision D7)
// ---------------------------------------------------------------

#ifndef EDGE_BRIDGE_NODE_ID
#define EDGE_BRIDGE_NODE_ID 1
#endif

constexpr std::uint8_t BRIDGE_NODE_ID = EDGE_BRIDGE_NODE_ID;
constexpr std::uint32_t BRIDGE_BAUD = 115200;
constexpr std::uint8_t BRIDGE_UART_RX_PIN = 20;
constexpr std::uint8_t BRIDGE_UART_TX_PIN = 21;
constexpr std::size_t BRIDGE_MAX_INFLIGHT = 4;

// ---------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------

constexpr std::uint8_t EEPROM_SDA_PIN = 8;
constexpr std::uint8_t EEPROM_SCL_PIN = 9;

constexpr std::size_t RADIO_RX_QUEUE_DEPTH = 16;

// ---------------------------------------------------------------
// Ordering rules (audit 5.4)
// ---------------------------------------------------------------

// The primary must be able to answer REPLICATION_FAILED before the
// client gives up on the attempt.
static_assert(
    REPL_DEADLINE_MS + PRIMARY_PROCESSING_BUDGET_MS <
        CLIENT_ATTEMPT_DEADLINE_MS,
    "replication deadline + primary work must fit in one client attempt"
);

// A retry of an attempt must still find its dedup entry.
static_assert(
    DEDUP_RETENTION_MS > CLIENT_ATTEMPT_DEADLINE_MS &&
        DEDUP_EVICT_AFTER_MS > CLIENT_ATTEMPT_DEADLINE_MS &&
        DEDUP_EVICT_AFTER_MS > REPL_DEADLINE_MS,
    "dedup entries must outlive a sender's last retransmission"
);

// An IN_PROGRESS entry may only be reclaimed after the request
// could no longer be running.
static_assert(
    DEDUP_IN_PROGRESS_TIMEOUT_MS >
        REPL_DEADLINE_MS + PRIMARY_PROCESSING_BUDGET_MS,
    "IN_PROGRESS reclaim must not race a live replication"
);

static_assert(
    CLIENT_INITIAL_TIMEOUT_MS < CLIENT_ATTEMPT_DEADLINE_MS &&
        REPL_INITIAL_TIMEOUT_MS < REPL_DEADLINE_MS,
    "first retransmission must happen before the deadline"
);

// Heartbeats must arrive several times per membership timeout.
static_assert(
    MEMBERSHIP_TIMEOUT_MS >= 3 * HEARTBEAT_INTERVAL_MS,
    "membership timeout must cover at least three heartbeats"
);

static_assert(
    CLIENT_ATTEMPT_BACKOFF_MS *
            (CLIENT_MAX_ATTEMPTS * (CLIENT_MAX_ATTEMPTS - 1) / 2) >
        MEMBERSHIP_TIMEOUT_MS + HEARTBEAT_INTERVAL_MS,
    "client attempts must span a membership change"
);

static_assert(
    BOOT_LISTEN_MS > MEMBERSHIP_TIMEOUT_MS,
    "boot listen window must exceed one membership timeout"
);

// SuperMini header GPIOs, minus strapping pins 2/8/9 (8/9 carry I2C),
// USB 18/19 and the bridge UART 20/21.
constexpr bool sensor_pin_ok(std::uint8_t pin)
{
    return pin <= 10 && pin != 2 && pin != 8 && pin != 9;
}

static_assert(
    sensor_pin_ok(DHT11_PIN) && sensor_pin_ok(PIR_PIN) && sensor_pin_ok(SOUND_AO_PIN),
    "sensor pins must avoid strapping pins 2/8/9, USB 18/19 and UART 20/21"
);

static_assert(DHT11_READ_INTERVAL_MS >= 1000, "the DHT11 allows at most one read per second");

static_assert(DHT_STALE_MS > DHT11_READ_INTERVAL_MS, "allow at least one DHT11 read before calling it stale");

static_assert(PIR_POLL_INTERVAL_MS <= 500, "poll several times per ~2 s PIR pulse");

// On the C3 only GPIO0-4 are ADC1 (ADC2 is unusable with the radio on).
static_assert(SOUND_AO_PIN <= 4, "LM393 AO must be on an ADC1 pin (GPIO0-4)");

static_assert(SOUND_ADC_ATTENUATION <= 3, "SOUND_ADC_ATTENUATION is 0..3 (0, 2.5, 6, 11 dB)");

static_assert(
    SOUND_BLOCK_SAMPLES >= 2 && SOUND_BLOCK_MS < SOUND_EVENT_GAP_MS,
    "a swing needs two samples; a block must be shorter than the event gap"
);

static_assert(REAL_SENSOR_NODES <= 0x0F, "REAL_SENSOR_NODES has one bit per node A..D");

static_assert(
    MAX_PENDING_REQUESTS >= MAX_CLIENT_OPS &&
        MAX_PENDING_REQUESTS >= MAX_PENDING_REPLICATIONS,
    "request tables must hold every client op / replication"
);

// Node A's BH1750 shares the EEPROM bus. With ADDR high it would sit at
// 0x5C, inside the EEPROM's 0x58-0x5F ID block (eeprom.h): keep ADDR low.
static_assert(
    (BH1750_I2C_ADDRESS & 0xF0) != 0x50,
    "BH1750 must not use an EEPROM address (0x50-0x57 data, 0x58-0x5F ID block)"
);

// ---------------------------------------------------------------
// EEPROM supply from a GPIO (decision D10, node B only)
// ---------------------------------------------------------------

// Board B's 3V3 header pin is loose, so its EEPROM VCC is fed from a
// GPIO driven high (platformio.ini sets EDGE_EEPROM_POWER_PIN=6 for
// node-b and node-b-fi only). NO_EEPROM_POWER_PIN: VCC on 3V3.
constexpr std::uint8_t NO_EEPROM_POWER_PIN = 0xFF;

#ifdef EDGE_EEPROM_POWER_PIN
constexpr std::uint8_t EEPROM_POWER_PIN = EDGE_EEPROM_POWER_PIN;
#else
constexpr std::uint8_t EEPROM_POWER_PIN = NO_EEPROM_POWER_PIN;
#endif

// Settle time after driving the pin high, before the first I2C access.
constexpr std::uint32_t EEPROM_POWER_SETTLE_MS = 10;

// Same rules as a sensor pin, and it must not be any pin in use.
static_assert(
    EEPROM_POWER_PIN == NO_EEPROM_POWER_PIN ||
        (sensor_pin_ok(EEPROM_POWER_PIN) &&
         EEPROM_POWER_PIN != EEPROM_SDA_PIN && EEPROM_POWER_PIN != EEPROM_SCL_PIN &&
         EEPROM_POWER_PIN != PIR_PIN && EEPROM_POWER_PIN != SOUND_AO_PIN &&
         EEPROM_POWER_PIN != DHT11_PIN),
    "EEPROM power pin must be a free header GPIO (0-10 minus 2/8/9, not I2C or a sensor pin)"
);

// ---------------------------------------------------------------
// Console input guard (BRINGUP_LOG problem 5)
// ---------------------------------------------------------------

// Input is discarded for this long after boot (console::InputGuard):
// the host's tty can echo the board's buffered output back before it
// switches to raw mode.
constexpr std::uint32_t CONSOLE_INPUT_HOLD_MS = 1000;

} // namespace config

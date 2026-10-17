/*
 * Firmware entry point: hardware wiring and task start-up only.
 *
 * Task model (audit 5.10): ONE node task owns all distributed-store
 * state (runtime: HLC, membership, request/dedup tables, storage,
 * anti-entropy) and also runs the sensor sampler, the console and
 * the Pi bridge, so no state is shared between tasks. The ESP-NOW
 * callbacks (Wi-Fi task) only copy frames into a queue (rule R3).
 *
 * Every board runs this same image; only NODE_ID differs.
 */

#include <Arduino.h>
#include <esp_random.h>
#include <esp_system.h>

#include <cstdarg>
#include <cstdio>

#include "bridge.h"
#include "config.h"
#include "console.h"
#include "eeprom.h"
#include "node_runtime.h"
#include "platform_time.h"
#include "sensor.h"
#include "storage.h"
#include "transport.h"

#ifndef NODE_ID
#error "NODE_ID must be defined by PlatformIO (see platformio.ini)"
#endif

#ifndef EDGE_FAULT_INJECTION
#define EDGE_FAULT_INJECTION 0
#endif

namespace {

constexpr node::Id LOCAL_NODE_ID = static_cast<node::Id>(NODE_ID);

static_assert(
    LOCAL_NODE_ID >= node::MIN && LOCAL_NODE_ID <= node::MAX,
    "NODE_ID must be 1..4"
);

constexpr bool BRIDGE_ROLE = LOCAL_NODE_ID == config::BRIDGE_NODE_ID;

constexpr std::uint32_t NODE_TASK_STACK_BYTES = 16384;
constexpr UBaseType_t NODE_TASK_PRIORITY = 3;

// ------------------------------------------------------------------
// Hardware objects
// ------------------------------------------------------------------

class EspNowLink final : public edge::Link {
public:
    explicit EspNowLink(transport::EspNowTransport& transport)
        : transport_(transport)
    {
    }

    bool unicast(node::Id destination, const std::uint8_t* data, std::size_t length) override
    {
        return transport_.send_unicast(destination, data, length) == transport::Result::OK;
    }

    bool broadcast(const std::uint8_t* data, std::size_t length) override
    {
        return transport_.send_broadcast(data, length) == transport::Result::OK;
    }

private:
    transport::EspNowTransport& transport_;
};

transport::EspNowTransport radio;
EspNowLink link(radio);

eeprom::AT24C256 eeprom_chip;
storage::StorageManager store(eeprom_chip);

edge::Runtime runtime(
    LOCAL_NODE_ID,
    store,
    link,
    platform_time::now_ms
);

sensor::SimulatedSource sensor_source(
    sensor::kind_for_node(LOCAL_NODE_ID),
    0xC0FFEEu + LOCAL_NODE_ID
);

sensor::Sampler sampler(runtime, sensor_source);

HardwareSerial& bridge_uart = Serial1;

// ------------------------------------------------------------------
// Output
// ------------------------------------------------------------------

void serial_write(void*, const char* text)
{
    Serial.print(text);
}

void log_line(void*, const char* line)
{
    const std::uint64_t now = platform_time::now_ms();

    Serial.printf(
        "%6lu.%03lu %s\n",
        static_cast<unsigned long>(now / 1000),
        static_cast<unsigned long>(now % 1000),
        line
    );
}

void bridge_write(void*, const std::uint8_t* data, std::size_t length)
{
    bridge_uart.write(data, length);
}

bridge::Bridge pi_bridge(runtime, bridge_write, nullptr);

// ------------------------------------------------------------------
// Console hooks
// ------------------------------------------------------------------

void print_platform_stats(void*, console::WriteFn write, void* write_context)
{
    const transport::Stats radio_stats = radio.stats();
    char line[200];

    std::snprintf(
        line,
        sizeof(line),
        "esp-now rx %lu  queue overflows %lu  unknown MAC %lu\n",
        static_cast<unsigned long>(radio_stats.rx_frames),
        static_cast<unsigned long>(radio_stats.rx_queue_overflows),
        static_cast<unsigned long>(radio_stats.rx_unknown_mac)
    );
    write(write_context, line);

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        if (id == LOCAL_NODE_ID) {
            continue;
        }

        std::snprintf(
            line,
            sizeof(line),
            "esp-now tx -> %c: %lu delivered, %lu failed (MAC-layer ACK)\n",
            static_cast<char>('A' + id - node::MIN),
            static_cast<unsigned long>(radio_stats.tx_status_ok[id - node::MIN]),
            static_cast<unsigned long>(radio_stats.tx_status_failed[id - node::MIN])
        );
        write(write_context, line);
    }

    std::snprintf(
        line,
        sizeof(line),
        "heap free %lu (min %lu)  node task stack headroom %lu bytes\n",
        static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getMinFreeHeap()),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t))
    );
    write(write_context, line);

    if (BRIDGE_ROLE) {
        const bridge::Stats& b = pi_bridge.stats();
        const uart::DecoderStats& d = pi_bridge.decoder_stats();

        std::snprintf(
            line,
            sizeof(line),
            "bridge requests %lu  responses %lu  pings %lu  busy %lu  frames %lu  crc errors %lu  len errors %lu\n",
            static_cast<unsigned long>(b.requests),
            static_cast<unsigned long>(b.responses),
            static_cast<unsigned long>(b.pings),
            static_cast<unsigned long>(b.rejected_busy),
            static_cast<unsigned long>(d.frames),
            static_cast<unsigned long>(d.crc_errors),
            static_cast<unsigned long>(d.length_errors)
        );
        write(write_context, line);
    }
}

void reboot(void*)
{
    Serial.flush();
    delay(50);
    esp_restart();
}

console::Hooks console_hooks()
{
    console::Hooks hooks{};
    hooks.print_platform_stats = print_platform_stats;
    hooks.reboot = reboot;
    return hooks;
}

console::Console operator_console(runtime, &sampler, serial_write, nullptr, console_hooks());

// ------------------------------------------------------------------
// Node task
// ------------------------------------------------------------------

StaticTask_t node_task_control;
StackType_t node_task_stack[NODE_TASK_STACK_BYTES / sizeof(StackType_t)];

void node_task(void*)
{
    transport::ReceivedFrame frame{};
    std::uint8_t bridge_buffer[64];

    for (;;) {
        const std::uint64_t now = platform_time::now_ms();

        while (radio.receive(frame)) {
            runtime.on_frame(frame.data, frame.length, frame.source_node, now);
        }

        radio.drain_send_status();

        while (Serial.available() > 0) {
            operator_console.on_char(static_cast<char>(Serial.read()), now);
        }

        if (BRIDGE_ROLE) {
            int available = bridge_uart.available();

            while (available > 0) {
                const std::size_t chunk =
                    static_cast<std::size_t>(available) < sizeof(bridge_buffer)
                        ? static_cast<std::size_t>(available)
                        : sizeof(bridge_buffer);

                const std::size_t got = bridge_uart.readBytes(bridge_buffer, chunk);
                pi_bridge.on_bytes(bridge_buffer, got, now);
                available = bridge_uart.available();
            }

            pi_bridge.tick(now);
        }

        runtime.tick(now);
        sampler.tick(now);
        operator_console.tick(now);

        // Yield one tick; ESP-NOW frames queue up meanwhile.
        vTaskDelay(1);
    }
}

[[noreturn]] void halt(const char* reason)
{
    for (;;) {
        Serial.printf("FATAL: %s\n", reason);
        delay(5000);
    }
}

} // namespace

void setup()
{
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // Room for a whole `stats` / `dump` answer. With no USB host
    // attached the core discards output instead of blocking, so a
    // board on battery power never stalls on logging.
    Serial.setTxBufferSize(4096);
#endif

    Serial.begin(115200);

    delay(1500);

    Serial.printf(
        "\nedge-kv firmware, node %c%s%s\n",
        static_cast<char>('A' + LOCAL_NODE_ID - node::MIN),
        BRIDGE_ROLE ? " (Pi bridge)" : "",
        EDGE_FAULT_INJECTION ? " [fault injection build]" : ""
    );

    if (radio.begin(LOCAL_NODE_ID) != transport::Result::OK) {
        halt("ESP-NOW initialisation failed");
    }

    if (eeprom_chip.begin(config::EEPROM_SDA_PIN, config::EEPROM_SCL_PIN) != eeprom::Result::OK) {
        halt("AT24C256 not answering on I2C (SDA 8, SCL 9, addr 0x50)");
    }

    runtime.set_log(log_line, nullptr, edge::LogLevel::INFO);

    if (!runtime.begin(platform_time::now_ms(), esp_random())) {
        halt("node runtime failed to start (storage?)");
    }

    sampler.begin();

    if (BRIDGE_ROLE) {
        bridge_uart.begin(
            config::BRIDGE_BAUD,
            SERIAL_8N1,
            config::BRIDGE_UART_RX_PIN,
            config::BRIDGE_UART_TX_PIN
        );

        // Only ever read what is already buffered.
        bridge_uart.setTimeout(2);
    }

    operator_console.print_banner();

    if (xTaskCreateStatic(
            node_task,
            "node",
            NODE_TASK_STACK_BYTES / sizeof(StackType_t),
            nullptr,
            NODE_TASK_PRIORITY,
            node_task_stack,
            &node_task_control
        ) == nullptr) {
        halt("cannot start node task");
    }
}

void loop()
{
    // All work happens in the node task.
    vTaskDelete(nullptr);
}

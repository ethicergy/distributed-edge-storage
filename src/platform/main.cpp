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
#include <driver/gpio.h>
#include <driver/temp_sensor.h>
#include <esp_random.h>
#include <esp_system.h>
#include <hal/usb_serial_jtag_ll.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bridge.h"
#include "config.h"
#include "console.h"
#include "eeprom.h"
#include "node_runtime.h"
#include "platform_time.h"
#include "sensor.h"
#include "sensor_sources.h"
#include "sensors.h"
#include "storage.h"
#include "transport.h"

#ifndef NODE_ID
#error "NODE_ID must be defined by PlatformIO (see platformio.ini)"
#endif

#ifndef EDGE_FAULT_INJECTION
#define EDGE_FAULT_INJECTION 0
#endif

// PlatformIO env name, set in platformio.ini.
#ifndef EDGE_ENV_NAME
#define EDGE_ENV_NAME "unknown"
#endif

namespace {

constexpr node::Id LOCAL_NODE_ID = static_cast<node::Id>(NODE_ID);

static_assert(
    LOCAL_NODE_ID >= node::MIN && LOCAL_NODE_ID <= node::MAX,
    "NODE_ID must be 1..4"
);

constexpr bool BRIDGE_ROLE = LOCAL_NODE_ID == config::BRIDGE_NODE_ID;

constexpr std::uint32_t NODE_TASK_STACK_BYTES = 16384;
constexpr int USB_TX_BUFFER_BYTES = 4096;
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

constexpr bool REAL_SENSOR = sensor::real_sensor_enabled(LOCAL_NODE_ID);

sensor::SimulatedSource simulated_sensor(
    sensor::kind_for_node(LOCAL_NODE_ID),
    0xC0FFEEu + LOCAL_NODE_ID
);

// config::REAL_SENSOR_NODES picks the real driver; the simulated source
// stands in while it is off or failing.
sensor::FallbackSource sensor_source(
    REAL_SENSOR ? platform_sensors::real_source(LOCAL_NODE_ID) : nullptr,
    simulated_sensor
);

sensor::Sampler sampler(runtime, sensor_source);

HardwareSerial& bridge_uart = Serial1;

// ------------------------------------------------------------------
// Output
// ------------------------------------------------------------------

// Problem 9: when the console last printed (see nudge_usb_tx).
std::uint64_t usb_last_write_ms = 0;

void serial_write(void*, const char* text)
{
    Serial.print(text);
    usb_last_write_ms = platform_time::now_ms();
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
    usb_last_write_ms = now;
}

void bridge_write(void*, const std::uint8_t* data, std::size_t length)
{
    bridge_uart.write(data, length);
}

bridge::Bridge pi_bridge(runtime, bridge_write, nullptr);

// ------------------------------------------------------------------
// Board identity (boot banner and `stats`)
// ------------------------------------------------------------------

char node_letter(node::Id id)
{
    return static_cast<char>('A' + id - node::MIN);
}

const char* reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external reset pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "PANIC (exception or abort)";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "unknown";
    }
}

void format_mac(const std::uint8_t* mac, char (&out)[18])
{
    std::snprintf(
        out,
        sizeof(out),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
}

// Env, build time, reset reason, and whether this board's MAC is the
// one the static table (transport.cpp) expects for NODE_ID.
void print_identity(console::WriteFn write, void* write_context)
{
    char line[200];

    std::snprintf(
        line,
        sizeof(line),
        "env %s, built %s %s, reset reason: %s\n",
        EDGE_ENV_NAME,
        __DATE__,
        __TIME__,
        reset_reason_name(esp_reset_reason())
    );
    write(write_context, line);

    if (config::EEPROM_POWER_PIN != config::NO_EEPROM_POWER_PIN) {
        std::snprintf(
            line,
            sizeof(line),
            "EEPROM power: GPIO%u (workaround D10, VCC not on 3V3)\n",
            static_cast<unsigned>(config::EEPROM_POWER_PIN)
        );
        write(write_context, line);
    }

    std::uint8_t own_mac[transport::MAC_ADDRESS_SIZE] = {};
    esp_read_mac(own_mac, ESP_MAC_WIFI_STA);

    char own_text[18];
    format_mac(own_mac, own_text);

    const std::uint8_t* expected = transport::configured_mac(LOCAL_NODE_ID);

    if (expected != nullptr &&
        std::memcmp(own_mac, expected, transport::MAC_ADDRESS_SIZE) == 0) {
        std::snprintf(
            line,
            sizeof(line),
            "MAC %s matches the table entry for node %c\n",
            own_text,
            node_letter(LOCAL_NODE_ID)
        );
        write(write_context, line);
        return;
    }

    char expected_text[18] = "none";

    if (expected != nullptr) {
        format_mac(expected, expected_text);
    }

    std::snprintf(
        line,
        sizeof(line),
        "\n!!!!!!!! MAC MISMATCH !!!!!!!!\n"
        "!!! this board is %s, the table entry for node %c is %s\n",
        own_text,
        node_letter(LOCAL_NODE_ID),
        expected_text
    );
    write(write_context, line);

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        const std::uint8_t* mac = transport::configured_mac(id);

        if (mac != nullptr && std::memcmp(own_mac, mac, transport::MAC_ADDRESS_SIZE) == 0) {
            std::snprintf(
                line,
                sizeof(line),
                "!!! this MAC is node %c's entry: flash env node-%c instead\n",
                node_letter(id),
                static_cast<char>('a' + id - node::MIN)
            );
            write(write_context, line);
        }
    }

    write(
        write_context,
        "!!! peers drop frames from unknown MACs: fix src/platform/transport.cpp or reflash\n\n"
    );
}

// ------------------------------------------------------------------
// Console hooks
// ------------------------------------------------------------------

void print_platform_stats(void*, console::WriteFn write, void* write_context)
{
    const transport::Stats radio_stats = radio.stats();
    char line[200];

    print_identity(write, write_context);

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

    std::uint16_t hottest_page = 0;
    const std::uint32_t hottest = eeprom_chip.hottest_page_writes(hottest_page);

    std::snprintf(
        line,
        sizeof(line),
        "eeprom page writes since boot: total %lu, hottest page %u with %lu (rated 1000000)\n",
        static_cast<unsigned long>(eeprom_chip.total_page_writes()),
        static_cast<unsigned>(hottest_page),
        static_cast<unsigned long>(hottest)
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

// ESP32-C3 internal temperature sensor: the die temperature, well
// above ambient and rising with radio load. Started once at boot, read
// on demand by `status` and `stats` (a few microseconds, no bus).
bool chip_temp_started = false;

void start_chip_temp()
{
    temp_sensor_config_t config = TSENS_CONFIG_DEFAULT();
    chip_temp_started = temp_sensor_set_config(config) == ESP_OK && temp_sensor_start() == ESP_OK;
}

bool read_chip_temp(void*, float& celsius)
{
    return chip_temp_started && temp_sensor_read_celsius(&celsius) == ESP_OK;
}

console::Hooks console_hooks()
{
    console::Hooks hooks{};
    hooks.print_platform_stats = print_platform_stats;
    hooks.reboot = reboot;
    hooks.read_chip_temp = read_chip_temp;
    return hooks;
}

console::Console operator_console(runtime, &sampler, serial_write, nullptr, console_hooks());

// ------------------------------------------------------------------
// Node task
// ------------------------------------------------------------------

console::InputGuard input_guard(operator_console);

// BRINGUP_LOG problem 9: a short reply could stay on the board until the
// next input arrived, seconds later. 20 ms after the last output, flush
// the 64-byte hardware FIFO once (a partial packet is otherwise not
// sent; this is what fixed it on the boards), and re-enable the "host
// took the data" interrupt so the core's ISR sends what waits in its
// buffer. The interrupt only while at least half the buffer is free:
// with the port closed the buffer fills, and a "connected" core would
// block each write for up to 100 ms.
void nudge_usb_tx(std::uint64_t now_ms)
{
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    static std::uint64_t flushed_for_ms = 0;

    // 20 ms after the last output: push out a partial packet still in
    // the 64-byte hardware FIFO, once per burst of output.
    if (usb_last_write_ms != 0 && usb_last_write_ms != flushed_for_ms &&
        now_ms - usb_last_write_ms >= 20) {
        usb_serial_jtag_ll_txfifo_flush();
        flushed_for_ms = usb_last_write_ms;
    }

    const int free_bytes = Serial.availableForWrite();

    if (free_bytes < USB_TX_BUFFER_BYTES && free_bytes >= USB_TX_BUFFER_BYTES / 2) {
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    }
#endif
}

StaticTask_t node_task_control;
StackType_t node_task_stack[NODE_TASK_STACK_BYTES / sizeof(StackType_t)];

void node_task(void*)
{
    transport::ReceivedFrame frame{};
    std::uint8_t bridge_buffer[64];

    // Characters that arrived during setup() may be echoed boot text.
    input_guard.begin(platform_time::now_ms());

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
        nudge_usb_tx(now);

        // Yield one tick; ESP-NOW frames queue up meanwhile.
        vTaskDelay(1);
    }
}

// D10: on node B the EEPROM's VCC comes from a GPIO. Drive it high at
// full strength before the first I2C access. Nothing reconfigures the
// pin afterwards (config.h checks it is no I2C or sensor pin).
void power_eeprom()
{
    if (config::EEPROM_POWER_PIN == config::NO_EEPROM_POWER_PIN) {
        return;
    }

    const auto pin = static_cast<gpio_num_t>(config::EEPROM_POWER_PIN);

    gpio_reset_pin(pin);
    gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);

    delay(config::EEPROM_POWER_SETTLE_MS);
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
    Serial.setTxBufferSize(USB_TX_BUFFER_BYTES);
#endif

    Serial.begin(115200);

    delay(1500);

    Serial.printf(
        "\nedge-kv firmware, node %c%s%s\n",
        static_cast<char>('A' + LOCAL_NODE_ID - node::MIN),
        BRIDGE_ROLE ? " (Pi bridge)" : "",
        EDGE_FAULT_INJECTION ? " [fault injection build]" : ""
    );

    print_identity(serial_write, nullptr);

    if (radio.begin(LOCAL_NODE_ID) != transport::Result::OK) {
        halt("ESP-NOW initialisation failed");
    }

    power_eeprom();

    if (eeprom_chip.begin(config::EEPROM_SDA_PIN, config::EEPROM_SCL_PIN) != eeprom::Result::OK) {
        halt("AT24C256 not answering on I2C (SDA 8, SCL 9, addr 0x50)");
    }

    runtime.set_log(log_line, nullptr, edge::LogLevel::INFO);

    if (!runtime.begin(platform_time::now_ms(), esp_random())) {
        halt("node runtime failed to start (storage?)");
    }

    // After the EEPROM: the BH1750 shares its I2C bus.
    sampler.begin();

    Serial.printf(
        "sensor %s: %s\n",
        sensor::kind_key(sensor_source.kind()),
        sensor_source.real_enabled() ? "hardware"
            : REAL_SENSOR ? "hardware did not start, using the SIMULATED source"
            : "simulated (REAL_SENSOR_NODES)"
    );

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

    start_chip_temp();
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

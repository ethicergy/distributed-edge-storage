#pragma once

#include <cstddef>
#include <cstdint>

#include "node_runtime.h"
#include "sensor.h"

/*
 * Line-based operator console (USB serial on every node).
 *
 * Hardware tests are run by typing commands instead of flashing
 * special firmware. `help` lists the commands. Client commands are
 * asynchronous: the answer is printed when the operation completes.
 */
namespace console {

using WriteFn = void (*)(void* context, const char* text);

// Platform-specific extras (firmware: radio queue counters, reboot).
struct Hooks {
    void (*print_platform_stats)(void* context, WriteFn write, void* write_context) = nullptr;
    void (*reboot)(void* context) = nullptr;
    void* context = nullptr;
};

class Console {
public:
    Console(
        edge::Runtime& runtime,
        sensor::Sampler* sampler,
        WriteFn write,
        void* write_context,
        Hooks hooks = Hooks{}
    );

    // Feed received characters (handles CR/LF and backspace).
    void on_char(char c, std::uint64_t now_ms);

    // Discard received characters, and any partial line, until
    // now_ms + duration_ms (BRINGUP_LOG problem 5: a host opening the
    // port can echo the board's own output back as input).
    void hold_input(std::uint64_t now_ms, std::uint32_t duration_ms);

    // Execute one command line.
    void execute(const char* line, std::uint64_t now_ms);

    // Drives `bench`.
    void tick(std::uint64_t now_ms);

    void print_banner();

private:
    struct Bench {
        bool active = false;
        std::uint32_t total = 0;
        std::uint32_t issued = 0;
        std::uint32_t done = 0;
        std::uint32_t ok = 0;
        std::uint32_t failed = 0;
        std::uint64_t latency_sum = 0;
        std::uint32_t latency_min = 0;
        std::uint32_t latency_max = 0;
        std::uint64_t started_ms = 0;
        bool waiting = false;
    };

    enum Tag : std::uint32_t {
        TAG_COMMAND = 1,
        TAG_SENSOR_BASE = 0x100,
        TAG_BENCH = 0x200
    };

    static void on_complete(void* context, std::uint32_t tag, const edge::ClientResult& result);
    void handle_result(std::uint32_t tag, const edge::ClientResult& result);

    void printf(const char* format, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;

    void cmd_help();
    void cmd_status(std::uint64_t now_ms);
    void cmd_members(std::uint64_t now_ms);
    void cmd_ring(const char* key, std::uint64_t now_ms);
    void cmd_kv(edge::OpType type, const char* args, std::uint64_t now_ms);
    void cmd_sensors(std::uint64_t now_ms);
    void cmd_dump();
    void cmd_stats();
    void cmd_log(const char* level);
    void cmd_fault(const char* args);
    void cmd_stale(const char* args);
    void cmd_bench(const char* args, std::uint64_t now_ms);
    void cmd_sensor(const char* args);

    void print_value(const std::uint8_t* key, std::size_t key_length,
                     const std::uint8_t* value, std::size_t value_length);

    edge::Runtime& runtime_;
    sensor::Sampler* sampler_;
    WriteFn write_;
    void* write_context_;
    Hooks hooks_;

    char line_[256]{};
    std::size_t line_length_ = 0;

    std::uint64_t hold_until_ms_ = 0;
    std::uint32_t held_chars_ = 0;

    Bench bench_{};
};

// When to hold console input (BRINGUP_LOG problem 5): at boot, and when
// the USB host comes back (cable plugged in again while the board
// stayed powered). Never in the middle of a session: a pause in the
// output does not mean the port was reopened.
class InputGuard {
public:
    explicit InputGuard(Console& console);

    void begin(std::uint64_t now_ms);

    // usb_plugged: a USB host is sending frames (HWCDC::isPlugged()).
    void poll(bool usb_plugged, std::uint64_t now_ms);

private:
    Console& console_;
    bool plugged_ = true;
};

} // namespace console

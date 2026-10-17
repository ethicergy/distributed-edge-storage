/*
 * Real sensor drivers (D1), one per node, behind config::REAL_SENSOR_NODES.
 *
 *   A  DHT11 module      DATA -> config::DHT11_PIN     own task, 1 read / 2 s
 *   B  AM312 PIR         OUT  -> config::PIR_PIN       polled by MotionSource
 *   C  LM393 sound (DO)  DO   -> config::SOUND_DO_PIN  ISR edge counter
 *   D  BH1750 (GY-302)   I2C 0x23 on the EEPROM bus    continuous high-res
 *
 * The logic (windows, settling, events, keep-last-good, fallback) is in
 * src/sensor_sources.cpp and tested on the host; this file only touches
 * the hardware. Nothing here runs unless the node's bit is set.
 */

#include "sensors.h"

#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>

#include <cmath>

#include "config.h"
#include "i2c_bus.h"
#include "platform_time.h"
#include "sensor_sources.h"

namespace platform_sensors {

namespace {

// ------------------------------------------------------------------
// DHT11 (node A): read in its own low-priority task, so the node task
// (radio, storage, console) never waits for it. One read takes about
// 25 ms: an 18 ms start pulse (the task sleeps) and ~5 ms with
// interrupts off while the 40 bits arrive.
// ------------------------------------------------------------------

constexpr std::uint32_t DHT_TASK_STACK_BYTES = 3072;
constexpr UBaseType_t DHT_TASK_PRIORITY = 1; // below the node task (3)
constexpr std::uint32_t DHT_POWER_UP_MS = 1500;

class Dht11Input final : public sensor::ValueInput {
public:
    bool begin() override
    {
        dht_.begin();

        task_ = xTaskCreateStatic(
            task_entry,
            "dht11",
            DHT_TASK_STACK_BYTES / sizeof(StackType_t),
            this,
            DHT_TASK_PRIORITY,
            stack_,
            &task_control_
        );

        return task_ != nullptr;
    }

    sensor::ValueAttempts take() override
    {
        portENTER_CRITICAL(&lock_);
        const sensor::ValueAttempts out = attempts_;
        attempts_ = sensor::ValueAttempts{};
        portEXIT_CRITICAL(&lock_);
        return out;
    }

private:
    static void task_entry(void* context)
    {
        static_cast<Dht11Input*>(context)->run();
    }

    [[noreturn]] void run()
    {
        vTaskDelay(pdMS_TO_TICKS(DHT_POWER_UP_MS));

        for (;;) {
            // force = true: the library's own 2 s cache is not needed,
            // this loop already keeps to DHT11_READ_INTERVAL_MS.
            const float celsius = dht_.readTemperature(false, true);

            portENTER_CRITICAL(&lock_);

            if (std::isnan(celsius)) {
                ++attempts_.failed;
            } else {
                ++attempts_.ok;
                attempts_.last_good_milli = static_cast<std::int32_t>(std::lround(celsius * 1000.0f));
                attempts_.last_good_ms = platform_time::now_ms();
            }

            portEXIT_CRITICAL(&lock_);

            vTaskDelay(pdMS_TO_TICKS(config::DHT11_READ_INTERVAL_MS));
        }
    }

    DHT dht_{config::DHT11_PIN, DHT11};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    sensor::ValueAttempts attempts_{};

    TaskHandle_t task_ = nullptr;
    StaticTask_t task_control_{};
    StackType_t stack_[DHT_TASK_STACK_BYTES / sizeof(StackType_t)]{};
};

// ------------------------------------------------------------------
// AM312 (node B): push-pull OUT at 3.3 V; the pull-down keeps an
// unplugged sensor at "no motion".
// ------------------------------------------------------------------

class PirInput final : public sensor::DigitalInput {
public:
    bool begin() override
    {
        pinMode(config::PIR_PIN, INPUT_PULLDOWN);
        return true;
    }

    bool level() override { return digitalRead(config::PIR_PIN) == HIGH; }
};

// ------------------------------------------------------------------
// LM393 DO (node C): the ISR only counts edges into the active level.
// The counter has one writer (this ISR); aligned 32-bit loads are
// atomic on the C3, and readers only take differences.
// ------------------------------------------------------------------

volatile std::uint32_t sound_edges = 0;

void IRAM_ATTR on_sound_edge()
{
    sound_edges = sound_edges + 1;
}

class SoundInput final : public sensor::DigitalInput {
public:
    bool begin() override
    {
        // DO is an open-collector output on most boards (with its own
        // pull-up); the internal pull-up covers boards without one.
        pinMode(config::SOUND_DO_PIN, INPUT_PULLUP);
        attachInterrupt(
            digitalPinToInterrupt(config::SOUND_DO_PIN),
            on_sound_edge,
            config::SOUND_ACTIVE_HIGH ? RISING : FALLING
        );
        return true;
    }

    bool level() override { return digitalRead(config::SOUND_DO_PIN) == HIGH; }

    std::uint32_t edges() override { return sound_edges; }
};

// ------------------------------------------------------------------
// BH1750 (node D) on the EEPROM bus, continuous high-resolution mode:
// a read is one 2-byte transfer (~0.1 ms at 400 kHz), under the bus
// lock (i2c_bus.h).
// ------------------------------------------------------------------

constexpr std::uint8_t BH1750_POWER_ON = 0x01;
constexpr std::uint8_t BH1750_CONTINUOUS_HIGH_RES = 0x10;

class Bh1750Source final : public sensor::Source {
public:
    bool begin() override
    {
        i2c_bus::Lock lock;
        return command(BH1750_POWER_ON) && command(BH1750_CONTINUOUS_HIGH_RES);
    }

    sensor::Kind kind() const override { return sensor::Kind::LIGHT; }
    bool simulated() const override { return false; }

    bool read(std::uint64_t, sensor::Sample& sample) override
    {
        std::uint16_t raw = 0;

        {
            i2c_bus::Lock lock;

            if (Wire.requestFrom(config::BH1750_I2C_ADDRESS, static_cast<std::uint8_t>(2)) != 2) {
                ++failures_;
                return false;
            }

            raw = static_cast<std::uint16_t>(Wire.read() << 8);
            raw = static_cast<std::uint16_t>(raw | Wire.read());
        }

        // lux = raw / 1.2 (datasheet, MTreg default).
        sample = sensor::Sample{};
        sample.value_milli = static_cast<std::int32_t>(static_cast<std::uint32_t>(raw) * 10000u / 12u);
        return true;
    }

    std::uint32_t failures() const override { return failures_; }

private:
    static bool command(std::uint8_t opcode)
    {
        Wire.beginTransmission(config::BH1750_I2C_ADDRESS);
        Wire.write(opcode);
        return Wire.endTransmission(true) == 0;
    }

    std::uint32_t failures_ = 0;
};

} // namespace

sensor::Source* real_source(node::Id id)
{
    // Constructed on first use only: a node whose bit is clear never
    // touches these pins.
    switch (id) {
        case node::A: {
            static Dht11Input dht;
            static sensor::LatestValueSource source(sensor::Kind::TEMPERATURE, dht);
            return &source;
        }
        case node::B: {
            static PirInput pir;
            static sensor::MotionSource source(pir);
            return &source;
        }
        case node::C: {
            static SoundInput mic;
            static sensor::SoundSource source(mic);
            return &source;
        }
        case node::D: {
            static Bh1750Source source;
            return &source;
        }
        default:
            return nullptr;
    }
}

} // namespace platform_sensors
